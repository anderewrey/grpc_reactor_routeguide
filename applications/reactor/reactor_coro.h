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
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <utility>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/latest_slot.h"
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
  /// Delivery::kPull with ReadPacing::kContinuous: one slot keeps the newest message, and the stream never waits.
  kLatest,
  /// Delivery::kPush with ReadPacing::kPerMessage: messages are queued, and the stream waits once a given number
  /// of them do.
  kBounded,
};

/// The buffer of the reader over a reactor of this Delivery and ReadPacing: the one place that maps a reactor
/// configuration to a reader. A combination with no reader gives nothing, and a Reader over it does not compile.
/// @return the buffer, or nothing when no reader takes that combination
constexpr std::optional<Buffer> BufferFor(Client::Delivery delivery, Client::ReadPacing pacing) noexcept {
  if (pacing == Client::ReadPacing::kTurnByTurn) return Buffer::kNone;
  if (delivery == Client::Delivery::kPush && pacing == Client::ReadPacing::kContinuous) return Buffer::kUnbounded;
  if (delivery == Client::Delivery::kPull && pacing == Client::ReadPacing::kContinuous) return Buffer::kLatest;
  if (delivery == Client::Delivery::kPush && pacing == Client::ReadPacing::kPerMessage) return Buffer::kBounded;
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
                "coro::Reader takes a reading reactor under ReadPacing::kTurnByTurn (no buffer), under "
                "Delivery::kPush and ReadPacing::kContinuous (the unbounded buffer), under Delivery::kPull and "
                "ReadPacing::kContinuous (the latest-only buffer), or under Delivery::kPush and "
                "ReadPacing::kPerMessage (the bounded buffer)");
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
/// callback can still reach it. Should OnDone() never come, the reactor is leaked rather than freed under gRPC, and
/// a callback it still runs would reach the freed reader, as for every awaitable of the level.
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

  /// The message of the CANCELLED status an early Finish() gives when it dropped a message, whatever the status
  /// the RPC itself ended with.
  static constexpr const char* kDroppedMessage = "Finish() before the end of the stream: unread messages dropped";

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
  /// once the messages already queued are read, a Read() gives nothing, and Finish() mostly returns CANCELLED.
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
  /// That happens when Finish() is called, not when it is awaited: a message arriving in between is dropped.
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
      Queue dropped;  // freed out of the lock, as this block ends
      {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        if (!queue_.empty()) dropped_ = true;
        dropped.swap(queue_);
      }
      reactor_->CancelAndRelease();
    }
    return Awaiter{*this};
  }

 private:
  using Callbacks = Client::ActiveReadCallbacks<ResponseT, Client::Delivery::kPush, Client::ReadPacing::kContinuous>;
  using Queue = std::deque<std::unique_ptr<ResponseT>>;

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
    std::unique_ptr<ResponseT> oldest;
    bool wait = false;
    {
      const std::lock_guard lock(mutex_);
      oldest = PopLocked();
      wait = !oldest && !stream_ended_;
      waiting_ = wait;
    }
    if (!wait) {
      if (oldest) awaiter.message = std::move(*oldest);  // moved out of the lock, with no copy of its content
      return false;
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
      std::unique_ptr<ResponseT> oldest;
      {
        const std::lock_guard lock(mutex_);
        oldest = PopLocked();
      }
      if (oldest) awaiter.message = std::move(*oldest);
    }
    if (!awaiter.message) ended_ = true;
    return std::move(awaiter.message);
  }

  /// Under the lock: takes the oldest message out of the queue, for the caller to move out of the lock.
  /// @return the oldest message, or null when the queue is empty
  std::unique_ptr<ResponseT> PopLocked() {
    if (queue_.empty()) return nullptr;
    std::unique_ptr<ResponseT> oldest = std::move(queue_.front());
    queue_.pop_front();
    return oldest;
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

  Executor& executor_;
  ResumeHandover ready_;  // re-armed after each wait of a Read() for the queue
  ResumeHandover done_;
  bool ended_ = false;  // Read() gave nothing, or Finish() came first. Application thread only
  // Guards what follows, shared by the read callbacks on a gRPC thread and the coroutine.
  mutable std::mutex mutex_;
  Queue queue_;                // oldest first
  bool waiting_ = false;       // a Read() waits, or is about to: the next callback completes the hand-over
  bool stream_ended_ = false;  // the read callback reported the end of the stream
  bool closed_ = false;        // an early Finish() came: the read callback drops its message
  bool dropped_ = false;       // a message was dropped unread, so the status is CANCELLED
  std::unique_ptr<ReactorT> reactor_;  // declared last, so the callbacks' targets exist before the RPC starts
};

/// The reader with a latest-only buffer: a server-streaming RPC whose reactor reads on at its own pace, while a
/// coroutine takes only the newest message, from one slot inside the reader. A message the coroutine has not
/// taken yet is overwritten by the next one.
///
///     coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>> reader{
///         executor, stub, rect};
///     while (auto fix = co_await reader.Read()) Draw(*fix);  // reader.Overwritten() fixes skipped before this one
///     const grpc::Status status = co_await reader.Finish();
///
/// Its reactor runs under Delivery::kPull and ReadPacing::kContinuous: it lends each message to the read callback,
/// and arms the next read into the same storage as soon as the callback returns, so the stream never waits for the
/// coroutine. The read callback, on the gRPC thread, therefore copies the message, then puts the copy in a
/// LatestSlot, under the reader's lock, replacing the message the coroutine has not taken; Read(), on the
/// application thread, takes the newest one under the same lock, or waits until one comes, through a
/// ResumeHandover, as the unbounded reader does. The copy is the level's own code, so no user code runs on the
/// gRPC thread. It is the cost of this buffer: one copy per message, taken or not.
///
/// Read() gives the message only, a std::optional<ResponseT> as for every reader. Overwritten() then says how many
/// messages were overwritten, unread, since the message the previous Read() gave, so a coroutine that reads to the
/// end has seen every message as either taken or overwritten. The newest message is never overwritten: once the
/// stream has ended, Read() gives the one left in the slot, if any, then nothing.
///
/// An early Finish(), awaited before Read() has given nothing, drops the message in the slot, and those still to
/// come, cancels the RPC, and gives CANCELLED when it dropped one, as the unbounded reader does: else the server's
/// own status, once that status had already arrived.
///
/// The reactor lives in the Reader, so in the coroutine's frame, and so does the slot. A frame destroyed with a
/// message in the slot cancels the RPC, destroys the reactor once its OnDone() has run, then frees the slot.
///
/// Non-copyable and non-movable: the reactor's callbacks refer to it.
/// @tparam ReactorT a service-specific reactor, deriving from Client::ActiveReadReactor under Delivery::kPull and
///         ReadPacing::kContinuous, and constructed from a stub, a context, a request and the read callbacks
template <class ReactorT>
  requires(BufferOf(static_cast<ReactorT*>(nullptr)) == Buffer::kLatest)
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
  /// What Read() gives: the newest message, owned, or nothing once the stream has ended and the slot is empty.
  using MessageT = std::optional<ResponseT>;

  /// The message of the CANCELLED status an early Finish() gives when it dropped a message, whatever the status
  /// the RPC itself ended with.
  static constexpr const char* kDroppedMessage = "Finish() before the end of the stream: unread message dropped";

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

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run. A message still in the
  /// slot is freed after it, with the reader.
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
  /// once the message in the slot is read, a Read() gives nothing, and Finish() mostly returns CANCELLED.
  void TryCancel() {
    if (reactor_) reactor_->TryCancel();
  }

  /// How many messages were overwritten in the slot, unread, between the message the previous Read() gave and
  /// the one the last Read() gave. Application thread only, like Read().
  /// @return the messages skipped before the last one given; 0 before any, and after a Read() that gave nothing
  [[nodiscard]] std::uint64_t Overwritten() const noexcept { return overwritten_; }

  /// Takes the newest message, or waits for the next one. Awaited until it gives nothing; awaited again after, it
  /// gives nothing at once.
  /// @return an awaitable whose co_await gives the newest message, owned, or nothing once the stream has ended
  [[nodiscard]] auto Read() noexcept {
    struct Awaiter {
      Reader& reader;
      MessageT message;     // taken by await_suspend() when the slot held one
      bool waited = false;  // await_suspend() reached the hand-over
      bool await_ready() const noexcept { return reader.ended_; }
      bool await_suspend(std::coroutine_handle<> waiter) { return reader.TakeOrWait(waiter, *this); }
      MessageT await_resume() { return reader.TakeRead(*this); }
    };
    assert(reactor_ && "Read() comes before Finish()");
    overwritten_ = 0;
    return Awaiter{*this, {}};
  }

  /// Awaited exactly once: resumes on the application thread once the RPC has ended. Awaited before Read() has
  /// given nothing, it ends the RPC first: it drops the message in the slot, and those still to come, and cancels
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
      // Stopping early: no message will be asked for, so the slot is emptied, and a read callback still to come
      // drops its message too.
      ended_ = true;
      MessageT dropped;  // freed out of the lock, as this block ends
      {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        dropped = slot_.Take();
        if (dropped) dropped_ = true;
      }
      reactor_->CancelAndRelease();
    }
    return Awaiter{*this};
  }

 private:
  using Callbacks = Client::ActiveReadCallbacks<ResponseT, Client::Delivery::kPull, Client::ReadPacing::kContinuous>;

  /// The level's own code, on the gRPC thread: copies the lent message, since the reactor reads the next one into
  /// the same storage once the callback returns, and puts the copy in the slot under the lock, or drops it once
  /// Finish() closed the slot. Wakes the Read() waiting, if any, out of the lock. The end of the stream wakes it
  /// the same way. OnDone() has its own hand-over, as it can come while the slot still holds a message.
  Callbacks MakeCallbacks() {
    Callbacks cbs;
    cbs.read_ok = [this](auto*, const ResponseT& lent) {
      ResponseT copy{lent};  // copied out of the lock; the message it replaces is freed under it
      bool wake = false;
      {
        const std::lock_guard lock(mutex_);
        if (closed_) {
          dropped_ = true;
        } else {
          // Put() says whether the slot was empty: the waiting flag already says whether to wake.
          static_cast<void>(slot_.Put(std::move(copy)));
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

  /// Read()'s await_suspend(), on the application thread: takes the message in the slot, or goes on at once when
  /// the stream has ended, or else marks the coroutine waiting under the lock, and suspends through the hand-over.
  /// The slot is taken before the end of the stream is honoured, so the last message is always given.
  /// @return true when the coroutine suspends
  template <class Awaiter>
  bool TakeOrWait(std::coroutine_handle<> waiter, Awaiter& awaiter) {
    bool wait = false;
    {
      const std::lock_guard lock(mutex_);
      awaiter.message = TakeLocked();
      wait = !awaiter.message && !stream_ended_;
      waiting_ = wait;
    }
    if (!wait) return false;
    awaiter.waited = true;
    return ready_.Suspend(waiter);
  }

  /// Read()'s await_resume(), on the application thread: gives the message taken, or takes the one a wait was
  /// woken for, or records the end of the stream. The hand-over is re-armed here, as in the unbounded reader.
  template <class Awaiter>
  MessageT TakeRead(Awaiter& awaiter) {
    if (ended_) return MessageT{};  // await_ready() said so: no suspension to resume from
    if (awaiter.waited) {
      ready_.Resumed();
      ready_.Rearm();
      const std::lock_guard lock(mutex_);
      awaiter.message = TakeLocked();
    }
    if (!awaiter.message) ended_ = true;
    return std::move(awaiter.message);
  }

  /// Under the lock: takes the message in the slot, and counts the messages it replaced since the last one taken.
  /// @return the newest message, or nothing when the slot is empty
  MessageT TakeLocked() {
    MessageT newest = slot_.Take();
    if (newest) {
      const std::uint64_t replaced = slot_.Replaced();
      overwritten_ = replaced - replaced_at_take_;
      replaced_at_take_ = replaced;
    }
    return newest;
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

  Executor& executor_;
  ResumeHandover ready_;  // re-armed after each wait of a Read() for the slot
  ResumeHandover done_;
  bool ended_ = false;  // Read() gave nothing, or Finish() came first. Application thread only
  // Application thread only: written under the lock by TakeLocked(), which only the coroutine calls.
  std::uint64_t overwritten_ = 0;       // messages replaced between the last two messages given
  std::uint64_t replaced_at_take_ = 0;  // slot_.Replaced() when the last message was given
  // Guards what follows, shared by the read callbacks on a gRPC thread and the coroutine. The slot's own lock is
  // only ever taken under it.
  mutable std::mutex mutex_;
  LatestSlot<ResponseT> slot_;
  bool waiting_ = false;       // a Read() waits, or is about to: the next callback completes the hand-over
  bool stream_ended_ = false;  // the read callback reported the end of the stream
  bool closed_ = false;        // an early Finish() came: the read callback drops its message
  bool dropped_ = false;       // a message was dropped unread, so the status is CANCELLED
  std::unique_ptr<ReactorT> reactor_;  // declared last, so the callbacks' targets exist before the RPC starts
};

/// The reader with a bounded buffer: a server-streaming RPC whose reactor reads on while few messages wait for
/// the coroutine, and holds the stream once a given number of them do, until the coroutine has taken one.
///
///     coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush>> reader{executor, stub, rect, 3};
///     while (auto place = co_await reader.Read()) Save(*place);
///     const grpc::Status status = co_await reader.Finish();
///
/// Its reactor runs under Delivery::kPush and ReadPacing::kPerMessage, and the reader takes the per-message
/// decision itself, so no user code runs on the gRPC thread. The read callback queues the message it is handed,
/// owned, under a lock, as the unbounded reader does, then answers: it lets the reactor read on while fewer than
/// the capacity wait, and holds the stream once that many do. Read(), on the application thread, takes the oldest
/// message under the same lock, or waits until one comes; the Read() that takes the queue below the capacity
/// releases the hold, through ResumeRead(), out of the lock. While the stream is held no read is armed, so no
/// message can come, and the queue never holds more than the capacity.
///
/// An early Finish(), awaited before Read() has given nothing, drops the messages queued, and those still to come,
/// then cancels the RPC and releases the hold, if any, through the reactor's CancelAndRelease(): a held stream
/// would otherwise keep the RPC open, and Finish() would never resume. The status follows the unbounded reader's
/// rule: CANCELLED when a message was dropped, else the server's own once it had arrived.
///
/// The reactor lives in the Reader, so in the coroutine's frame, and so does the queue. A frame destroyed with the
/// stream held cancels the RPC and releases the hold, destroys the reactor once its OnDone() has run, then frees
/// the queue.
///
/// Non-copyable and non-movable: the reactor's callbacks refer to it.
/// @tparam ReactorT a service-specific reactor, deriving from Client::ActiveReadReactor under Delivery::kPush and
///         ReadPacing::kPerMessage, and constructed from a stub, a context, a request and the read callbacks
template <class ReactorT>
  requires(BufferOf(static_cast<ReactorT*>(nullptr)) == Buffer::kBounded)
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

  /// The message of the CANCELLED status an early Finish() gives when it dropped a message, whatever the status
  /// the RPC itself ended with.
  static constexpr const char* kDroppedMessage = "Finish() before the end of the stream: unread messages dropped";

  /// Constructs the reactor, which sends the request and arms the first read at once.
  /// @param executor resumes the awaiting coroutine on the application thread, and must outlive this object
  /// @param stub the service stub, which must outlive the RPC
  /// @param request to send, serialized before this constructor returns
  /// @param capacity how many messages may wait for the coroutine before the stream is held: at least 1
  /// @param context of the RPC, for a deadline or metadata: a fresh one by default
  template <class StubT, class RequestT>
  Reader(Executor& executor, StubT& stub, const RequestT& request, std::size_t capacity,
         std::unique_ptr<grpc::ClientContext> context = std::make_unique<grpc::ClientContext>())
      : executor_(executor),
        capacity_(CheckedCapacity(capacity)),
        reactor_(std::make_unique<ReactorT>(stub, std::move(context), request, MakeCallbacks())) {}

  /// Cancels an RPC still in flight, releasing a hold, and destroys the reactor once its OnDone() has run. The
  /// messages still queued are freed after it, with the reader.
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
  /// once the messages already queued are read, a Read() gives nothing, and Finish() mostly returns CANCELLED.
  /// A held stream ends too: gRPC reports the cancel once the next Read() has released the hold.
  void TryCancel() {
    if (reactor_) reactor_->TryCancel();
  }

  /// How many messages wait in the queue: never more than the capacity. A snapshot, which the stream may change
  /// as soon as it is taken.
  /// @return the number of messages queued and not given yet
  [[nodiscard]] std::size_t Queued() const {
    const std::lock_guard lock(mutex_);
    return queue_.size();
  }

  /// How many times the read callback held the stream, since the RPC started. A snapshot, as Queued() is.
  /// @return the number of holds taken
  [[nodiscard]] std::uint64_t Holds() const {
    const std::lock_guard lock(mutex_);
    return holds_;
  }

  /// Takes the oldest message queued, or waits for the next one, and releases the stream's hold once the queue is
  /// below the capacity. Awaited once per message, until it gives nothing; awaited again after, it gives nothing
  /// at once.
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
  /// given nothing, it ends the RPC first: it drops the messages queued, and those still to come, cancels it, and
  /// releases the stream's hold. The status is then CANCELLED when a message was dropped, else the server's own
  /// once it had arrived.
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
      // Stopping early: no message will be asked for, so the queue is dropped, a read callback still to come
      // drops its message too and holds nothing, and the hold taken, if any, is the reactor's to release.
      ended_ = true;
      Queue dropped;  // freed out of the lock, as this block ends
      {
        const std::lock_guard lock(mutex_);
        closed_ = true;
        holding_ = false;
        if (!queue_.empty()) dropped_ = true;
        dropped.swap(queue_);
      }
      reactor_->CancelAndRelease();
    }
    return Awaiter{*this};
  }

 private:
  using Callbacks = Client::ActiveReadCallbacks<ResponseT, Client::Delivery::kPush, Client::ReadPacing::kPerMessage>;
  using Queue = std::deque<std::unique_ptr<ResponseT>>;

  /// @return the capacity, asserted to be at least 1: a reader that holds an empty queue would never resume
  static std::size_t CheckedCapacity(std::size_t capacity) noexcept {
    assert(capacity > 0 && "a bounded reader holds at least one message");
    return capacity;
  }

  /// The level's own code, on the gRPC thread: queues the message under the lock, then holds the stream once the
  /// queue is full, or drops the message once Finish() closed the queue, and wakes the Read() waiting, if any, out
  /// of the lock. The hold is recorded after the message is queued, so the Read() that releases it has the message
  /// to take; the reactor published the hold before this callback, so a ResumeRead() reaching it before this
  /// callback returns is safe. The end of the stream wakes a Read() the same way.
  Callbacks MakeCallbacks() {
    Callbacks cbs;
    cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) -> bool {
      bool wake = false;
      bool hold = false;
      {
        const std::lock_guard lock(mutex_);
        if (closed_) {
          dropped_ = true;  // freed below, out of the lock, as the callback returns
        } else {
          queue_.push_back(std::move(response));
          hold = queue_.size() >= capacity_;
          if (hold) {
            holding_ = true;
            ++holds_;
          }
          wake = std::exchange(waiting_, false);
        }
      }
      if (wake) ready_.Complete(executor_);
      return hold;
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
  /// hand-over. A message taken may release the stream's hold.
  /// @return true when the coroutine suspends
  template <class Awaiter>
  bool TakeOrWait(std::coroutine_handle<> waiter, Awaiter& awaiter) {
    std::unique_ptr<ResponseT> oldest;
    bool wait = false;
    bool release = false;
    {
      const std::lock_guard lock(mutex_);
      oldest = PopLocked(release);
      wait = !oldest && !stream_ended_;
      waiting_ = wait;
    }
    if (release) ReleaseHold();
    if (!wait) {
      if (oldest) awaiter.message = std::move(*oldest);  // moved out of the lock, with no copy of its content
      return false;
    }
    awaiter.waited = true;
    return ready_.Suspend(waiter);
  }

  /// Read()'s await_resume(), on the application thread: gives the message taken, or takes the one a wait was
  /// woken for, which may release the stream's hold too, or records the end of the stream. The hand-over is
  /// re-armed here, as in the unbounded reader.
  template <class Awaiter>
  MessageT TakeRead(Awaiter& awaiter) {
    if (ended_) return MessageT{};  // await_ready() said so: no suspension to resume from
    if (awaiter.waited) {
      ready_.Resumed();
      ready_.Rearm();
      std::unique_ptr<ResponseT> oldest;
      bool release = false;
      {
        const std::lock_guard lock(mutex_);
        oldest = PopLocked(release);
      }
      if (release) ReleaseHold();
      if (oldest) awaiter.message = std::move(*oldest);
    }
    if (!awaiter.message) ended_ = true;
    return std::move(awaiter.message);
  }

  /// Under the lock: takes the oldest message out of the queue, for the caller to move out of the lock, and claims
  /// the stream's hold once the queue is below the capacity, for the caller to release out of the lock.
  /// @param release set when the caller must release the hold
  /// @return the oldest message, or null when the queue is empty
  std::unique_ptr<ResponseT> PopLocked(bool& release) {
    if (queue_.empty()) return nullptr;
    std::unique_ptr<ResponseT> oldest = std::move(queue_.front());
    queue_.pop_front();
    if (holding_ && queue_.size() < capacity_) {
      holding_ = false;
      release = true;
    }
    return oldest;
  }

  /// On the application thread, out of the lock: releases the stream's hold, which arms the next read. Should the
  /// stream be over, no read is armed, and none will report its end, so the end is recorded here.
  void ReleaseHold() {
    if (reactor_->ResumeRead()) return;
    const std::lock_guard lock(mutex_);
    stream_ended_ = true;
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

  Executor& executor_;
  const std::size_t capacity_;  // the most messages queued: once that many wait, the stream is held
  ResumeHandover ready_;        // re-armed after each wait of a Read() for the queue
  ResumeHandover done_;
  bool ended_ = false;  // Read() gave nothing, or Finish() came first. Application thread only
  // Guards what follows, shared by the read callbacks on a gRPC thread and the coroutine.
  mutable std::mutex mutex_;
  Queue queue_;                // oldest first
  bool holding_ = false;       // the read callback held the stream, and no Read() has claimed the release yet
  std::uint64_t holds_ = 0;    // holds taken so far
  bool waiting_ = false;       // a Read() waits, or is about to: the next callback completes the hand-over
  bool stream_ended_ = false;  // the read callback reported the end of the stream, or no read is armed
  bool closed_ = false;        // an early Finish() came: the read callback drops its message
  bool dropped_ = false;       // a message was dropped unread, so the status is CANCELLED
  std::unique_ptr<ReactorT> reactor_;  // declared last, so the callbacks' targets exist before the RPC starts
};

}  // namespace RpcReactor::coro
