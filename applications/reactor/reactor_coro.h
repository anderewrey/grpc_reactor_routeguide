///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <grpcpp/grpcpp.h>

#include <cassert>
#include <chrono>
#include <concepts>
#include <coroutine>
#include <cstddef>
#include <memory>
#include <optional>
#include <ranges>
#include <type_traits>
#include <utility>
#include <vector>

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

/// A server-streaming RPC a coroutine reads one message at a time: the request is sent on construction,
/// `co_await Read()` resumes the coroutine on the application thread with the next message, owned, or with
/// nothing once the stream has ended, and `co_await Finish()` then gives the RPC's status. A coroutine that
/// stops reading before the end awaits Finish() all the same: it cancels the RPC, and mostly gives CANCELLED.
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
class Reader {
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
  static_assert(Traits::kPacing == Client::ReadPacing::kTurnByTurn,
                "a reader without a buffer takes one message at a time: ReadPacing::kTurnByTurn");

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

// ---------------------------------------------------------------------------------------------------------
// The writers: Writer, whose coroutine awaits each write, and Source, a range the reactor pulls from. Both
// end with the final response of a client-streaming RPC, as a UnaryResult.
// ---------------------------------------------------------------------------------------------------------

/// The request, the response, the delivery and the submission of the generic writing reactor ReactorT
/// derives from, for Writer and Source.
template <class ReactorT>
class WriteTraits {
  template <class RequestT_, class ResponseT_, Client::Delivery kDelivery_, Client::Submission kSubmission_>
  struct Traits_ {
    using RequestT = RequestT_;
    using ResponseT = ResponseT_;
    static constexpr Client::Delivery kDelivery = kDelivery_;
    static constexpr Client::Submission kSubmission = kSubmission_;
  };
  template <class RequestT_, class ResponseT_, Client::Delivery kDelivery_, Client::Submission kSubmission_>
  static Traits_<RequestT_, ResponseT_, kDelivery_, kSubmission_> TraitsOf(
      const Client::ActiveWriteReactor<RequestT_, ResponseT_, kDelivery_, kSubmission_>*);
  using Traits = decltype(TraitsOf(static_cast<ReactorT*>(nullptr)));

 public:
  using RequestT = Traits::RequestT;
  using ResponseT = Traits::ResponseT;
  static constexpr Client::Delivery kDelivery = Traits::kDelivery;
  static constexpr Client::Submission kSubmission = Traits::kSubmission;
  using ResultT = UnaryResult<ResponseT>;
  using Callbacks = Client::ActiveWriteCallbacks<RequestT, ResponseT, kDelivery, kSubmission>;

  /// On the application thread, once a writing RPC has ended: takes its status and its final response out of
  /// the reactor, then destroys the reactor once its OnDone() has returned.
  /// @param reactor the awaitable's reactor, whose OnDone() has called the done callback
  /// @param pushed Delivery::kPush only: the response the done callback was handed, never null by then
  static ResultT TakeResult(std::unique_ptr<ReactorT>& reactor, std::unique_ptr<ResponseT>& pushed) {
    ResultT result{reactor->Status(), {}};
    if constexpr (kDelivery == Client::Delivery::kPull) {
      static_cast<void>(reactor->GetResponse(result.response));  // false on a failed RPC: left empty
    } else {
      result.response = std::move(*pushed);  // never null: the reactor hands its message over even on a failure
    }
    // OnDone() may still be returning, on its gRPC thread, from the callback that resumed this coroutine.
    DestroyAfterOnDone(reactor, kOnDoneTimeout);
    return result;
  }

  /// The done callback of a writing awaitable, on the gRPC thread: records the response under kPush, then
  /// hands the coroutine to the executor. Under kPull the response and the status stay in the reactor.
  static typename Callbacks::OnDoneCallback DoneCallback(Executor& executor, ResumeHandover& done,
                                                std::unique_ptr<ResponseT>& pushed) {
    if constexpr (kDelivery == Client::Delivery::kPull) {
      return [&executor, &done](auto*, const grpc::Status&, const ResponseT&) { done.Complete(executor); };
    } else {
      return [&executor, &done, &pushed](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
        pushed = std::move(response);
        done.Complete(executor);
      };
    }
  }
};

/// A client-streaming RPC a coroutine writes one request at a time: `co_await Write(request)` resumes the
/// coroutine on the application thread once the write has completed, `co_await Close()` ends the request
/// stream, and `co_await Finish()` then gives the RPC's status and its final response, as a UnaryResult.
///
///     coro::Writer<routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kApplicationPushes>>
///         writer{executor, stub};
///     for (auto& point : points) {
///       if (!co_await writer.Write(std::move(point))) break;  // the RPC has ended
///     }
///     static_cast<void>(co_await writer.Close());
///     auto [status, summary] = co_await writer.Finish();
///
/// One write is in flight at most, which the shape of the code enforces: a coroutine cannot start a second
/// Write() before the first one resumes it. Write() gives false once the RPC has ended, a failed write
/// included, and the request was then not sent; it is never a reason to retry. A write can still succeed
/// after the server has ended the RPC, while its status is on the way, so Finish() is what says how the RPC
/// ended, never the writes.
///
/// Stopping early is awaiting Finish() before Close(): it cancels the RPC, whose write flow would otherwise
/// keep it open, and gives CANCELLED, or the server's own status had it already arrived. After a write that
/// failed, the RPC ends on its own, and Finish() gives its status without cancelling.
///
/// The reactor lives in the Writer, so in the coroutine's frame. Finish() destroys it on the application
/// thread once its OnDone() has run. A Writer destroyed before, such as in a frame destroyed while
/// suspended, cancels the RPC and destroys the reactor once OnDone() has run, or leaks it should OnDone()
/// never come.
///
/// Non-copyable and non-movable: the reactor's callbacks refer to it.
/// @tparam ReactorT a service-specific reactor, deriving from Client::ActiveWriteReactor under
///         Submission::kApplicationPushes, and constructed from a stub, a context and the write callbacks
template <class ReactorT>
class Writer {
  using Traits = WriteTraits<ReactorT>;
  static_assert(Traits::kSubmission == Client::Submission::kApplicationPushes,
                "a Writer's coroutine starts each write: Submission::kApplicationPushes. A range the reactor "
                "pulls from is a Source");

 public:
  /// What Write() sends.
  using RequestT = Traits::RequestT;
  /// What Finish() returns.
  using ResultT = Traits::ResultT;

  /// Constructs the reactor, which starts the RPC. Nothing is sent until the first Write().
  /// @param executor resumes the awaiting coroutine on the application thread, and must outlive this object
  /// @param stub the service stub, which must outlive the RPC
  /// @param context of the RPC, for a deadline or metadata: a fresh one by default
  template <class StubT>
  Writer(Executor& executor, StubT& stub,
         std::unique_ptr<grpc::ClientContext> context = std::make_unique<grpc::ClientContext>())
      : executor_(executor),
        reactor_(std::make_unique<ReactorT>(stub, std::move(context), MakeCallbacks())) {}

  /// Cancels an RPC still in flight, which ends its write flow, and destroys the reactor once its OnDone()
  /// has run.
  ~Writer() {
    write_.Abandon();
    done_.Abandon();
    DestroyAfterOnDone(reactor_, kOnDoneTimeout);
  }

  Writer(const Writer&) = delete;
  Writer& operator=(const Writer&) = delete;
  Writer(Writer&&) = delete;
  Writer& operator=(Writer&&) = delete;

  /// Sends a best-effort cancel. Callable from the application thread at any time: a Write() then gives
  /// false, and Finish() returns CANCELLED.
  void TryCancel() {
    if (reactor_) reactor_->TryCancel();
  }

  /// Sends a request. The write starts here, so a Write() is always awaited, at once.
  /// @param request to send, moved into the reactor
  /// @return an awaitable whose co_await gives true once the request was written, or false, at once or once
  ///         the write failed, when the RPC has ended and the request was not sent
  [[nodiscard]] auto Write(RequestT request) {
    struct Awaiter {
      Writer& writer;
      bool started;
      bool await_ready() const noexcept { return !started; }
      bool await_suspend(std::coroutine_handle<> waiter) const noexcept { return writer.write_.Suspend(waiter); }
      bool await_resume() const { return started && writer.TakeWrite(); }
    };
    assert(reactor_ && !closed_ && "Write() comes before Close() and Finish()");
    // Refused once the RPC has ended: cancelled, a write failed, or OnDone() has run. Never because a write
    // is in flight, since the coroutine resumed from the last one before it could call this.
    const bool started = !ended_ && reactor_->SendRequest(std::move(request));
    if (!started) ended_ = true;
    return Awaiter{*this, started};
  }

  /// Ends the request stream: no Write() may follow. It never suspends: the close completing is seen by
  /// Finish(), which waits for the RPC to end.
  /// @return an awaitable whose co_await gives true once the close is started, or false when the RPC has
  ///         ended
  [[nodiscard]] auto Close() {
    struct Awaiter {
      bool closing;
      static bool await_ready() noexcept { return true; }
      static void await_suspend(std::coroutine_handle<> /*waiter*/) noexcept {}
      bool await_resume() const noexcept { return closing; }
    };
    assert(reactor_ && !closed_ && "Close() is called once, before Finish()");
    closed_ = true;
    const bool closing = !ended_ && reactor_->CloseRequestStream();
    if (!closing) ended_ = true;
    return Awaiter{closing};
  }

  /// Awaited exactly once: resumes on the application thread once the RPC has ended. Awaited before Close(),
  /// with the RPC still running, it ends the RPC first: it cancels it, which ends the write flow that would
  /// otherwise keep it open. The status is then CANCELLED, unless the server's own status had already arrived.
  /// @return an awaitable whose co_await gives the ResultT
  [[nodiscard]] auto Finish() noexcept {
    struct Awaiter {
      Writer& writer;
      static bool await_ready() noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> waiter) const noexcept { return writer.done_.Suspend(waiter); }
      ResultT await_resume() const {
        writer.done_.Resumed();
        return Traits::TakeResult(writer.reactor_, writer.pushed_);
      }
    };
    assert(reactor_ && "Finish() is awaited exactly once");
    if (!closed_ && !ended_) {
      // Stopping early: no write or close will come, and the open write flow would keep the RPC open.
      ended_ = true;
      reactor_->TryCancel();
    }
    return Awaiter{*this};
  }

 private:
  /// The level's own code, on the gRPC thread: records the outcome of a write, or the response under kPush,
  /// then hands the coroutine to the executor. OnDone() has its own hand-over, since it can come before the
  /// coroutine has resumed from a write that failed.
  typename Traits::Callbacks MakeCallbacks() {
    typename Traits::Callbacks cbs;
    cbs.write_done = [this](auto*, bool ok) {
      write_ok_ = ok;  // published by Complete()
      write_.Complete(executor_);
    };
    cbs.done = Traits::DoneCallback(executor_, done_, pushed_);
    return cbs;
  }

  /// On the application thread, once a write has ended: whether it succeeded. The hand-over is re-armed here,
  /// before the next Write() can start a write that ends on a gRPC thread at once.
  bool TakeWrite() {
    write_.Resumed();
    write_.Rearm();
    if (!write_ok_) ended_ = true;
    return write_ok_;
  }

  Executor& executor_;
  ResumeHandover write_;  // re-armed after each write
  ResumeHandover done_;
  bool write_ok_ = false;  // set by the write callback before it hands the coroutine over
  bool ended_ = false;     // the RPC has ended for the writes: none can start. Application thread only
  bool closed_ = false;    // Close() was called. Application thread only
  std::unique_ptr<typename Traits::ResponseT> pushed_;  // Delivery::kPush only, set by the done callback
  std::unique_ptr<ReactorT> reactor_;  // declared last, so the callbacks' targets exist before the RPC starts
};

/// A client-streaming RPC whose requests are all at hand: the reactor pulls them from a range, one per
/// completed write, closes the request stream after the last one, and `co_await Finish()` gives the RPC's
/// status and its final response, as a UnaryResult.
///
///     coro::Source<routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>> upload{
///         executor, stub, std::move(track)};
///     auto [status, summary] = co_await upload.Finish();
///
/// The source takes any range of requests: a std::vector<RequestT> passed by rvalue is moved in, and any
/// other range, a view included, is copied into one, on the constructing thread. So the pulls, on gRPC
/// threads, only move requests out of a vector: no code of the application's, such as a view's function,
/// runs off the application thread.
///
/// Finish() never cancels. The coroutine skips nothing by awaiting it at once: the reactor sends the whole
/// range on its own, and Finish() waits for the RPC to end. TryCancel(), or destroying the Source, stops it.
///
/// The reactor lives in the Source, so in the coroutine's frame, as for the other awaitables.
///
/// Non-copyable and non-movable: the reactor's callbacks refer to it.
/// @tparam ReactorT a service-specific reactor, deriving from Client::ActiveWriteReactor under
///         Submission::kReactorPulls, and constructed from a stub, a context and the write callbacks
template <class ReactorT>
class Source {
  using Traits = WriteTraits<ReactorT>;
  static_assert(Traits::kSubmission == Client::Submission::kReactorPulls,
                "a Source is pulled by its reactor: Submission::kReactorPulls. A coroutine that awaits each "
                "write uses a Writer");

 public:
  /// What the range holds.
  using RequestT = Traits::RequestT;
  /// What Finish() returns.
  using ResultT = Traits::ResultT;

  /// Takes the requests, then constructs the reactor, which pulls the first one and starts the RPC.
  /// @param executor resumes the awaiting coroutine on the application thread, and must outlive this object
  /// @param stub the service stub, which must outlive the RPC
  /// @param requests every request to send, in order: moved in when a std::vector<RequestT> rvalue, copied
  ///        otherwise
  /// @param context of the RPC, for a deadline or metadata: a fresh one by default
  template <class StubT, std::ranges::input_range RangeT>
  requires std::constructible_from<typename Traits::RequestT, std::ranges::range_reference_t<RangeT>>
  Source(Executor& executor, StubT& stub, RangeT&& requests,
         std::unique_ptr<grpc::ClientContext> context = std::make_unique<grpc::ClientContext>())
      : executor_(executor),
        requests_(ToVector(std::forward<RangeT>(requests))),
        reactor_(std::make_unique<ReactorT>(stub, std::move(context), MakeCallbacks())) {}

  /// Cancels an RPC still in flight, which stops the pulls, and destroys the reactor once its OnDone() has
  /// run.
  ~Source() {
    done_.Abandon();
    DestroyAfterOnDone(reactor_, kOnDoneTimeout);
  }

  Source(const Source&) = delete;
  Source& operator=(const Source&) = delete;
  Source(Source&&) = delete;
  Source& operator=(Source&&) = delete;

  /// Sends a best-effort cancel. Callable from the application thread at any time: the reactor pulls no
  /// further request, and Finish() returns CANCELLED.
  void TryCancel() {
    if (reactor_) reactor_->TryCancel();
  }

  /// Awaited exactly once: resumes on the application thread once the RPC has ended, every request sent
  /// unless it failed or was cancelled.
  /// @return an awaitable whose co_await gives the ResultT
  [[nodiscard]] auto Finish() noexcept {
    struct Awaiter {
      Source& source;
      static bool await_ready() noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> waiter) const noexcept { return source.done_.Suspend(waiter); }
      ResultT await_resume() const {
        source.done_.Resumed();
        return Traits::TakeResult(source.reactor_, source.pushed_);
      }
    };
    assert(reactor_ && "Finish() is awaited exactly once");
    return Awaiter{*this};
  }

 private:
  /// The requests, as a vector the pulls move from.
  template <class RangeT>
  static std::vector<RequestT> ToVector(RangeT&& requests) {
    if constexpr (std::is_same_v<std::remove_cvref_t<RangeT>, std::vector<RequestT>>) {
      return std::vector<RequestT>(std::forward<RangeT>(requests));
    } else {
      std::vector<RequestT> copied;
      for (auto&& request : std::forward<RangeT>(requests)) copied.emplace_back(request);
      return copied;
    }
  }

  /// The level's own code. next_request runs once on the constructing thread, inside the reactor's
  /// constructor, then on a gRPC thread after each completed write, never two at once, and moves the next
  /// request out. The done callback records the response under kPush, then hands the coroutine over.
  typename Traits::Callbacks MakeCallbacks() {
    typename Traits::Callbacks cbs;
    cbs.next_request = [this](auto*) -> std::optional<RequestT> {
      if (next_ == requests_.size()) return std::nullopt;  // the reactor closes the request stream
      return std::move(requests_[next_++]);
    };
    cbs.done = Traits::DoneCallback(executor_, done_, pushed_);
    return cbs;
  }

  Executor& executor_;
  ResumeHandover done_;
  // The reactor's from its construction: only next_request reads them.
  std::vector<RequestT> requests_;
  std::size_t next_ = 0;
  std::unique_ptr<typename Traits::ResponseT> pushed_;  // Delivery::kPush only, set by the done callback
  std::unique_ptr<ReactorT> reactor_;  // declared last, so the callbacks' targets exist before the RPC starts
};

}  // namespace RpcReactor::coro
