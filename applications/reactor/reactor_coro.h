///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <grpcpp/grpcpp.h>

#include <cassert>
#include <chrono>
#include <coroutine>
#include <memory>
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
/// @tparam MessageT ResponseT under Delivery::kPull, std::unique_ptr<ResponseT> under Delivery::kPush
template <class MessageT>
struct UnaryResult {
  grpc::Status status;  ///< The RPC's outcome
  /// The response. Under Delivery::kPull it is swapped out of the reactor, and is empty unless the status is
  /// OK. Under Delivery::kPush it is the reactor's own message, never null, and empty on a failed RPC, so the
  /// status is what says whether its content is meaningful.
  MessageT response;
};

/// A unary RPC a coroutine awaits: the request is sent on construction, and `co_await Finish()` resumes the
/// coroutine on the application thread once the RPC has ended, with its status and its response.
///
///     coro::Unary<routeguide::GetFeature::ClientReactor<Delivery::kPull>> call{executor, stub, point};
///     auto [status, feature] = co_await call.Finish();
///
/// The response is always owned by the coroutine. Delivery decides how it becomes owned:
///
/// | Delivery | Finish() returns the response as  | How                                          |
/// |----------|-----------------------------------|----------------------------------------------|
/// | kPull    | ResponseT                         | Swapped out of the reactor by GetResponse()  |
/// | kPush    | std::unique_ptr<ResponseT>        | The message the done callback hands over     |
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
  /// What Finish() returns the response as.
  using MessageT = std::conditional_t<kPull, ResponseT, std::unique_ptr<ResponseT>>;
  /// What Finish() returns.
  using ResultT = UnaryResult<MessageT>;

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
      result.response = std::move(pushed_);
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
/// nothing once the stream has ended, and `co_await Finish()` then gives the RPC's status.
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
/// | Delivery | Read() gives                | How                                   | Next read armed            |
/// |----------|-----------------------------|---------------------------------------|----------------------------|
/// | kPull    | std::optional<ResponseT>    | Swapped out of the reactor            | At once, by GetResponse()  |
/// | kPush    | std::unique_ptr<ResponseT>  | The message the read callback hands   | At the next Read(), only   |
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
  using MessageT = std::conditional_t<kPull, std::optional<ResponseT>, std::unique_ptr<ResponseT>>;

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

  /// Awaited exactly once, once Read() has given nothing: resumes on the application thread once the RPC
  /// has ended.
  /// @return an awaitable whose co_await gives the RPC's status
  [[nodiscard]] auto Finish() noexcept {
    struct Awaiter {
      Reader& reader;
      static bool await_ready() noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> waiter) const noexcept { return reader.done_.Suspend(waiter); }
      grpc::Status await_resume() const { return reader.TakeStatus(); }
    };
    assert(reactor_ && "Finish() is awaited exactly once");
    assert(ended_ && "Finish() follows the Read() that gave nothing: the RPC cannot end while a message is held");
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
        read_.Rearm();
        if (!reactor_->ResumeRead()) ended_ = true;
        return std::move(pushed_);
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

}  // namespace RpcReactor::coro
