///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024 anderewrey
///

#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>  // uint64_t
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <type_traits>  // conditional_t
#include <utility>  // swap
#include <variant>  // monostate

/************************
 * gRPC Reactor: Following code belongs to the API implementation
 * It is generic as much as possible
 *
 * Active Object Pattern: Method Request & Future components
 * These generic reactor classes encapsulate RPC state (Method Request) and provide
 * asynchronous result access (Future). See reactor_client.md for detailed documentation.
 ************************/
namespace RpcReactor::Client {

/// Selects how a reactor hands a received message over to the application. Every reactor takes it
/// as a template parameter, with no default: neither approach is better in general, so the choice is
/// made per RPC from its constraints. See "Delivery approaches" in reactor_client.md.
enum class Delivery {
  /// The reactor keeps the message in its own storage. The callback is lent it, and the application
  /// pulls it out later through GetResponse() when the stream is held for it.
  kPull,
  /// The reactor moves the message out as an owned std::unique_ptr into the callback, and never refers
  /// to it again.
  kPush,
};

/// Selects who arms the read that follows a delivered message on a reading reactor, which is how much
/// the stream waits for the application. It is the second axis next to Delivery, independent of it, so
/// every combination of the two exists. The modes are declared along that one scale, from a stream that
/// never waits to one that always does. kPerMessage, in the middle, is the default: it reaches both
/// ends at runtime, one message at a time. kContinuous and kTurnByTurn fix one end for the whole RPC,
/// for a design that has already made that decision, and the compiler then enforces it. See "Read
/// pacing modes" in reactor_client.md.
enum class ReadPacing {
  /// Never waits: the reactor arms the next read itself, as the last statement of the reaction, so it
  /// takes no hold, and no release call exists to forget or to misplace. The stream never waits for the
  /// application, which costs an unbounded queue between them. Under kPull the callback is only lent the
  /// message until it returns, so it must copy out whatever it keeps.
  kContinuous,
  /// Waits when the read_ok callback says so, for each message: it returns true to hold the stream as
  /// under kTurnByTurn, or false to let the reactor read on at once as under kContinuous. The reactor
  /// cannot know the answer before calling the callback, so it takes a hold for every message and
  /// releases it itself when the callback declines. The application then owes a release call for each
  /// kept message, and none for a declined one.
  kPerMessage,
  /// Always waits: the reactor holds the RPC and arms nothing. The application releases it with
  /// GetResponse() (kPull) or ResumeRead() (kPush), once for every message, which bounds the stream to
  /// one message in flight and stalls it meanwhile.
  kTurnByTurn,
};

/// Selects who starts the transfer of each outgoing message on a writing reactor, which is the push/pull
/// of the outgoing direction, as Delivery is that of the incoming one. Every writing reactor takes it as a
/// template parameter, with no default, for the same reason as Delivery. It has no pacing counterpart:
/// gRPC accepts one write in flight at a time, so nothing is left to choose about when the next one may
/// start, only about who has the message once the previous one completed. See "The model behind the
/// reactors' choices" in docs/architecture.md.
enum class Submission {
  /// The application starts each transfer, by calling SendRequest(), SendLastRequest() or
  /// CloseRequestStream() from any thread, whenever it has a message ready. The reactor rejects a write
  /// while another is in flight, so the application keeps its surplus and calls again after write_done.
  /// Since those calls come from outside a reaction, the reactor holds the RPC while one may still come.
  kApplicationPushes,
  /// The reactor starts each transfer, by asking the next_request callback for the next message, once
  /// when the RPC starts and again each time a write completes, and closes the request stream when the
  /// callback has none. Every write is then issued by the reactor itself, so it takes no hold, and the
  /// application has no write call to make.
  kReactorPulls,
};

namespace detail {
/// What a read_ok callback receives: the message lent in place (Delivery::kPull), or given as an owned
/// object (Delivery::kPush).
template <class ResponseT, Delivery kDelivery>
using DeliveredMessage =
    std::conditional_t<kDelivery == Delivery::kPull, const ResponseT&, std::unique_ptr<ResponseT>>;

/// What a read_ok callback returns: the hold decision under ReadPacing::kPerMessage, nothing otherwise,
/// since the other pacings are fixed for the whole RPC.
template <ReadPacing kPacing>
using ReadOkResult = std::conditional_t<kPacing == ReadPacing::kPerMessage, bool, void>;

/// The next_request slot of a writing reactor's callbacks: a callback under Submission::kReactorPulls, and an
/// empty placeholder under kApplicationPushes, which has nobody to call it. Binding a callback to the
/// placeholder does not compile, so the choice cannot be half made.
template <Submission kSubmission, class CallbackT>
using NextRequestSlot = std::conditional_t<kSubmission == Submission::kReactorPulls, CallbackT, std::monostate>;

/// Records that a reactor's OnDone() has run, for CancelAndWait(). OnDone() signals it through a copy of the
/// reactor's std::shared_ptr taken on entry, so the latch outlives the reactor when the waiter destroys the
/// reactor as soon as it wakes, while OnDone() is still returning.
class DoneLatch {
 public:
  /// Called on entry to OnDone(): records its thread, so a wait from inside OnDone(), such as from the
  /// done callback, returns at once instead of waiting for itself.
  void Enter() { on_done_thread_ = std::this_thread::get_id(); }

  /// Called as OnDone()'s last action, through the caller's own copy of the latch.
  void Signal() {
    {
      std::lock_guard lock(mutex_);
      done_ = true;
    }
    cv_.notify_all();
  }

  /// @param timeout how long to wait for OnDone() to run
  /// @return true once OnDone() has run to its end, or when called from inside OnDone(); false on a timeout
  bool WaitFor(std::chrono::milliseconds timeout) {
    if (on_done_thread_.load() == std::this_thread::get_id()) return true;
    std::unique_lock lock(mutex_);
    return cv_.wait_for(lock, timeout, [this] { return done_; });
  }

 private:
  std::mutex mutex_;
  std::condition_variable cv_;
  bool done_{false};
  std::atomic<std::thread::id> on_done_thread_{};
};
}  // namespace detail

/// Template callbacks for unary RPC client reactor. It contains all available callbacks slots
/// needed by specialized RPC client reactors.
/// @tparam ResponseT type of protobuf message the RPC handles
/// @tparam kDelivery how the reactor hands the response over, which sets the done slot's signature
template <class ResponseT, Delivery kDelivery>
requires std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveUnaryCallbacks {
  /// Function signature for ClientUnaryReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// - Delivery::kPull: the response stays in the reactor, and is read later through GetResponse().
  /// - Delivery::kPush: the response ownership transfers to the callback, and carries the obligations documented in
  ///   reactor_client.md, under "Message ownership". It is never null: a failed RPC yields an empty one, so the
  ///   status is what says whether the content is meaningful. Stays in the reactor if this slot is unbound.
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  /// @param response the received message, lent (kPull) or given (kPush) to the callback
  using OnDoneCallback = std::conditional_t<
      kDelivery == Delivery::kPull,
      std::function<void(grpc::ClientUnaryReactor*, const grpc::Status&, const ResponseT&)>,
      std::function<void(grpc::ClientUnaryReactor*, const grpc::Status&, std::unique_ptr<ResponseT>)>>;
  OnDoneCallback done;  ///< Slot for ClientUnaryReactor::OnDone event
};

/// template class for unary RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides Status(), and
/// GetResponse() under Delivery::kPull)
/// @tparam ResponseT type of protobuf message the RPC handles
/// @tparam kDelivery how the reactor hands the response over, see Delivery
template <class ResponseT, Delivery kDelivery>
requires std::derived_from<ResponseT, google::protobuf::Message>
class ActiveUnaryReactor : public grpc::ClientUnaryReactor {
 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ActiveUnaryReactor(std::unique_ptr<grpc::ClientContext> context,
                     ActiveUnaryCallbacks<ResponseT, kDelivery>&& cbs)
      : context_(std::move(context)),
        cbs_(std::move(cbs)) {}

  /// Destructor of the reactor class. It tells the gRPC connection to close the channel.
  /// If the context/channel is already closed, there's no problem to TryCancel() it again.
  ~ActiveUnaryReactor() override {
    context_->TryCancel();
  }

  /// This class cannot be copied.
  ActiveUnaryReactor(const ActiveUnaryReactor&) = delete;
  /// This class cannot be copied.
  ActiveUnaryReactor& operator=(const ActiveUnaryReactor&) = delete;
  /// This class cannot be moved.
  ActiveUnaryReactor(ActiveUnaryReactor&&) = delete;
  /// This class cannot be moved.
  ActiveUnaryReactor& operator=(ActiveUnaryReactor&&) = delete;

  /// Sends a best-effort out-of-band cancel to the RPC. That signal is thread-safe
  /// and can be sent anytime from any thread. The goal of that signal is to provoke
  /// the `OnDone` event from the RPC.
  void TryCancel() {
    // (Point 2.2, 2.3) RPC termination behest
    context_->TryCancel();
  }

  /// Delivery::kPull only. Swaps the underlying data storage of the response object.
  /// The swap mechanism is important to avoid a deep-copy of the content of the
  /// response. Having the response swapped is acceptable because the unary RPC
  /// is meant to one response only, so the swap is a good technique to speed up
  /// the response proceeding time.
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) requires(kDelivery == Delivery::kPull) {
    if (!response_ready_) return false;
    // (Point 3.6) extracts response
    swap(*response_, response);  // Moving the read content on the user side
    response_ready_ = false;
    return true;
  }

  /// Cancels the RPC and waits until OnDone() has run, so the reactor can be destroyed at once. For an
  /// application that must tear a reactor down before its RPC ends, such as at shutdown, once its event
  /// loop no longer runs the OnDone handler. Destroying a reactor whose OnDone() has not run is a
  /// use-after-free, since gRPC still calls it: the destructor's cancel does not wait.
  /// Must not be called from a reaction other than OnDone(), which could not run meanwhile. From inside
  /// OnDone(), such as from the done callback, it returns at once.
  /// @param timeout how long to wait for OnDone()
  /// @return true once OnDone() has run, so the reactor may be destroyed; false on a timeout, when it may
  ///         not be
  bool CancelAndWait(std::chrono::milliseconds timeout) {
    context_->TryCancel();
    return done_latch_->WaitFor(timeout);
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

 protected:
  /// This event function is called by gRPC when the RPC is done. The OnDoneCallback
  /// callback is then called, but on the same gRPC thread. The received status
  /// is also copied into the reactor.
  /// This OnDone is also the relevent event acknowledging the response reception.
  /// @param status info coming from gRPC
  void OnDone(const grpc::Status& status) override {
    // Copied, so the latch outlives this reactor if a waiter destroys it once signalled.
    const auto latch = done_latch_;
    latch->Enter();
    // (Point 3.1, 3.2, 3.3) RPC termination
    if constexpr (kDelivery == Delivery::kPull) {
      response_ready_ = status.ok();
      if (cbs_.done) {
        status_ = status;  // doing deep-copy unfortunately
        // (Point 3.4) TriggerEvent: OnDone
        cbs_.done(this, status, *response_);
      }
    } else {
      if (cbs_.done) {
        status_ = status;  // doing deep-copy unfortunately
        // (Point 3.4) TriggerEvent: OnDone, handing the response ownership over
        cbs_.done(this, status, std::move(response_));
      }
    }
    latch->Signal();  // Last action: the reactor may be destroyed from here on
  }

  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  /// Read target. Under Delivery::kPull it holds the response until GetResponse() swaps it out; under
  /// Delivery::kPush it is moved out on OnDone.
  std::unique_ptr<ResponseT> response_{std::make_unique<ResponseT>()};

 private:
  grpc::Status status_;
  // Signalled once OnDone() has run, for CancelAndWait().
  std::shared_ptr<detail::DoneLatch> done_latch_{std::make_shared<detail::DoneLatch>()};
  ActiveUnaryCallbacks<ResponseT, kDelivery> cbs_;

  // Delivery::kPull only.
  // The application MAY call (but should not) GetResponse() while a gRPC thread is on OnDone().
  // That concurrent situation should not happen by design, unless the application
  // is not waiting for the OnDoneCallback prior reading the response_;
  // Once response_ready_ is set, the response_ may be thread-safely used by the application.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool response_ready_{false};
};

/// Template callbacks for stream-reader RPC client reactor. It contains all available callbacks slots
/// needed by specialized RPC client reactors.
/// @tparam ResponseT type of protobuf message the RPC handles
/// @tparam kDelivery how the reactor hands each message over, which sets the read_ok slot's parameter
/// @tparam kPacing who arms the next read, which sets the read_ok slot's return type
template <class ResponseT, Delivery kDelivery, ReadPacing kPacing = ReadPacing::kPerMessage>
requires std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveReadCallbacks {
  /// Function signature for ClientReadReactor::OnReadDone event with positive OK flag. Its parameter
  /// follows the Delivery, and its return type the ReadPacing.
  ///
  /// Delivery::kPull: the callback is lent the message, which stays in the reactor. Under kContinuous the
  /// next read overwrites it as soon as the callback returns, so anything worth keeping must be copied out
  /// first; the same holds for a message declined under kPerMessage.
  /// Delivery::kPush: the reactor swaps the message out before this call, so the callback owns the message
  /// and the reactor never refers to it again. Handing that ownership on carries obligations documented
  /// in reactor_client.md, under "Message ownership".
  ///
  /// ReadPacing::kContinuous: returns nothing, and the reactor arms the next read as soon as it returns.
  /// ReadPacing::kTurnByTurn: returns nothing, and the reactor arms none until GetResponse() (kPull) or
  /// ResumeRead() (kPush) is called.
  /// ReadPacing::kPerMessage: returns true to hold the stream as under kTurnByTurn, or false to let the
  /// reactor read on at once as under kContinuous.
  ///
  /// An unbound slot counts as declining every message: the reactor reads on, and takes no hold.
  /// @param reactor instance pointer on which the event is received
  /// @param response the received message, lent (kPull) or owned by the callback (kPush)
  using OnReadDoneOkCallback = std::function<detail::ReadOkResult<kPacing>(
      grpc::ClientReadReactor<ResponseT>*, detail::DeliveredMessage<ResponseT, kDelivery>)>;
  OnReadDoneOkCallback read_ok;  ///< Slot for ClientReadReactor::OnReadDone event with positive OK flag

  /// Function signature for ClientReadReactor::OnReadDone event with negative OK flag
  /// @param reactor instance pointer on which the event is received
  using OnReadDoneNOkCallback = std::function<void(grpc::ClientReadReactor<ResponseT>*)>;
  OnReadDoneNOkCallback read_nok;  ///< Slot for ClientReadReactor::OnReadDone event with negative OK flag

  /// Function signature for ClientReadReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  using OnDoneCallback = std::function<void(grpc::ClientReadReactor<ResponseT>*, const grpc::Status&)>;
  OnDoneCallback done;  ///< Slot for ClientReadReactor::OnDone event
};

/// Template class for stream-reader RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides Status(), and
/// GetResponse() under Delivery::kPull)
/// @tparam ResponseT type of protobuf message the RPC handles
/// @tparam kDelivery how the reactor hands each message over, see Delivery
/// @tparam kPacing who arms the read that follows a delivered message, see ReadPacing
template <class ResponseT, Delivery kDelivery, ReadPacing kPacing = ReadPacing::kPerMessage>
requires std::derived_from<ResponseT, google::protobuf::Message>
class ActiveReadReactor : public grpc::ClientReadReactor<ResponseT> {
 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ActiveReadReactor(std::unique_ptr<grpc::ClientContext> context,
                    ActiveReadCallbacks<ResponseT, kDelivery, kPacing>&& cbs)
      : context_(std::move(context)),
        cbs_(std::move(cbs)) {}

  /// Destructor of the reactor class. It tells the gRPC connection to close the channel.
  /// If the context/channel is already closed, there's no problem to TryCancel() it again.
  ~ActiveReadReactor() override {
    context_->TryCancel();
  }

  /// This class cannot be copied nor moved
  ActiveReadReactor(const ActiveReadReactor&) = delete;
  /// This class cannot be copied nor moved
  ActiveReadReactor& operator=(const ActiveReadReactor&) = delete;
  /// This class cannot be copied nor moved
  ActiveReadReactor(ActiveReadReactor&&) = delete;
  /// This class cannot be copied nor moved
  ActiveReadReactor& operator=(ActiveReadReactor&&) = delete;

  /// Sends a best-effort out-of-band cancel to the RPC. That signal is thread-safe
  /// and can be sent anytime from any thread. The goal of that signal is to provoke
  /// the `OnDone` event from the RPC.
  void TryCancel() {
    // (Point 3.2, 3.3) RPC termination behest
    context_->TryCancel();
  }

  /// Delivery::kPull only, under ReadPacing::kPerMessage or kTurnByTurn. Swaps the underlying data storage of the
  /// response object and resume the held RPC.
  /// The swap mechanism is important to avoid a deep-copy of the content of the
  /// response. Having the response swapped is acceptable because the stream-reader
  /// RPC is meant to be overwritten at each received response, so the swap is a
  /// good technique to speed up the response proceeding time.
  /// RemoveHold() is always called once a hold was added (by OnReadDone()'s AddHold()), whether or
  /// not reading is restarted. gRPC's hold count is a single, per-RPC counter (not one per read/
  /// write direction) that only gates OnDone() - it must be released exactly once per AddHold(),
  /// or the RPC stalls forever, even if stream_no_more_ became true while the hold was held.
  /// Not offered under ReadPacing::kContinuous, where the next read overwrites the message as soon as the
  /// callback returns, so there is nothing left to pull.
  /// Like ResumeRead(), it takes whichever message is held, so it is called once per message the callback
  /// kept. It can run before the callback returns, so handing the reactor on is the callback's last use of
  /// the lent message.
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) requires(kDelivery == Delivery::kPull && kPacing != ReadPacing::kContinuous) {
    // Claiming the token is what makes a duplicate or spurious call safe, as in ResumeRead(): only the
    // caller that takes the message away from OnReadDone() goes on to release the hold.
    if (response_ready_.exchange(0) == 0) return false;
    // (Point 2.9, 2.10) extracts response
    swap(response_, response);  // Moving the read content on the user side
    if (!stream_no_more_) {
      // (Point 2.11, 2.12) Restart reading
      this->StartRead(&response_);
    }
    // (Point 2.13) Resuming RPC - must run regardless of whether reading restarted, see above.
    this->RemoveHold();
    return true;
  }

  /// Delivery::kPush only, under ReadPacing::kPerMessage or kTurnByTurn. Arms the read that OnReadDone()
  /// left unarmed, and releases the hold it took. The application calls this once per held message, when
  /// it no longer needs the stream stalled. Until it does, the RPC cannot terminate: the hold suppresses
  /// OnDone(), and no outstanding read is left to report the stream ending, so an application that
  /// drops a held message without resuming strands the RPC and can never legally destroy the reactor.
  /// Not offered under ReadPacing::kContinuous, where the reactor arms its own reads and takes no hold.
  /// It releases whichever message is held at that moment, not a particular one: call it exactly once per
  /// message the callback kept (each one under kTurnByTurn, those it returned true for under kPerMessage),
  /// and never for a declined one, which may release the next message while its read_ok is still deciding.
  /// @return true when a read was armed. false when there was no hold to release (no message is
  ///         currently held), or when the stream is already over
  bool ResumeRead() requires(kDelivery == Delivery::kPush && kPacing != ReadPacing::kContinuous) {
    // Claiming the token is what makes a duplicate or spurious call safe: RemoveHold() below is
    // reached only by the caller that took the hold away from OnReadDone(), never twice, so the
    // gRPC outstanding-callback count cannot underflow into an early OnDone().
    if (read_held_.exchange(0) == 0) return false;
    // StartRead() before RemoveHold(), so the outstanding-callback count never reaches zero between
    // the two and lets OnDone() fire while this call is still running.
    const bool armed = !stream_no_more_;
    // (Point 2.10, 2.11) Restart reading
    if (armed) this->StartRead(&response_);
    // (Point 2.12) Resuming RPC. Must run exactly once per AddHold(), armed or not, or the RPC
    // stalls forever.
    this->RemoveHold();
    return armed;
  }

  /// Cancels the RPC and waits until OnDone() has run, so the reactor can be destroyed at once. For an
  /// application that must tear a reactor down before its RPC ends, such as at shutdown, once its event
  /// loop no longer runs the OnDone handler. Destroying a reactor whose OnDone() has not run is a
  /// use-after-free, since gRPC still calls it: the destructor's cancel does not wait. A message still held is
  /// released as by GetResponse() or ResumeRead(), without arming a read, since the application tearing
  /// down will not make that call.
  /// Must not be called from a reaction other than OnDone(), which could not run meanwhile. From inside
  /// OnDone(), such as from the done callback, it returns at once.
  /// @param timeout how long to wait for OnDone()
  /// @return true once OnDone() has run, so the reactor may be destroyed; false on a timeout, when it may
  ///         not be
  bool CancelAndWait(std::chrono::milliseconds timeout) {
    context_->TryCancel();
    // Published before the held message is claimed, so a release call racing this one arms no read.
    stream_no_more_ = true;
    ReleaseHeldRead();
    return done_latch_->WaitFor(timeout);
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

 protected:
  /// This event function is called by gRPC when the stream has an event. The user-side
  /// callback is then called, but on the same gRPC thread.
  /// This OnReadDone is also the relevant event acknowledging the response reception.
  /// Based on the value of the `ok` flag, the OnReadDoneOkCallback or OnReadDoneNOkCallback
  /// is called. A received message is handed over by OnReadDoneOk(), according to the Delivery and the
  /// ReadPacing the reactor was instantiated with.
  /// @param ok true: a response is received. false: the stream reader is closed
  ///           (but not the RPC itself).
  void OnReadDone(const bool ok) override {
    // (Point 2.1, 2.2, 2.3, 4.1, 4.2) Event received from stream
    if (!ok) {
      stream_no_more_ = true;
      // (Point 4.2) OnReadDone: False
      if (cbs_.read_nok) cbs_.read_nok(this);
      return;
    }
    OnReadDoneOk();
  }
  /// This event function is called by gRPC when the RPC is done and no more operation is possible with that reactor
  /// instance. The OnDoneCallback is then called, but on the same gRPC thread. The received status
  /// is also copied into the reactor.
  /// @param status info coming from gRPC
  void OnDone(const grpc::Status& status) override {
    const auto latch = done_latch_;  // As in ActiveUnaryReactor::OnDone()
    latch->Enter();
    // (Point 4.4, 4.5) RPC termination
    stream_no_more_ = true;
    if (cbs_.done) {
      status_ = status;  // doing deep-copy unfortunately
      // (Point 4.5) OnDone
      cbs_.done(this, status);
    }
    latch->Signal();
  }

  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  /// Read target. Under Delivery::kPull it holds the response until GetResponse() swaps it out; under
  /// Delivery::kPush it is swapped out on each read, and never exposed.
  ResponseT response_;

 private:
  /// Hands a received message over, in two independent steps. The Delivery sets what the read_ok
  /// callback receives, and the ReadPacing sets who arms the next read.
  void OnReadDoneOk() {
    // (Point 2.3) OnReadDone: true
    // An unbound slot has nobody to keep the message or to release a hold, so the reactor drains the
    // stream itself. Re-arming from inside the reaction needs no hold.
    if (!cbs_.read_ok) {
      this->StartRead(&response_);
      return;
    }
    // (Point 2.4) Step 1, Delivery: prepare what the callback receives. Done before any hold is
    // published, so a ResumeRead() racing ahead cannot arm a read into response_ while it is still
    // being swapped out.
    auto message = TakeMessage();
    // Step 2, ReadPacing: who arms the next read. The diagrams in reactor_client.md draw kPerMessage,
    // whose two branches are what the two other modes do for every message: kTurnByTurn the kept one,
    // and kContinuous the declined one without its hold. Points in that declined branch are numbered
    // differently in the kPull and kPush diagrams, so both are cited, kPull first.
    if constexpr (kPacing == ReadPacing::kContinuous) {
      // (Point 2.17 / 2.14) TriggerEvent: OnReadDoneOk
      HandOver(message);
      // (Point 2.19 / 2.15) Restart reading. Last action of the reaction, which is what removes the need
      // for a hold. See "Read pacing modes" in reactor_client.md.
      this->StartRead(&response_);
    } else {
      // (Point 2.5) Holding the RPC. Taken before the callback, never after: the callback is what hands
      // the message off, so GetResponse() or ResumeRead(), and their RemoveHold(), can run on the
      // application thread before the callback even returns. See "Hold before handing off, not after"
      // in reactor_client.md, and https://github.com/grpc/grpc/pull/18072 for why the hold exists.
      this->AddHold();
      // Published only once the hold is taken: the flag is what the releasing call claims, so setting
      // it is already part of the hand-off.
      [[maybe_unused]] const auto held = PublishHeld();
      if constexpr (kPacing == ReadPacing::kPerMessage) {
        // (Point 2.6, or 2.17 / 2.14 when declined) TriggerEvent: OnReadDoneOk. Held if the callback keeps
        // the message.
        if (HandOver(message)) return;
        ReleaseDeclined(held);
      } else {
        // (Point 2.6) TriggerEvent: OnReadDoneOk. The application owns the re-arm from here.
        HandOver(message);
      }
    }
  }

  /// Delivery step: kPull lends response_ in place, kPush swaps it into a new owned message, by pointer
  /// swap rather than deep-copy.
  auto TakeMessage() {
    if constexpr (kDelivery == Delivery::kPull) {
      return &std::as_const(response_);
    } else {
      auto message = std::make_unique<ResponseT>();
      swap(*message, response_);
      return message;
    }
  }

  /// Invokes the read_ok callback with what TakeMessage() prepared.
  /// @return the callback's hold decision under ReadPacing::kPerMessage, nothing otherwise
  template <class MessageT>
  detail::ReadOkResult<kPacing> HandOver(MessageT& message) {
    if constexpr (kDelivery == Delivery::kPull) {
      return cbs_.read_ok(this, *message);
    } else {
      return cbs_.read_ok(this, std::move(message));
    }
  }

  /// Publishes the hold OnReadDone() took, for the call that will release it to claim. Each held message
  /// gets its own non-zero token, so a claim can only ever take the hold of the message it was made for.
  /// @return the token, for ReleaseDeclined()
  std::uint64_t PublishHeld() {
    const auto held = ++held_count_;
    if constexpr (kDelivery == Delivery::kPull) {
      response_ready_ = held;
    } else {
      read_held_ = held;
    }
    return held;
  }

  /// ReadPacing::kPerMessage, after the callback declined the message: claims the hold back, then
  /// restarts reading and releases it, since no GetResponse() or ResumeRead() will. A lost claim means
  /// the application already released the hold itself, against the callback's own answer, so there is
  /// nothing left to do. Either way the hold is released exactly once.
  /// The claim is made with the message's own token, not a plain flag: once the application releases
  /// the hold, the next message can be read, held and published on another gRPC thread while this
  /// callback is still deciding. A plain flag would let this claim take that next message's hold.
  /// @param held the token PublishHeld() returned for this message
  void ReleaseDeclined(std::uint64_t held) {
    auto& flag = (kDelivery == Delivery::kPull) ? response_ready_ : read_held_;
    if (!flag.compare_exchange_strong(held, 0)) return;
    // (Point 2.18, 2.19 / 2.15) Restart reading, before the release, which keeps the RPC alive on its own
    this->StartRead(&response_);
    // (Point 2.20 / 2.16) Resuming RPC
    this->RemoveHold();
  }

  /// For CancelAndWait(): releases the hold of a message still held, without arming a read. Claims it with
  /// the same token GetResponse() and ResumeRead() claim, so exactly one of them releases it.
  void ReleaseHeldRead() {
    auto& flag = (kDelivery == Delivery::kPull) ? response_ready_ : read_held_;
    if (flag.exchange(0) != 0) this->RemoveHold();
  }

  grpc::Status status_;
  // Signalled once OnDone() has run, for CancelAndWait().
  std::shared_ptr<detail::DoneLatch> done_latch_{std::make_shared<detail::DoneLatch>()};
  ActiveReadCallbacks<ResponseT, kDelivery, kPacing> cbs_;

  // Under ReadPacing::kPerMessage or kTurnByTurn: the number of messages held so far, which gives each
  // held message its token. Only touched by OnReadDone(), whose successive runs are ordered by the
  // StartRead() between them.
  std::uint64_t held_count_{0};

  // Delivery::kPush, under ReadPacing::kPerMessage or kTurnByTurn: the token of the held message whose
  // hold is outstanding and still unclaimed, waiting for the ResumeRead() that releases it, or 0 when
  // none is. Claiming it atomically is what makes a duplicate or spurious ResumeRead() a no-op rather
  // than an underflow while nothing is held.
  // Set by gRPC thread, cleared by whichever thread claims it.
  std::atomic<std::uint64_t> read_held_{0};

  // Delivery::kPull, under ReadPacing::kPerMessage or kTurnByTurn: the token of the held message waiting
  // in response_ for GetResponse(), and with it an unclaimed hold, or 0 when none is. Claimed the same
  // way as read_held_. Once claimed by GetResponse(), the response_ may be thread-safely used by the
  // application.
  // Set by gRPC thread, cleared by whichever thread claims it.
  std::atomic<std::uint64_t> response_ready_{0};

  // Once we got OnReadDone(false) or OnDone(), no more StartRead() must be called. Only read by
  // GetResponse() and ResumeRead(), since every other StartRead() is issued from inside a reaction.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool stream_no_more_{false};
};

/// Template callbacks for stream-writer RPC client reactor. It contains all available callbacks slots
/// needed by specialized RPC client reactors.
/// @tparam RequestT type of protobuf message the RPC sends
/// @tparam ResponseT type of protobuf message the RPC receives as final response
/// @tparam kDelivery how the reactor hands the final response over, which sets the done slot's signature
/// @tparam kSubmission who starts the transfer of each request
template <class RequestT, class ResponseT, Delivery kDelivery, Submission kSubmission>
requires std::derived_from<RequestT, google::protobuf::Message> &&
         std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveWriteCallbacks {
  /// Function signature for ClientWriteReactor::OnWriteDone event
  /// @param reactor instance pointer on which the event is received
  /// @param ok true if the write was successful, false otherwise
  using OnWriteDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>*, bool ok)>;
  OnWriteDoneCallback write_done;  ///< Slot for ClientWriteReactor::OnWriteDone event

  /// Function signature for the reactor pulling the next request, under Submission::kReactorPulls only.
  /// Called once while the reactor starts the RPC, which is inside its constructor, on the thread that
  /// constructs it, so before the application holds the reactor. Then called again on a gRPC thread after
  /// each successful write, right after write_done. Never called twice at once, and never after a write
  /// failed or the RPC was cancelled. It must answer at once, without blocking, since it runs inside a
  /// reaction: a request to send it, or std::nullopt to close the request stream. It cannot answer "not
  /// yet": a source that may have nothing ready while it is not over calls for kApplicationPushes. An
  /// unbound slot counts as a source with nothing to send.
  /// @param reactor instance pointer on which the request will be written
  /// @return the next request, given to the reactor, or std::nullopt when there is none
  using NextRequestCallback = std::function<std::optional<RequestT>(grpc::ClientWriteReactor<RequestT>*)>;
  /// Slot for the reactor pulling the next request. Under kApplicationPushes it is an empty placeholder.
  [[no_unique_address]] detail::NextRequestSlot<kSubmission, NextRequestCallback> next_request;

  /// Function signature for ClientWriteReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// - Delivery::kPull: the response stays in the reactor, and is read later through GetResponse().
  /// - Delivery::kPush: the response ownership transfers to the callback, and carries the obligations documented in
  ///   reactor_client.md, under "Message ownership". It is never null: a failed RPC yields an empty one, so the
  ///   status is what says whether the content is meaningful. Stays in the reactor if this slot is unbound.
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  /// @param response the received message, lent (kPull) or given (kPush) to the callback
  using OnDoneCallback = std::conditional_t<
      kDelivery == Delivery::kPull,
      std::function<void(grpc::ClientWriteReactor<RequestT>*, const grpc::Status&, const ResponseT&)>,
      std::function<void(grpc::ClientWriteReactor<RequestT>*, const grpc::Status&, std::unique_ptr<ResponseT>)>>;
  OnDoneCallback done;  ///< Slot for ClientWriteReactor::OnDone event
};

/// Template class for stream-writer RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides Status(), and
/// GetResponse() under Delivery::kPull)
/// @tparam RequestT type of protobuf message the RPC sends
/// @tparam ResponseT type of protobuf message the RPC receives as final response
/// @tparam kDelivery how the reactor hands the final response over, see Delivery
/// @tparam kSubmission who starts the transfer of each request, see Submission
template <class RequestT, class ResponseT, Delivery kDelivery, Submission kSubmission>
requires std::derived_from<RequestT, google::protobuf::Message> &&
         std::derived_from<ResponseT, google::protobuf::Message>
class ActiveWriteReactor : public grpc::ClientWriteReactor<RequestT> {
  static constexpr bool kPushes = kSubmission == Submission::kApplicationPushes;

 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ActiveWriteReactor(std::unique_ptr<grpc::ClientContext> context,
                     ActiveWriteCallbacks<RequestT, ResponseT, kDelivery, kSubmission>&& cbs)
      : context_(std::move(context)),
        cbs_(std::move(cbs)) {}

  /// Destructor of the reactor class. It tells the gRPC connection to close the channel.
  /// If the context/channel is already closed, there's no problem to TryCancel() it again.
  ~ActiveWriteReactor() override {
    context_->TryCancel();
  }

  /// This class cannot be copied.
  ActiveWriteReactor(const ActiveWriteReactor&) = delete;
  /// This class cannot be copied.
  ActiveWriteReactor& operator=(const ActiveWriteReactor&) = delete;
  /// This class cannot be moved.
  ActiveWriteReactor(ActiveWriteReactor&&) = delete;
  /// This class cannot be moved.
  ActiveWriteReactor& operator=(ActiveWriteReactor&&) = delete;

  /// Activates the RPC. Shadows grpc::ClientWriteReactor::StartCall(), which the specialized reactor calls
  /// last in its constructor, so a new specialization cannot forget what this adds, which follows the
  /// Submission.
  /// Submission::kApplicationPushes: takes the hold that covers the whole write flow. gRPC destroys a call
  /// as soon as nothing is pending on it, before it even calls OnDone(), so a write issued from outside a
  /// reaction needs a hold, or it can reach a call that is already gone. This is the pattern gRPC
  /// documents for such operations (see AddHold() in grpcpp/support/client_callback.h): a hold taken
  /// before StartCall(), and released once no more writes will be issued. gRPC accepts a hold only before
  /// StartCall() or from inside a reaction, which is why it is taken here. The flow is published as open
  /// before StartCall(), so a reaction arriving right after it finds the hold claimable.
  /// Unlike ActiveBidiReactor's idle hold, this one cannot be released when the RPC ends on its own: a
  /// client-streaming RPC has no read stream, so no reaction reports a deadline or a server error while no
  /// write is in flight. The application must therefore end the write flow itself, with SendLastRequest(),
  /// CloseRequestStream() or TryCancel(), or by a write that fails. Until then, OnDone() cannot fire.
  /// Submission::kReactorPulls: pulls the first request, and issues its write, or the close when there is
  /// none, before the RPC starts, which gRPC allows. Every later operation is issued from OnWriteDone(),
  /// inside a reaction, so no hold is needed, and the RPC ends on its own.
  void StartCall() {
    if constexpr (kPushes) {
      this->AddHold();
      write_flow_ = kFlowOpen;
    } else {
      PullNextRequest();
    }
    grpc::ClientWriteReactor<RequestT>::StartCall();
  }

  /// Sends a request message asynchronously on the client stream.
  /// The write operation completes asynchronously and OnWriteDone() will be called.
  /// gRPC requires that only one write be in flight at a time, so this method
  /// returns false if a write is already pending. Callers should wait for
  /// OnWriteDone() before calling SendRequest() again.
  /// The caller gives up the request to the reactor: it must be a temporary or explicitly
  /// std::move()'d, since the reactor takes ownership and the caller must not keep using it.
  /// When the write flow is known to be over, because a write failed or TryCancel() was called, this
  /// call ends it and returns false, which releases the write-flow hold and lets OnDone() follow.
  /// Submission::kApplicationPushes only: under kReactorPulls the reactor issues every write itself.
  /// @param request message to send, moved into the reactor
  /// @return true if the write was initiated, false if rejected (write pending, write side closed, or
  ///         the RPC is over or cancelled)
  bool SendRequest(RequestT&& request) requires(kPushes) {
    // Winning the claim is what makes the StartWrite() below safe from any thread: the write-flow hold
    // stays outstanding while the claim is held, so the call cannot be destroyed under it. A lost claim
    // means the write side is already ended, or another call is using it at this very moment.
    if (!ClaimWriteFlow()) return false;
    if (write_pending_) {  // Write already in progress
      ReturnWriteFlow();
      return false;
    }
    // Checked after write_pending_, which OnWriteDone() clears only after publishing the flag: a write
    // that just failed is seen as failed here, never as merely completed.
    if (stream_no_more_) {
      EndWriteFlow();
      return false;
    }
    pending_request_ = std::move(request);
    write_pending_ = true;
    this->StartWrite(&pending_request_);
    ReturnWriteFlow();
    return true;
  }

  /// Sends the final request message and signals the end of the client request stream in a
  /// single operation, avoiding the race between a separate SendRequest()/CloseRequestStream()
  /// pair. The caller gives up the request the same way SendRequest() requires. It ends the write
  /// flow, so no further write call is accepted. Submission::kApplicationPushes only.
  /// @param request last message to send, moved into the reactor
  /// @return true if the write was initiated, false if rejected (write pending, write side closed, or
  ///         the RPC is over or cancelled, in which case the write flow ends anyway)
  bool SendLastRequest(RequestT&& request) requires(kPushes) {
    if (!ClaimWriteFlow()) return false;  // Same claim, same reasoning, as SendRequest()
    if (write_pending_) {
      ReturnWriteFlow();
      return false;
    }
    if (stream_no_more_) {
      EndWriteFlow();
      return false;
    }
    pending_request_ = std::move(request);
    write_pending_ = true;
    this->StartWriteLast(&pending_request_, grpc::WriteOptions());
    // No more writes will be issued, which is when gRPC's pattern releases the write-flow hold. The
    // write just started keeps the RPC alive until its OnWriteDone().
    EndWriteFlow();
    return true;
  }

  /// Signals the end of the client request stream, and ends the write flow.
  /// After this call, no more SendRequest() calls are allowed. Submission::kApplicationPushes only: under
  /// kReactorPulls the reactor closes the stream when next_request has nothing more.
  /// @return true if the close was initiated. false if rejected: a write is pending (wait for
  ///         OnWriteDone() and retry), the write side is already closed, or the RPC is over or
  ///         cancelled, in which case the write flow ends anyway and OnDone() follows.
  bool CloseRequestStream() requires(kPushes) {
    if (!ClaimWriteFlow()) return false;  // Already closed, or another call is using the write side
    if (write_pending_) {  // Wait for the in-flight write to complete first
      ReturnWriteFlow();
      return false;
    }
    if (stream_no_more_) {  // Nothing left to close: end the flow so that OnDone() can follow
      EndWriteFlow();
      return false;
    }
    this->StartWritesDone();
    // Released after StartWritesDone(), as gRPC's pattern prescribes. The close just started keeps the
    // RPC alive until its OnWritesDoneDone().
    EndWriteFlow();
    return true;
  }

  /// Sends a best-effort out-of-band cancel to the RPC, to provoke the `OnDone` event. Safe from any
  /// thread. Under Submission::kApplicationPushes, it also ends the write flow, since no write can succeed
  /// once the RPC is cancelled: an idle write side would otherwise keep the write-flow hold, and OnDone()
  /// would never come. A write call running on another thread at that moment ends the flow itself when it
  /// returns. Under kReactorPulls, it stops the reactor from pulling any further request.
  void TryCancel() {
    context_->TryCancel();
    // Published before the attempt to end the flow, for the reason given at ReturnWriteFlow().
    stream_no_more_ = true;
    if constexpr (kPushes) EndOpenWriteFlow();
  }

  /// Delivery::kPull only. Swaps the underlying data storage of the response object.
  /// The swap mechanism is important to avoid a deep-copy of the content of the
  /// response. Having the response swapped is acceptable because the client-streaming RPC
  /// is meant to receive one final response only, so the swap is a good technique to speed up
  /// the response proceeding time.
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) requires(kDelivery == Delivery::kPull) {
    if (!response_ready_) return false;
    swap(*response_, response);  // Moving the read content on the user side
    response_ready_ = false;
    return true;
  }

  /// Cancels the RPC and waits until OnDone() has run, so the reactor can be destroyed at once. For an
  /// application that must tear a reactor down before its RPC ends, such as at shutdown, once its event
  /// loop no longer runs the OnDone handler. Destroying a reactor whose OnDone() has not run is a
  /// use-after-free, since gRPC still calls it: the destructor's cancel does not wait. Under
  /// Submission::kApplicationPushes, it also ends the write flow, as TryCancel() does.
  /// Must not be called from a reaction other than OnDone(), which could not run meanwhile. From inside
  /// OnDone(), such as from the done callback, it returns at once.
  /// @param timeout how long to wait for OnDone()
  /// @return true once OnDone() has run, so the reactor may be destroyed; false on a timeout, when it may
  ///         not be
  bool CancelAndWait(std::chrono::milliseconds timeout) {
    // TryCancel() ends the write flow too, which releases the write-flow hold.
    TryCancel();
    return done_latch_->WaitFor(timeout);
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

 protected:
  /// This event function is called by gRPC when a write operation completes.
  /// The OnWriteDoneCallback is then called, but on the same gRPC thread.
  /// @param ok true if the write was successful
  void OnWriteDone(bool ok) override {
    // Published before write_pending_ is cleared, so a write call that sees no write pending also sees
    // the failure, and ends the write flow rather than issuing a write that cannot succeed.
    if (!ok) stream_no_more_ = true;
    if constexpr (kPushes) {
      write_pending_ = false;
      // A failed write ends the write flow, since no write can succeed anymore. Ending it from inside the
      // reaction is safe: gRPC counts this write as pending until the reaction returns.
      if (!ok) EndOpenWriteFlow();
      if (cbs_.write_done) cbs_.write_done(this, ok);
    } else {
      if (cbs_.write_done) cbs_.write_done(this, ok);
      // After write_done, so the application has seen this write complete before it is asked for the
      // next one. Issued from inside this reaction, which is what makes it safe without a hold.
      if (ok) PullNextRequest();
    }
  }

  /// This event function is called by gRPC when an explicit StartWritesDone() operation (i.e.
  /// CloseRequestStream()) completes. Not called for a close implied via StartWriteLast() (i.e.
  /// SendLastRequest()). Either way, the write side is conclusively over once this fires, and, since a
  /// client-streaming RPC has no read side, so is the stream. The write flow already ended when the
  /// close was issued.
  /// @param ok true if the close was successful
  void OnWritesDoneDone(bool /*ok*/) override { stream_no_more_ = true; }

  /// This event function is called by gRPC when the RPC is done. The OnDoneCallback
  /// callback is then called, but on the same gRPC thread. The received status
  /// is also copied into the reactor.
  /// @param status info coming from gRPC
  void OnDone(const grpc::Status& status) override {
    const auto latch = done_latch_;  // As in ActiveUnaryReactor::OnDone()
    latch->Enter();
    stream_no_more_ = true;
    if constexpr (kDelivery == Delivery::kPull) {
      response_ready_ = status.ok();
      if (cbs_.done) {
        status_ = status;  // doing deep-copy unfortunately
        cbs_.done(this, status, *response_);
      }
    } else {
      if (cbs_.done) {
        status_ = status;  // doing deep-copy unfortunately
        cbs_.done(this, status, std::move(response_));
      }
    }
    latch->Signal();
  }

  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  /// Read target. Under Delivery::kPull it holds the response until GetResponse() swaps it out; under
  /// Delivery::kPush it is moved out on OnDone.
  std::unique_ptr<ResponseT> response_{std::make_unique<ResponseT>()};

 private:
  grpc::Status status_;
  // Signalled once OnDone() has run, for CancelAndWait().
  std::shared_ptr<detail::DoneLatch> done_latch_{std::make_shared<detail::DoneLatch>()};
  ActiveWriteCallbacks<RequestT, ResponseT, kDelivery, kSubmission> cbs_;

  // Storage for the request currently being written. SendRequest() moves the caller's argument
  // here so StartWrite() has a stable pointer that survives past the caller's own statement -
  // the application gives up the request once SendRequest() accepts it. Under Submission::kReactorPulls,
  // the request next_request returned is moved here instead.
  RequestT pending_request_;

  // Delivery::kPull only: flag indicating the response is ready to be read via GetResponse()
  // Set by gRPC thread, read by application thread.
  std::atomic_bool response_ready_{false};

  // Submission::kApplicationPushes only: flag indicating a write operation is in progress.
  // gRPC requires that only one write be in flight at a time.
  // Set by the thread that claimed the write flow, cleared by gRPC thread via OnWriteDone.
  std::atomic_bool write_pending_{false};

  /// Submission::kReactorPulls: asks next_request for the next request, and writes it, or closes the request
  /// stream when there is none. Runs before the RPC starts, then only inside OnWriteDone(), so one write at
  /// a time, and never outside a reaction once the RPC runs. Pulls nothing once the RPC is cancelled: its
  /// end no longer depends on the request stream being closed.
  void PullNextRequest() requires(!kPushes) {
    if (stream_no_more_) return;
    std::optional<RequestT> next;
    if (cbs_.next_request) next = cbs_.next_request(this);
    if (next) {
      pending_request_ = std::move(*next);
      this->StartWrite(&pending_request_);
    } else {
      this->StartWritesDone();
    }
  }

  /// Claims the open write flow for one write call, so nothing can end it, and release the hold,
  /// until that call returns it.
  /// @return false when the write flow is ended, or claimed by a call on another thread
  bool ClaimWriteFlow() {
    int open = kFlowOpen;
    return write_flow_.compare_exchange_strong(open, kFlowBusy);
  }

  /// Returns the claimed write flow, open, unless a request to end it arrived meanwhile. Both sides
  /// publish before they test: the requester sets stream_no_more_ before trying to end an open flow, and
  /// this reopens the flow before reading stream_no_more_. So whichever side looks last ends it.
  void ReturnWriteFlow() {
    write_flow_ = kFlowOpen;
    if (stream_no_more_) EndOpenWriteFlow();
  }

  /// Ends the write flow its caller has claimed, and releases the write-flow hold.
  void EndWriteFlow() {
    write_flow_ = kFlowEnded;
    this->RemoveHold();
  }

  /// Ends the write flow if it is open and unclaimed, and releases the write-flow hold. A claimed flow is
  /// ended by its claimer instead, when it returns it.
  void EndOpenWriteFlow() {
    int open = kFlowOpen;
    if (write_flow_.compare_exchange_strong(open, kFlowEnded)) this->RemoveHold();
  }

  // Submission::kApplicationPushes only: the write flow, and with it the hold StartCall() took for it.
  // Open while writes may still be issued, busy while one write call holds the claim, ended once the hold
  // is released. Only the transition to ended releases the hold, so it is released exactly once. Ended
  // until StartCall() opens it, since there is no hold to release before.
  static constexpr int kFlowEnded = 0;
  static constexpr int kFlowOpen = 1;
  static constexpr int kFlowBusy = 2;
  std::atomic<int> write_flow_{kFlowEnded};

  // Once a write failed, TryCancel() was called, the close completed, or OnDone() fired, no more
  // StartWrite() must be called. A request to end the write flow: whoever sees it with the flow open,
  // or returns the flow while it is set, ends it. Under Submission::kReactorPulls, what stops the pulls.
  // Set by any thread, read by whichever thread claims the write flow, or pulls the next request.
  std::atomic_bool stream_no_more_{false};
};
/// Template callbacks for bidirectional streaming RPC client reactor. It contains all available
/// callback slots needed by specialized RPC client reactors.
/// @tparam RequestT type of protobuf message the RPC sends
/// @tparam ResponseT type of protobuf message the RPC receives
/// @tparam kDelivery how the reactor hands each message over, which sets the read_ok slot's parameter
/// @tparam kSubmission who starts the transfer of each request
/// @tparam kPacing who arms the next read, which sets the read_ok slot's return type
template <class RequestT, class ResponseT, Delivery kDelivery, Submission kSubmission,
          ReadPacing kPacing = ReadPacing::kPerMessage>
requires std::derived_from<RequestT, google::protobuf::Message> &&
         std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveBidiCallbacks {
  /// Function signature for ClientBidiReactor::OnReadDone event with positive OK flag. Its parameter
  /// follows the Delivery, and its return type the ReadPacing, exactly as in ActiveReadCallbacks: the
  /// message is lent (kPull) or given (kPush), and under ReadPacing::kPerMessage the callback returns
  /// true to hold the read side, or false to let the reactor read on at once. An unbound slot counts as
  /// declining every message.
  /// @param reactor instance pointer on which the event is received
  /// @param response the received message, lent (kPull) or owned by the callback (kPush)
  using OnReadDoneOkCallback = std::function<detail::ReadOkResult<kPacing>(
      grpc::ClientBidiReactor<RequestT, ResponseT>*, detail::DeliveredMessage<ResponseT, kDelivery>)>;
  OnReadDoneOkCallback read_ok;  ///< Slot for ClientBidiReactor::OnReadDone event with positive OK flag

  /// Function signature for ClientBidiReactor::OnReadDone event with negative OK flag.
  /// @param reactor instance pointer on which the event is received
  using OnReadDoneNOkCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>*)>;
  OnReadDoneNOkCallback read_nok;  ///< Slot for ClientBidiReactor::OnReadDone event with negative OK flag

  /// Function signature for ClientBidiReactor::OnWriteDone event.
  /// @param reactor instance pointer on which the event is received
  /// @param ok true if the write was successful, false otherwise
  using OnWriteDoneCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>*, bool ok)>;
  OnWriteDoneCallback write_done;  ///< Slot for ClientBidiReactor::OnWriteDone event

  /// Function signature for the reactor pulling the next request, under Submission::kReactorPulls only,
  /// with the same contract as ActiveWriteCallbacks::NextRequestCallback. The read side keeps running
  /// meanwhile, and the request stream closes once this has nothing more, while the server may still send.
  /// @param reactor instance pointer on which the request will be written
  /// @return the next request, given to the reactor, or std::nullopt when there is none
  using NextRequestCallback =
      std::function<std::optional<RequestT>(grpc::ClientBidiReactor<RequestT, ResponseT>*)>;
  /// Slot for the reactor pulling the next request. Under kApplicationPushes it is an empty placeholder.
  [[no_unique_address]] detail::NextRequestSlot<kSubmission, NextRequestCallback> next_request;

  /// Function signature for ClientBidiReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  using OnDoneCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>*, const grpc::Status&)>;
  OnDoneCallback done;  ///< Slot for ClientBidiReactor::OnDone event
};

/// Template class for bidirectional streaming RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides Status(), and
/// GetResponse() under Delivery::kPull)
/// @tparam RequestT type of protobuf message the RPC sends
/// @tparam ResponseT type of protobuf message the RPC receives
/// @tparam kDelivery how the reactor hands each message over, see Delivery
/// @tparam kSubmission who starts the transfer of each request, see Submission
/// @tparam kPacing who arms the read that follows a delivered message, see ReadPacing
template <class RequestT, class ResponseT, Delivery kDelivery, Submission kSubmission,
          ReadPacing kPacing = ReadPacing::kPerMessage>
requires std::derived_from<RequestT, google::protobuf::Message> &&
         std::derived_from<ResponseT, google::protobuf::Message>
class ActiveBidiReactor : public grpc::ClientBidiReactor<RequestT, ResponseT> {
  static constexpr bool kPushes = kSubmission == Submission::kApplicationPushes;

 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// The derived, specialized reactor must call StartRead() after binding this reactor
  /// to the RPC via stub.async()->Method(context, this), and before StartCall().
  /// Calling StartRead() here, before that binding exists, would segfault.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ActiveBidiReactor(std::unique_ptr<grpc::ClientContext> context,
                    ActiveBidiCallbacks<RequestT, ResponseT, kDelivery, kSubmission, kPacing>&& cbs)
      : context_(std::move(context)),
        cbs_(std::move(cbs)) {}

  /// Destructor of the reactor class. It tells the gRPC connection to close the channel.
  /// If the context/channel is already closed, there's no problem to TryCancel() it again.
  ~ActiveBidiReactor() override {
    context_->TryCancel();
  }

  /// This class cannot be copied.
  ActiveBidiReactor(const ActiveBidiReactor&) = delete;
  /// This class cannot be copied.
  ActiveBidiReactor& operator=(const ActiveBidiReactor&) = delete;
  /// This class cannot be moved.
  ActiveBidiReactor(ActiveBidiReactor&&) = delete;
  /// This class cannot be moved.
  ActiveBidiReactor& operator=(ActiveBidiReactor&&) = delete;

  /// Activates the RPC. Shadows grpc::ClientBidiReactor::StartCall(), which the specialized reactor
  /// already calls last in its constructor, so a new specialization cannot forget what this adds, which
  /// follows the Submission.
  /// Submission::kApplicationPushes: reserves the hold that covers the write operations the application
  /// issues from its own thread. gRPC accepts a hold only before StartCall() or from inside a reaction,
  /// which is why the first one is taken here and every later one is taken in OnWriteDone().
  /// The flag is published before StartCall(), so a reaction arriving immediately after it finds a
  /// claimable hold rather than one that is outstanding but still invisible.
  /// Submission::kReactorPulls: pulls the first request, as ActiveWriteReactor::StartCall() does. Every
  /// later write is issued from OnWriteDone(), so the write side takes no hold.
  void StartCall() {
    if constexpr (kPushes) {
      this->AddHold();
      write_idle_ = true;
    } else {
      PullNextRequest();
    }
    grpc::ClientBidiReactor<RequestT, ResponseT>::StartCall();
  }

  /// Sends a request message asynchronously on the bidirectional stream.
  /// The write operation completes asynchronously and OnWriteDone() will be called.
  /// gRPC requires that only one write be in flight at a time, so this method
  /// returns false if a write is already pending. Callers should wait for
  /// OnWriteDone() before calling SendRequest() again.
  /// The caller gives up the request to the reactor: it must be a temporary or explicitly
  /// std::move()'d, since the reactor takes ownership and the caller must not keep using it.
  /// Submission::kApplicationPushes only: under kReactorPulls the reactor issues every write itself.
  /// @param request message to send, moved into the reactor
  /// @return true if the write was initiated, false if rejected (stream closed, write pending,
  ///         or the RPC has already finished/is finishing)
  bool SendRequest(RequestT&& request) requires(kPushes) {
    if (writes_done_) return false;  // Stream already closed, by this same thread
    // Winning the claim is what makes the StartWrite() below safe from the application thread: the
    // idle-window hold is outstanding across it, so OnDone() cannot be tearing down the state the
    // call reaches into. A lost claim means a write is already in flight, or the write side is
    // over, which are exactly the cases that must be rejected. See "Hold requirements" in
    // reactor_client.md.
    bool idle = true;
    if (!write_idle_.compare_exchange_strong(idle, false)) return false;
    // Re-checked after winning the claim, and it is not redundant with it. The claim proves the
    // state is still alive, which is what makes the call safe; it says nothing about the stream
    // being dead. A write issued on a terminating RPC is safe but doomed: gRPC fails it, with
    // OnWriteDone(false). Releasing and refusing here reports that synchronously instead, as a false
    // return. The check can still race the stream ending, and the write it lets through then fails
    // the same way, so the RPC ends either way.
    if (stream_no_more_) {
      this->RemoveHold();
      return false;
    }
    pending_request_ = std::move(request);
    // StartWrite() before RemoveHold(), so the outstanding-callback count never reaches zero
    // between the two. Same ordering, same reason, as ResumeRead().
    this->StartWrite(&pending_request_);
    this->RemoveHold();
    return true;
  }

  /// Sends the final request message and signals the end of the client request stream in a
  /// single operation, avoiding the race between a separate SendRequest()/CloseRequestStream()
  /// pair. The caller gives up the request the same way SendRequest() requires. The server may
  /// continue sending responses. Submission::kApplicationPushes only.
  /// @param request last message to send, moved into the reactor
  /// @return true if the write was initiated, false if rejected (stream closed, write pending,
  ///         or the RPC has already finished/is finishing)
  bool SendLastRequest(RequestT&& request) requires(kPushes) {
    if (writes_done_) return false;  // Stream already closed, by this same thread
    bool idle = true;                // Same claim, same reasoning, as SendRequest()
    if (!write_idle_.compare_exchange_strong(idle, false)) return false;
    if (stream_no_more_) {           // Same post-claim re-check, same reasoning
      this->RemoveHold();
      return false;
    }
    pending_request_ = std::move(request);
    // Per gRPC's contract, calling this already forbids any further StartWrite/StartWriteLast/
    // StartWritesDone, the same as CloseRequestStream() - set writes_done_ now, synchronously.
    // It is also what stops OnWriteDone() from re-arming the hold for a write side that is over.
    writes_done_ = true;
    this->StartWriteLast(&pending_request_, grpc::WriteOptions());
    this->RemoveHold();
    return true;
  }

  /// Signals the end of the client request stream.
  /// After this call, no more SendRequest() calls are allowed.
  /// The server may continue sending responses. Submission::kApplicationPushes only: under
  /// kReactorPulls the reactor closes the stream when next_request has nothing more.
  /// @return true if the close was initiated, false if rejected (already closed, write pending,
  ///         or the RPC has already finished/is finishing). Callers should wait for OnWriteDone()
  ///         and retry.
  bool CloseRequestStream() requires(kPushes) {
    if (writes_done_) return false;  // Already closed
    // StartWritesDone() is an application-thread call like the writes, so it needs the same claim.
    bool idle = true;
    if (!write_idle_.compare_exchange_strong(idle, false)) return false;
    if (stream_no_more_) {           // Same post-claim re-check, same reasoning
      this->RemoveHold();
      return false;
    }
    writes_done_ = true;
    this->StartWritesDone();
    this->RemoveHold();
    return true;
  }

  /// Sends a best-effort out-of-band cancel to the RPC. That signal is thread-safe
  /// and can be sent anytime from any thread. The goal of that signal is to provoke
  /// the `OnDone` event from the RPC. Under Submission::kReactorPulls, it also stops the reactor from
  /// pulling any further request.
  void TryCancel() {
    context_->TryCancel();
    // Not under kApplicationPushes: a ResumeRead() or GetResponse() that then armed no read would leave
    // the idle-window hold with nothing to release it, since only OnReadDone(false) does once idle.
    if constexpr (!kPushes) stream_no_more_ = true;
  }

  /// Delivery::kPull only, under ReadPacing::kPerMessage or kTurnByTurn. Swaps the underlying data storage of the
  /// response object and resume the held RPC.
  /// The swap mechanism is important to avoid a deep-copy of the content of the
  /// response. Having the response swapped is acceptable because the bidirectional
  /// RPC overwrites the response at each received message, so the swap is a
  /// good technique to speed up the response proceeding time.
  /// RemoveHold() is always called once a hold was added (by OnReadDone()'s AddHold()), whether or
  /// not reading is restarted. gRPC's hold count is a single, per-RPC counter shared by both the
  /// read and write directions - it only gates OnDone() and does not block other reactions (e.g.
  /// OnWriteDone() on an unrelated in-flight write can still fire and set stream_no_more_ while
  /// this hold is outstanding). The hold must be released exactly once per AddHold() regardless,
  /// or the RPC stalls forever.
  /// Not offered under ReadPacing::kContinuous, where the next read overwrites the message as soon as the
  /// callback returns, so there is nothing left to pull.
  /// Like ResumeRead(), it takes whichever message is held, so it is called once per message the callback
  /// kept. It can run before the callback returns, so handing the reactor on is the callback's last use of
  /// the lent message.
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) requires(kDelivery == Delivery::kPull && kPacing != ReadPacing::kContinuous) {
    // Claimed, as in ActiveReadReactor::GetResponse(), so only one caller releases the hold.
    if (response_ready_.exchange(0) == 0) return false;
    swap(response_, response);  // Moving the read content on the user side
    if (!stream_no_more_) {
      // Restart reading
      this->StartRead(&response_);
    }
    // Resuming RPC - must run regardless of whether reading restarted, see above.
    this->RemoveHold();
    return true;
  }

  /// Delivery::kPush only, under ReadPacing::kPerMessage or kTurnByTurn. Arms the read that OnReadDone()
  /// left unarmed, and releases the hold it took. The application calls this once per held message, when
  /// it no longer needs the read side stalled. Until it does, the RPC cannot terminate: the hold suppresses
  /// OnDone(), and no outstanding read is left to report the stream ending, so an application that
  /// drops a message without resuming strands the RPC and can never legally destroy the reactor. The
  /// write side keeps running meanwhile, since a read hold stalls only the reads.
  /// Not offered under ReadPacing::kContinuous, where the reactor arms its own reads and takes no hold.
  /// Releases whichever message is held at that moment, with the same once-per-kept-message rule as
  /// ActiveReadReactor::ResumeRead().
  /// @return true when a read was armed. false when there was no hold to release (no message is
  ///         currently held), or when the RPC is already finished/finishing
  bool ResumeRead() requires(kDelivery == Delivery::kPush && kPacing != ReadPacing::kContinuous) {
    // Claiming the token is what makes a duplicate or spurious call safe: RemoveHold() below is
    // reached only by the caller that took the hold away from OnReadDone(), never twice, so the
    // gRPC outstanding-callback count cannot underflow into an early OnDone().
    if (read_held_.exchange(0) == 0) return false;
    // StartRead() before RemoveHold(), so the outstanding-callback count never reaches zero between
    // the two and lets OnDone() fire while this call is still running.
    // stream_no_more_ is genuinely reachable here, unlike on a read-only reactor: it covers both
    // directions, so a write that failed while this hold was outstanding already set it.
    const bool armed = !stream_no_more_;
    if (armed) this->StartRead(&response_);
    // Must run exactly once per AddHold(), armed or not, or the RPC stalls forever.
    this->RemoveHold();
    return armed;
  }

  /// Cancels the RPC and waits until OnDone() has run, so the reactor can be destroyed at once. For an
  /// application that must tear a reactor down before its RPC ends, such as at shutdown, once its event
  /// loop no longer runs the OnDone handler. Destroying a reactor whose OnDone() has not run is a
  /// use-after-free, since gRPC still calls it: the destructor's cancel does not wait. A message still held is
  /// released as by GetResponse() or ResumeRead(), without arming a read, and under
  /// Submission::kApplicationPushes the idle-window hold is released too, since with no read armed,
  /// OnReadDone(false) may never come to release it.
  /// Must not be called from a reaction other than OnDone(), which could not run meanwhile. From inside
  /// OnDone(), such as from the done callback, it returns at once.
  /// @param timeout how long to wait for OnDone()
  /// @return true once OnDone() has run, so the reactor may be destroyed; false on a timeout, when it may
  ///         not be
  bool CancelAndWait(std::chrono::milliseconds timeout) {
    context_->TryCancel();
    // Published before the releases below, so a release call racing this one arms no read, and an
    // OnWriteDone() re-arming the idle-window hold meanwhile releases it itself.
    stream_no_more_ = true;
    ReleaseHeldRead();
    if constexpr (kPushes) ReleaseWriteIdleHold();
    return done_latch_->WaitFor(timeout);
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

 protected:
  /// This event function is called by gRPC when the stream has a read event. The user-side
  /// callback is then called, but on the same gRPC thread.
  /// Based on the value of the `ok` flag, the OnReadDoneOkCallback or OnReadDoneNOkCallback
  /// is called. A received message is handed over by OnReadDoneOk(), according to the Delivery and the
  /// ReadPacing the reactor was instantiated with.
  /// @param ok true: a response is received. false: the read stream is closed.
  void OnReadDone(const bool ok) override {
    if (!ok) {
      // Published before the release below, so an OnWriteDone() concurrently arming the idle-window
      // hold observes the flag and releases what this reaction could not yet claim.
      stream_no_more_ = true;
      // The only trigger that frees a write side left idle. gRPC's contract makes a read failure a
      // reliable end-of-RPC signal for either direction, so this covers the passive terminations
      // (a deadline, or the server closing) that no application write action would ever report.
      // Under Submission::kReactorPulls there is no such hold to free.
      if constexpr (kPushes) ReleaseWriteIdleHold();
      if (cbs_.read_nok) cbs_.read_nok(this);
      return;
    }
    OnReadDoneOk();
  }

  /// This event function is called by gRPC when a write operation completes.
  /// The OnWriteDoneCallback is then called, but on the same gRPC thread.
  /// @param ok true if the write was successful
  void OnWriteDone(bool ok) override {
    if (!ok) stream_no_more_ = true;
    if constexpr (!kPushes) {
      if (cbs_.write_done) cbs_.write_done(this, ok);
      // As in ActiveWriteReactor::OnWriteDone(): after write_done, and inside this reaction.
      if (ok) PullNextRequest();
      return;
    }
    // Re-arms the idle-window hold for the next application-thread write, from inside this reaction
    // where taking a hold is free of the race the application thread has. Skipped once the write
    // side is conclusively over, so a closed or failed stream never suppresses OnDone().
    // The writes_done_ half of that guard is deliberate rather than load-bearing: removing it keeps
    // every test green, because OnReadDone(false) would release the surplus hold once the server
    // finishes. It stays because the write side already knows it is over here, and leaning on a
    // read-side event to undo a hold this reaction should never have taken would couple the two
    // directions for no reason.
    if (ok && !writes_done_ && !stream_no_more_) {
      this->AddHold();
      write_idle_ = true;
      // Re-checked after the flag is published, because a concurrent OnReadDone(false) can end the
      // RPC between the guard above and here, and its own release would have found nothing to
      // claim. Both sides publish before they test, so exactly one of them releases.
      if (stream_no_more_) ReleaseWriteIdleHold();
    }
    // Armed before the callback, because the callback may write inline, or hand off to a thread
    // that writes before this reaction returns.
    if (cbs_.write_done) cbs_.write_done(this, ok);
  }

  /// This event function is called by gRPC when an explicit StartWritesDone() operation (i.e.
  /// CloseRequestStream()) completes. Not called for a close implied via StartWriteLast() (i.e.
  /// SendLastRequest()). Either way, the write side is already over: under Submission::kApplicationPushes
  /// writes_done_ keeps it shut, and under kReactorPulls no pull follows a close.
  /// A successful close says nothing about the read side, which the server may keep feeding: marking
  /// the stream over here would stop a held read from ever being re-armed, and the RPC would never
  /// end. Only a failed close, like any failed operation, means neither direction can go on.
  /// @param ok true if the close was successful
  void OnWritesDoneDone(bool ok) override {
    if (!ok) stream_no_more_ = true;
  }

  /// This event function is called by gRPC when the RPC is done and no more operation is possible
  /// with that reactor instance. The OnDoneCallback is then called, but on the same gRPC thread.
  /// The received status is also copied into the reactor.
  /// @param status info coming from gRPC
  void OnDone(const grpc::Status& status) override {
    const auto latch = done_latch_;  // As in ActiveUnaryReactor::OnDone()
    latch->Enter();
    stream_no_more_ = true;
    if (cbs_.done) {
      status_ = status;  // doing deep-copy unfortunately
      cbs_.done(this, status);
    }
    latch->Signal();
  }

  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  /// Read target. Under Delivery::kPull it holds the response until GetResponse() swaps it out; under
  /// Delivery::kPush it is swapped out on each read, and never exposed.
  ResponseT response_;

 private:
  /// Hands a received message over, in two independent steps, exactly as ActiveReadReactor::OnReadDoneOk()
  /// does: the Delivery sets what the read_ok callback receives, and the ReadPacing who arms the next
  /// read. The hold it may take covers the read side only; the write side keeps its own idle-window hold.
  void OnReadDoneOk() {
    // An unbound slot has nobody to keep the message or to release a hold: read on, without a hold.
    if (!cbs_.read_ok) {
      this->StartRead(&response_);
      return;
    }
    // Step 1, Delivery. Before any hold is published, for the reason given in ActiveReadReactor.
    auto message = TakeMessage();
    // Step 2, ReadPacing.
    if constexpr (kPacing == ReadPacing::kContinuous) {
      HandOver(message);
      // Restart reading. Last action of the reaction, which is what removes the need for a hold.
      this->StartRead(&response_);
    } else {
      // Holding the read side. Taken before the callback, for the same reason as in ActiveReadReactor.
      this->AddHold();
      [[maybe_unused]] const auto held = PublishHeld();
      if constexpr (kPacing == ReadPacing::kPerMessage) {
        if (HandOver(message)) return;
        ReleaseDeclined(held);
      } else {
        HandOver(message);
      }
    }
  }

  /// Delivery step, as in ActiveReadReactor::TakeMessage().
  auto TakeMessage() {
    if constexpr (kDelivery == Delivery::kPull) {
      return &std::as_const(response_);
    } else {
      auto message = std::make_unique<ResponseT>();
      swap(*message, response_);
      return message;
    }
  }

  /// Invokes the read_ok callback with what TakeMessage() prepared.
  /// @return the callback's hold decision under ReadPacing::kPerMessage, nothing otherwise
  template <class MessageT>
  detail::ReadOkResult<kPacing> HandOver(MessageT& message) {
    if constexpr (kDelivery == Delivery::kPull) {
      return cbs_.read_ok(this, *message);
    } else {
      return cbs_.read_ok(this, std::move(message));
    }
  }

  /// Publishes the read hold OnReadDone() took, for the call that will release it to claim, under its own
  /// token, as in ActiveReadReactor::PublishHeld().
  /// @return the token, for ReleaseDeclined()
  std::uint64_t PublishHeld() {
    const auto held = ++held_count_;
    if constexpr (kDelivery == Delivery::kPull) {
      response_ready_ = held;
    } else {
      read_held_ = held;
    }
    return held;
  }

  /// ReadPacing::kPerMessage, after the callback declined the message, as in
  /// ActiveReadReactor::ReleaseDeclined(): claims the read hold back with the message's own token,
  /// restarts reading, and releases it.
  /// @param held the token PublishHeld() returned for this message
  void ReleaseDeclined(std::uint64_t held) {
    auto& flag = (kDelivery == Delivery::kPull) ? response_ready_ : read_held_;
    if (!flag.compare_exchange_strong(held, 0)) return;
    this->StartRead(&response_);
    this->RemoveHold();
  }

  /// For CancelAndWait(): releases the hold of a message still held, without arming a read, as
  /// ActiveReadReactor::ReleaseHeldRead() does.
  void ReleaseHeldRead() {
    auto& flag = (kDelivery == Delivery::kPull) ? response_ready_ : read_held_;
    if (flag.exchange(0) != 0) this->RemoveHold();
  }

  /// Submission::kReactorPulls: asks next_request for the next request, and writes it, or closes the request
  /// stream when there is none, as ActiveWriteReactor::PullNextRequest() does. Closing the request stream
  /// leaves the read side running, so the RPC still ends when the server finishes.
  void PullNextRequest() requires(!kPushes) {
    if (stream_no_more_) return;
    std::optional<RequestT> next;
    if (cbs_.next_request) next = cbs_.next_request(this);
    if (next) {
      pending_request_ = std::move(*next);
      this->StartWrite(&pending_request_);
    } else {
      this->StartWritesDone();
    }
  }

  /// Releases the idle-window hold, but only for the caller that claims it, so the count cannot
  /// underflow into an early OnDone() when both reaction sides try at once.
  void ReleaseWriteIdleHold() {
    bool idle = true;
    if (write_idle_.compare_exchange_strong(idle, false)) this->RemoveHold();
  }

  grpc::Status status_;
  // Signalled once OnDone() has run, for CancelAndWait().
  std::shared_ptr<detail::DoneLatch> done_latch_{std::make_shared<detail::DoneLatch>()};
  ActiveBidiCallbacks<RequestT, ResponseT, kDelivery, kSubmission, kPacing> cbs_;

  // Under ReadPacing::kPerMessage or kTurnByTurn: the number of messages held so far, as in
  // ActiveReadReactor. Only touched by OnReadDone().
  std::uint64_t held_count_{0};

  // Delivery::kPush, under ReadPacing::kPerMessage or kTurnByTurn: the token of the message whose read
  // hold is outstanding and still unclaimed, waiting for the ResumeRead() that releases it, or 0.
  // Set by gRPC thread, cleared by whichever thread claims it.
  std::atomic<std::uint64_t> read_held_{0};

  // Delivery::kPull, under ReadPacing::kPerMessage or kTurnByTurn: the token of the held message waiting
  // in response_ for GetResponse(), and with it an unclaimed read hold, or 0. Claimed the same way as
  // read_held_.
  // Set by gRPC thread, cleared by whichever thread claims it.
  std::atomic<std::uint64_t> response_ready_{0};

  // Storage for the request currently being written. SendRequest() moves the caller's argument
  // here so StartWrite() has a stable pointer that survives past the caller's own statement -
  // the application gives up the request once SendRequest() accepts it.
  RequestT pending_request_;

  // Whether an unclaimed idle-window hold is outstanding, which is also the permission to issue a
  // write from the application thread. True means the write stream is idle and one operation may be
  // started; false means a write is already in flight, or the write side is over. Claiming it with
  // a compare-exchange is what closes the race a plain flag test could only narrow, and gRPC's
  // one-write-in-flight rule is enforced by the same claim rather than by a separate flag.
  // Armed before StartCall() and again in OnWriteDone(); claimed by whichever thread writes,
  // closes the stream, or reports the read stream ending.
  std::atomic_bool write_idle_{false};

  // Submission::kApplicationPushes only: flag indicating CloseRequestStream() or SendLastRequest() has
  // been called. After this, no more SendRequest() calls are allowed.
  // Set by the application thread, read by it and by the gRPC thread running OnWriteDone().
  std::atomic_bool writes_done_{false};

  // Once we got OnReadDone(false), OnWriteDone(false), OnWritesDoneDone(false), or OnDone(), no more
  // StartRead() or StartWrite() must be called. Per gRPC's own contract (grpcpp/support/client_callback.h), a
  // failure on either direction means neither will succeed anymore, so one flag covers both.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool stream_no_more_{false};
};
}  // namespace RpcReactor::Client
