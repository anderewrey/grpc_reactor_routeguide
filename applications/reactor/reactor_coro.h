///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <grpcpp/grpcpp.h>

#include <cassert>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client.h"

/// The awaitables of the coroutine level: each wraps one generic reactor of reactor_client.h, and lets a
/// coroutine await what the reactor's callbacks report. It knows the generic reactors and the task core,
/// not EventLoop, and not RouteGuide: an awaitable is a template on the service-specific reactor class, and
/// constructs it from the arguments its constructor takes.
///
/// Every awaitable keeps the level's boundary rule: its callbacks, on a gRPC thread, only record what they
/// were given and hand the suspended coroutine to the executor. The coroutine always resumes on the
/// executor's thread, the application thread.
namespace RpcReactor::coro {

/// How long an awaitable waits for its reactor's OnDone(), once the RPC is cancelled or has ended, before it
/// gives up and leaks the reactor rather than freeing it under gRPC.
inline constexpr std::chrono::seconds kOnDoneTimeout{5};

/// Destroys a reactor once its OnDone() has run, cancelling its RPC first. Should OnDone() never come, the
/// reactor is leaked rather than freed under gRPC.
/// @param reactor the awaitable's reactor, empty once destroyed; already empty is fine
/// @param timeout how long to wait for OnDone()
/// @return false when the reactor was leaked
template <class ReactorT>
bool DestroyAfterOnDone(std::unique_ptr<ReactorT>& reactor, std::chrono::milliseconds timeout) {
  if (!reactor) return true;
  if (reactor->CancelAndWait(timeout)) {
    reactor.reset();
    return true;
  }
  static_cast<void>(reactor.release());
  return false;
}

/// What an awaited unary RPC ends with: its status, and its response, owned by the coroutine.
/// @tparam ResponseT the RPC's response message
template <class ResponseT>
struct UnaryResult {
  grpc::Status status;  ///< The RPC's outcome
  /// The response, empty unless the status is OK: the status is what says whether its content is meaningful.
  ResponseT response;
};

/// A unary RPC a coroutine awaits: the request is sent on construction, and `co_await Finish()` resumes the
/// coroutine on the application thread once the RPC has ended, with its status and its response.
///
///     coro::Unary<routeguide::GetFeature::ClientReactor<Delivery::kPull>> call{executor, stub, point};
///     auto [status, feature] = co_await call.Finish();
///
/// The response is always owned by the coroutine, as a ResponseT under both deliveries. Delivery decides only
/// how it becomes owned:
///
/// | Delivery | How                                                                  |
/// |----------|----------------------------------------------------------------------|
/// | kPull    | Swapped out of the reactor by GetResponse()                          |
/// | kPush    | Moved out of the message the done callback hands over, with no copy  |
///
/// The reactor lives in the Unary, so in the coroutine's frame. Finish() destroys it on the application
/// thread once its OnDone() has run. A Unary destroyed before, such as in a frame destroyed while suspended,
/// cancels the RPC and destroys the reactor once OnDone() has run, or leaks it should OnDone() never come.
///
/// Non-copyable and non-movable: the reactor's callback refers to it.
/// @tparam ReactorT a service-specific reactor, deriving from Client::ActiveUnaryReactor, and constructed
///         from a stub, a context, a request and the unary callbacks
template <class ReactorT>
class Unary {
  /// The response and the delivery of the generic unary reactor ReactorT derives from.
  template <class ResponseT_, Client::Delivery kDelivery_>
  struct Traits_ {
    using ResponseT = ResponseT_;
    static constexpr Client::Delivery kDelivery = kDelivery_;
  };
  template <class ResponseT_, Client::Delivery kDelivery_>
  static Traits_<ResponseT_, kDelivery_> TraitsOf(const Client::ActiveUnaryReactor<ResponseT_, kDelivery_>*);

  using Traits = decltype(TraitsOf(static_cast<ReactorT*>(nullptr)));
  using ResponseT = Traits::ResponseT;
  static constexpr Client::Delivery kDelivery = Traits::kDelivery;
  static constexpr bool kPull = kDelivery == Client::Delivery::kPull;

 public:
  /// What Finish() returns.
  using ResultT = UnaryResult<ResponseT>;

  /// Constructs the reactor, which sends the request at once.
  /// @param executor resumes the awaiting coroutine on the application thread, and must outlive this object
  /// @param stub the service stub, which must outlive the RPC
  /// @param request to send, serialized before this constructor returns
  /// @param context of the RPC, for a deadline or metadata: a fresh one by default
  template <class StubT, class RequestT>
  Unary(Executor& executor, StubT& stub, const RequestT& request,
        std::unique_ptr<grpc::ClientContext> context = std::make_unique<grpc::ClientContext>())
      : executor_(executor),
        reactor_(std::make_unique<ReactorT>(stub, std::move(context), request, MakeCallbacks())) {}

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run.
  ~Unary() {
    handover_.Abandon();
    DestroyAfterOnDone(reactor_, kOnDoneTimeout);
  }

  Unary(const Unary&) = delete;
  Unary& operator=(const Unary&) = delete;
  Unary(Unary&&) = delete;
  Unary& operator=(Unary&&) = delete;

  /// Sends a best-effort cancel. Callable from the application thread at any time: an RPC still in flight
  /// then ends CANCELLED, which the awaited Finish() returns.
  void TryCancel() {
    if (reactor_) reactor_->TryCancel();
  }

  /// Awaited exactly once: resumes on the application thread once the RPC has ended.
  /// @return an awaitable whose co_await gives the ResultT
  [[nodiscard]] auto Finish() noexcept {
    struct Awaiter {
      Unary& call;
      static bool await_ready() noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> waiter) const noexcept { return call.handover_.Suspend(waiter); }
      ResultT await_resume() const { return call.Take(); }
    };
    assert(reactor_ && "Finish() is awaited exactly once");
    return Awaiter{*this};
  }

 private:
  using Callbacks = Client::ActiveUnaryCallbacks<ResponseT, kDelivery>;

  /// The level's own code, on the gRPC thread: records the response under kPush, then hands the coroutine
  /// to the executor. Under kPull the response and the status stay in the reactor.
  Callbacks MakeCallbacks() {
    Callbacks cbs;
    if constexpr (kPull) {
      cbs.done = [this](auto*, const grpc::Status&, const ResponseT&) { handover_.Complete(executor_); };
    } else {
      cbs.done = [this](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
        pushed_ = std::move(response);
        handover_.Complete(executor_);
      };
    }
    return cbs;
  }

  /// On the application thread, once the RPC has ended: takes the outcome out of the reactor, then destroys
  /// the reactor once its OnDone() has returned.
  ResultT Take() {
    handover_.Resumed();
    ResultT result{reactor_->Status(), {}};
    if constexpr (kPull) {
      static_cast<void>(reactor_->GetResponse(result.response));  // false on a failed RPC: left empty
    } else {
      result.response = std::move(*pushed_);  // never null: the reactor hands its message over even on a failure
    }
    // OnDone() may still be returning, on its gRPC thread, from the callback that resumed this coroutine.
    DestroyAfterOnDone(reactor_, kOnDoneTimeout);
    return result;
  }

  Executor& executor_;
  ResumeHandover handover_;
  std::unique_ptr<ResponseT> pushed_;  // Delivery::kPush only, set by the done callback
  std::unique_ptr<ReactorT> reactor_;  // declared last, so the callback's targets exist before the RPC starts
};

/// Which buffer a Reader puts between its reactor and the coroutine. It is never named by the application: the
/// reactor's Delivery and ReadPacing choose it, see BufferFor().
enum class Buffer {
  /// ReadPacing::kTurnByTurn, either delivery: one message at most is in flight, and the stream waits.
  kNone,
  /// Delivery::kPush with ReadPacing::kContinuous: every message is queued, and the stream never waits.
  kUnbounded,
};

/// The buffer of the reader over a reactor of this Delivery and ReadPacing: the one place that maps a reactor
/// configuration to a reader. A combination with no reader gives nothing, and a Reader over it does not compile.
/// The latest-only buffer (kPull, kContinuous) and the bounded one (kPush, kPerMessage) are to come here.
/// @return the buffer, or nothing when no reader takes that combination
constexpr std::optional<Buffer> BufferFor(Client::Delivery delivery, Client::ReadPacing pacing) noexcept {
  if (pacing == Client::ReadPacing::kTurnByTurn) return Buffer::kNone;
  if (delivery == Client::Delivery::kPush && pacing == Client::ReadPacing::kContinuous) return Buffer::kUnbounded;
  return std::nullopt;
}

/// The buffer of the reader over a reactor: BufferFor() its Delivery and ReadPacing, deduced from the generic
/// reading reactor it derives from. Called with a null pointer of the reactor's type, at compile time.
/// @return the buffer, or nothing when no reader takes the reactor's combination
template <class ResponseT, Client::Delivery kDelivery, Client::ReadPacing kPacing>
constexpr std::optional<Buffer> BufferOf(const Client::ActiveReadReactor<ResponseT, kDelivery, kPacing>* /*unused*/) {
  return BufferFor(kDelivery, kPacing);
}
/// Any other type is no reading reactor, so no reader takes it.
/// @return nothing
constexpr std::optional<Buffer> BufferOf(const void* /*not_a_reactor*/) { return std::nullopt; }

/// A server-streaming RPC a coroutine reads: the request is sent on construction, `co_await Read()` resumes the
/// coroutine on the application thread with the next message, owned, or with nothing once the stream has ended,
/// and `co_await Finish()` then gives the RPC's status. The reactor's Delivery and ReadPacing choose the buffer
/// between the reactor and the coroutine, see BufferFor(), and each buffer is a specialization below. This
/// primary template is the combinations with none, and does not compile.
/// @tparam ReactorT a service-specific reading reactor
template <class ReactorT>
class Reader {
  static_assert(BufferOf(static_cast<ReactorT*>(nullptr)).has_value(),
                "coro::Reader takes a reading reactor under ReadPacing::kTurnByTurn (no buffer), or under "
                "Delivery::kPush and ReadPacing::kContinuous (the unbounded buffer)");
};

/// The reader without a buffer, for a server-streaming RPC a coroutine reads one message at a time: the request
/// is sent on construction, `co_await Read()` resumes the coroutine on the application thread with the next
/// message, owned, or with nothing once the stream has ended, and `co_await Finish()` then gives the RPC's
/// status. A coroutine that stops reading before the end awaits Finish() all the same: it cancels the RPC, and
/// mostly gives CANCELLED.
///
///     coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kTurnByTurn>> reader{
///         executor, stub, rect};
///     while (auto feature = co_await reader.Read()) Show(*feature);
///     const grpc::Status status = co_await reader.Finish();
///
/// The reader has no buffer: its reactor runs under ReadPacing::kTurnByTurn, so one message at most is in
/// flight, and the stream waits for the coroutine. Delivery decides how each message becomes owned, and when
/// the next one is read:
///
/// Read() gives a std::optional<ResponseT> under both deliveries.
///
/// | Delivery | How                                                         | Next read armed            |
/// |----------|-------------------------------------------------------------|----------------------------|
/// | kPull    | Swapped out of the reactor                                  | At once, by GetResponse()  |
/// | kPush    | Moved out of the message the read callback hands, no copy   | At the next Read(), only   |
///
/// So under kPull the next message can arrive while the coroutine works on this one, and under kPush
/// nothing is read until the coroutine asks for it.
///
/// The reactor lives in the Reader, so in the coroutine's frame. Finish() destroys it on the application
/// thread once its OnDone() has run. A Reader destroyed before, such as in a frame destroyed while
/// suspended, cancels the RPC and destroys the reactor once OnDone() has run, or leaks it should OnDone()
/// never come.
///
/// Non-copyable and non-movable: the reactor's callbacks refer to it.
/// @tparam ReactorT a service-specific reactor, deriving from Client::ActiveReadReactor under
///         ReadPacing::kTurnByTurn, and constructed from a stub, a context, a request and the read callbacks
template <class ReactorT>
  requires(BufferOf(static_cast<ReactorT*>(nullptr)) == Buffer::kNone)
class Reader<ReactorT> {
  /// The response, the delivery and the pacing of the generic reading reactor ReactorT derives from.
  template <class ResponseT_, Client::Delivery kDelivery_, Client::ReadPacing kPacing_>
  struct Traits_ {
    using ResponseT = ResponseT_;
    static constexpr Client::Delivery kDelivery = kDelivery_;
    static constexpr Client::ReadPacing kPacing = kPacing_;
  };
  template <class ResponseT_, Client::Delivery kDelivery_, Client::ReadPacing kPacing_>
  static Traits_<ResponseT_, kDelivery_, kPacing_> TraitsOf(
      const Client::ActiveReadReactor<ResponseT_, kDelivery_, kPacing_>*);

  using Traits = decltype(TraitsOf(static_cast<ReactorT*>(nullptr)));
  using ResponseT = Traits::ResponseT;
  static constexpr Client::Delivery kDelivery = Traits::kDelivery;
  static constexpr bool kPull = kDelivery == Client::Delivery::kPull;

 public:
  /// What Read() gives: a message, owned, or nothing once the stream has ended.
  using MessageT = std::optional<ResponseT>;

  /// Constructs the reactor, which sends the request and arms the first read at once.
  /// @param executor resumes the awaiting coroutine on the application thread, and must outlive this object
  /// @param stub the service stub, which must outlive the RPC
  /// @param request to send, serialized before this constructor returns
  /// @param context of the RPC, for a deadline or metadata: a fresh one by default
  template <class StubT, class RequestT>
  Reader(Executor& executor, StubT& stub, const RequestT& request,
         std::unique_ptr<grpc::ClientContext> context = std::make_unique<grpc::ClientContext>())
      : executor_(executor),
        reactor_(std::make_unique<ReactorT>(stub, std::move(context), request, MakeCallbacks())) {}

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run. A message the
  /// reactor still holds is released, without arming a read.
  ~Reader() {
    read_.Abandon();
    done_.Abandon();
    DestroyAfterOnDone(reactor_, kOnDoneTimeout);
  }

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(Reader&&) = delete;

  /// Sends a best-effort cancel. Callable from the application thread at any time: the stream then ends,
  /// so a Read() gives nothing, and Finish() returns CANCELLED.
  void TryCancel() {
    if (reactor_) reactor_->TryCancel();
  }

  /// Asks for the next message. Under kPush, this is what arms its read: the read the last message left
  /// unarmed. Awaited once per message, until it gives nothing; awaited again after, it gives nothing at
  /// once.
  /// @return an awaitable whose co_await gives the next message, owned, or nothing once the stream has ended
  [[nodiscard]] auto Read() {
    struct Awaiter {
      Reader& reader;
      bool await_ready() const noexcept { return reader.ended_; }
      bool await_suspend(std::coroutine_handle<> waiter) const noexcept { return reader.read_.Suspend(waiter); }
      MessageT await_resume() const { return reader.TakeRead(); }
    };
    assert(reactor_ && "Read() comes before Finish()");
    if constexpr (!kPull) ArmNextRead();
    return Awaiter{*this};
  }

  /// Awaited exactly once: resumes on the application thread once the RPC has ended. Awaited before Read()
  /// has given nothing, it ends the RPC first: it cancels it, and releases the message the reactor holds,
  /// which would otherwise keep it open. The status is then CANCELLED, unless the coroutine had read every
  /// message and the server's own status had already arrived, which it then gives.
  /// @return an awaitable whose co_await gives the RPC's status
  [[nodiscard]] auto Finish() noexcept {
    struct Awaiter {
      Reader& reader;
      static bool await_ready() noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> waiter) const noexcept { return reader.done_.Suspend(waiter); }
      grpc::Status await_resume() const { return reader.TakeStatus(); }
    };
    assert(reactor_ && "Finish() is awaited exactly once");
    if (!ended_) {
      // Stopping early: no read will be asked for, and the message held, if any, is dropped.
      ended_ = true;
      held_ = false;
      reactor_->CancelAndRelease();
    }
    return Awaiter{*this};
  }

 private:
  using Callbacks = Client::ActiveReadCallbacks<ResponseT, kDelivery, Client::ReadPacing::kTurnByTurn>;

  /// The level's own code, on the gRPC thread: records the message under kPush, then hands the coroutine
  /// to the executor. Under kPull the message stays in the reactor. A read that fails, at the end of the
  /// stream or on a cancel, hands the coroutine over with no message, and OnDone() has its own hand-over,
  /// since it can come before the coroutine has resumed from that last read.
  Callbacks MakeCallbacks() {
    Callbacks cbs;
    if constexpr (kPull) {
      cbs.read_ok = [this](auto*, const ResponseT&) { read_.Complete(executor_); };
    } else {
      cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) {
        pushed_ = std::move(response);
        read_.Complete(executor_);
      };
    }
    cbs.read_nok = [this](auto*) { read_.Complete(executor_); };
    cbs.done = [this](auto*, const grpc::Status&) { done_.Complete(executor_); };
    return cbs;
  }

  /// Under kPush, on the application thread: arms the read the last message given left unarmed, if any.
  /// The hand-over is re-armed first, since the read can end on a gRPC thread before ResumeRead() returns.
  void ArmNextRead() {
    if (!held_) return;
    held_ = false;
    read_.Rearm();
    // false once the stream is over, so no read is armed, and none will end
    if (!reactor_->ResumeRead()) ended_ = true;
  }

  /// On the application thread, once a read has ended: takes its message, owned, or records the end of the
  /// stream. Under kPull, taking the message arms the next read, so the hand-over is re-armed first.
  MessageT TakeRead() {
    if (ended_) return MessageT{};  // await_ready() said so: no suspension to resume from
    read_.Resumed();
    if constexpr (kPull) {
      read_.Rearm();
      ResponseT response;
      if (reactor_->GetResponse(response)) return MessageT{std::move(response)};
    } else {
      if (pushed_) {
        held_ = true;
        MessageT message{std::move(*pushed_)};
        pushed_.reset();
        return message;
      }
    }
    ended_ = true;
    return MessageT{};
  }

  /// On the application thread, once the RPC has ended: takes the status out of the reactor, then destroys
  /// the reactor once its OnDone() has returned.
  grpc::Status TakeStatus() {
    done_.Resumed();
    grpc::Status status = reactor_->Status();
    // OnDone() may still be returning, on its gRPC thread, from the callback that resumed this coroutine.
    DestroyAfterOnDone(reactor_, kOnDoneTimeout);
    return status;
  }

  Executor& executor_;
  ResumeHandover read_;  // re-armed for each read
  ResumeHandover done_;
  bool ended_ = false;  // the stream has ended: no read is armed, and none will end. Application thread only
  bool held_ = false;   // Delivery::kPush only: a message was given, and its read is not armed yet
  std::unique_ptr<ResponseT> pushed_;  // Delivery::kPush only, set by the read callback
  std::unique_ptr<ReactorT> reactor_;  // declared last, so the callbacks' targets exist before the RPC starts
};

/// The reader with an unbounded buffer: a server-streaming RPC whose reactor reads on at its own pace, while a
/// coroutine takes the messages one at a time, in order, from a queue inside the reader.
///
///     coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>> reader{
///         executor, stub, rect};
///     while (auto report = co_await reader.Read()) Store(*report);
///     const grpc::Status status = co_await reader.Finish();
///
/// Its reactor runs under Delivery::kPush and ReadPacing::kContinuous: it arms each next read itself, as soon as
/// the read callback returns, so the stream never waits for the coroutine. The read callback, on the gRPC thread,
/// queues the message it is handed, owned, under a lock; Read(), on the application thread, takes the oldest one
/// under the same lock, with no copy, or waits until one comes. Nothing bounds the queue: a coroutine slower than
/// the stream lets messages pile up, which Queued() shows.
///
/// A Read() that finds the queue empty waits through a ResumeHandover. Whichever callback next queues a message,
/// or ends the stream, finds it waiting under the lock, and completes the hand-over once, out of the lock, so no
/// lock is held while the executor queues the coroutine. Once the stream has ended, Read() gives every message
/// still queued, then nothing.
///
/// An early Finish(), awaited before Read() has given nothing, drops the messages still queued, and those still
/// to come, cancels the RPC, and gives CANCELLED, whatever the server's status: the coroutine did not read every
/// message the server sent. Only when nothing was dropped does it give the server's own status, once that status
/// had already arrived. The reader without a buffer never needs to say so, since nothing can wait behind it.
///
/// The reactor lives in the Reader, so in the coroutine's frame, and so does the queue. A frame destroyed with
/// messages queued cancels the RPC, destroys the reactor once its OnDone() has run, then frees the queue, so no
/// callback can still reach it. Should OnDone() never come, the reactor is leaked rather than freed under gRPC.
///
/// Non-copyable and non-movable: the reactor's callbacks refer to it.
/// @tparam ReactorT a service-specific reactor, deriving from Client::ActiveReadReactor under Delivery::kPush and
///         ReadPacing::kContinuous, and constructed from a stub, a context, a request and the read callbacks
template <class ReactorT>
  requires(BufferOf(static_cast<ReactorT*>(nullptr)) == Buffer::kUnbounded)
class Reader<ReactorT> {
  /// The response of the generic reading reactor ReactorT derives from.
  template <class ResponseT_, Client::Delivery kDelivery_, Client::ReadPacing kPacing_>
  struct Traits_ {
    using ResponseT = ResponseT_;
  };
  template <class ResponseT_, Client::Delivery kDelivery_, Client::ReadPacing kPacing_>
  static Traits_<ResponseT_, kDelivery_, kPacing_> TraitsOf(
      const Client::ActiveReadReactor<ResponseT_, kDelivery_, kPacing_>*);

  using ResponseT = decltype(TraitsOf(static_cast<ReactorT*>(nullptr)))::ResponseT;

 public:
  /// What Read() gives: a message, owned, or nothing once the stream has ended and the queue is empty.
  using MessageT = std::optional<ResponseT>;

  /// Constructs the reactor, which sends the request and arms the first read at once.
  /// @param executor resumes the awaiting coroutine on the application thread, and must outlive this object
  /// @param stub the service stub, which must outlive the RPC
  /// @param request to send, serialized before this constructor returns
  /// @param context of the RPC, for a deadline or metadata: a fresh one by default
  template <class StubT, class RequestT>
  Reader(Executor& executor, StubT& stub, const RequestT& request,
         std::unique_ptr<grpc::ClientContext> context = std::make_unique<grpc::ClientContext>())
      : executor_(executor),
        reactor_(std::make_unique<ReactorT>(stub, std::move(context), request, MakeCallbacks())) {}

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run. The messages still
  /// queued are freed after it, with the reader.
  ~Reader() {
    ready_.Abandon();
    done_.Abandon();
    DestroyAfterOnDone(reactor_, kOnDoneTimeout);
  }

  Reader(const Reader&) = delete;
  Reader& operator=(const Reader&) = delete;
  Reader(Reader&&) = delete;
  Reader& operator=(Reader&&) = delete;

  /// Sends a best-effort cancel. Callable from the application thread at any time: the stream then ends, so
  /// once the messages already queued are read, a Read() gives nothing, and Finish() returns CANCELLED.
  void TryCancel() {
    if (reactor_) reactor_->TryCancel();
  }

  /// How many messages wait in the queue: how far the coroutine is behind the stream. A snapshot, which the
  /// stream may change as soon as it is taken.
  /// @return the number of messages queued and not given yet
  [[nodiscard]] std::size_t Queued() const {
    const std::lock_guard lock(mutex_);
    return queue_.size();
  }

  /// Takes the oldest message queued, or waits for the next one. Awaited once per message, until it gives
  /// nothing; awaited again after, it gives nothing at once.
  /// @return an awaitable whose co_await gives the next message, owned, or nothing once the stream has ended
  [[nodiscard]] auto Read() noexcept {
    struct Awaiter {
      Reader& reader;
      MessageT message;     // taken by await_suspend() when one was queued
      bool waited = false;  // await_suspend() reached the hand-over
      bool await_ready() const noexcept { return reader.ended_; }
      bool await_suspend(std::coroutine_handle<> waiter) { return reader.TakeOrWait(waiter, *this); }
      MessageT await_resume() { return reader.TakeRead(*this); }
    };
    assert(reactor_ && "Read() comes before Finish()");
    return Awaiter{*this, {}};
  }

  /// Awaited exactly once: resumes on the application thread once the RPC has ended. Awaited before Read() has
  /// given nothing, it ends the RPC first: it drops the messages queued, and those still to come, and cancels
  /// it. The status is then CANCELLED when a message was dropped, else the server's own once it had arrived.
  /// @return an awaitable whose co_await gives the RPC's status
  [[nodiscard]] auto Finish() {
    struct Awaiter {
      Reader& reader;
      static bool await_ready() noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> waiter) const noexcept { return reader.done_.Suspend(waiter); }
      grpc::Status await_resume() const { return reader.TakeStatus(); }
    };
    assert(reactor_ && "Finish() is awaited exactly once");
    if (!ended_) {
      // Stopping early: no message will be asked for, so the queue is dropped, and a read callback still to
      // come drops its message too.
      ended_ = true;
      {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        if (!queue_.empty()) dropped_ = true;
        queue_.clear();
      }
      reactor_->CancelAndRelease();
    }
    return Awaiter{*this};
  }

 private:
  using Callbacks = Client::ActiveReadCallbacks<ResponseT, Client::Delivery::kPush, Client::ReadPacing::kContinuous>;

  /// The level's own code, on the gRPC thread: queues the message under the lock, or drops it once Finish()
  /// closed the queue, and wakes the Read() waiting, if any, out of the lock. The end of the stream wakes it the
  /// same way. OnDone() has its own hand-over, as it can come while a Read() still has messages to take.
  Callbacks MakeCallbacks() {
    Callbacks cbs;
    cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) {
      bool wake = false;
      {
        const std::lock_guard lock(mutex_);
        if (closed_) {
          dropped_ = true;  // freed below, out of the lock, as the callback returns
        } else {
          queue_.push_back(std::move(response));
          wake = std::exchange(waiting_, false);
        }
      }
      if (wake) ready_.Complete(executor_);
    };
    cbs.read_nok = [this](auto*) {
      bool wake = false;
      {
        const std::lock_guard lock(mutex_);
        stream_ended_ = true;
        wake = std::exchange(waiting_, false);
      }
      if (wake) ready_.Complete(executor_);
    };
    cbs.done = [this](auto*, const grpc::Status&) { done_.Complete(executor_); };
    return cbs;
  }

  /// Read()'s await_suspend(), on the application thread: takes the oldest message queued, or goes on at once
  /// when the stream has ended, or else marks the coroutine waiting under the lock, and suspends through the
  /// hand-over. A callback that finds it waiting completes the hand-over, before or after Suspend().
  /// @return true when the coroutine suspends
  template <class Awaiter>
  bool TakeOrWait(std::coroutine_handle<> waiter, Awaiter& awaiter) {
    {
      const std::lock_guard lock(mutex_);
      if (!queue_.empty()) {
        awaiter.message = PopLocked();
        return false;
      }
      if (stream_ended_) return false;
      waiting_ = true;
    }
    awaiter.waited = true;
    return ready_.Suspend(waiter);
  }

  /// Read()'s await_resume(), on the application thread: gives the message taken, or takes the one a wait was
  /// woken for, or records the end of the stream. A wait is matched by exactly one Complete(), which has come by
  /// now, so the hand-over is re-armed here, before the next wait can be marked: that mark, not Read(), is
  /// what starts the operation the hand-over waits for.
  template <class Awaiter>
  MessageT TakeRead(Awaiter& awaiter) {
    if (ended_) return MessageT{};  // await_ready() said so: no suspension to resume from
    if (awaiter.waited) {
      ready_.Resumed();
      ready_.Rearm();
      const std::lock_guard lock(mutex_);
      if (!queue_.empty()) awaiter.message = PopLocked();
    }
    if (!awaiter.message) ended_ = true;
    return std::move(awaiter.message);
  }

  /// Under the lock: moves the oldest message out of the queue, with no copy of its content.
  MessageT PopLocked() {
    const std::unique_ptr<ResponseT> oldest = std::move(queue_.front());
    queue_.pop_front();
    return MessageT{std::move(*oldest)};
  }

  /// On the application thread, once the RPC has ended: takes the status out of the reactor, then destroys the
  /// reactor once its OnDone() has returned. CANCELLED instead when an early Finish() dropped a message.
  grpc::Status TakeStatus() {
    done_.Resumed();
    grpc::Status status = reactor_->Status();
    // OnDone() may still be returning, on its gRPC thread, from the callback that resumed this coroutine.
    DestroyAfterOnDone(reactor_, kOnDoneTimeout);
    const std::lock_guard lock(mutex_);
    if (dropped_) return grpc::Status(grpc::StatusCode::CANCELLED, kDroppedMessage);
    return status;
  }

  static constexpr const char* kDroppedMessage = "Finish() before the end of the stream: unread messages dropped";

  Executor& executor_;
  ResumeHandover ready_;  // re-armed after each wait of a Read() for the queue
  ResumeHandover done_;
  bool ended_ = false;  // Read() gave nothing, or Finish() came first. Application thread only
  // Guards what follows, shared by the read callbacks on a gRPC thread and the coroutine.
  mutable std::mutex mutex_;
  std::deque<std::unique_ptr<ResponseT>> queue_;  // oldest first
  bool waiting_ = false;       // a Read() waits, or is about to: the next callback completes the hand-over
  bool stream_ended_ = false;  // the read callback reported the end of the stream
  bool closed_ = false;        // an early Finish() came: the read callback drops its message
  bool dropped_ = false;       // a message was dropped unread, so the status is CANCELLED
  std::unique_ptr<ReactorT> reactor_;  // declared last, so the callbacks' targets exist before the RPC starts
};

}  // namespace RpcReactor::coro
