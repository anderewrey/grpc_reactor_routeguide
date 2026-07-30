///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024 anderewrey
///

#pragma once

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>

#include <atomic>
#include <functional>
#include <memory>
#include <utility>  // swap

/************************
 * gRPC Reactor: Following code belongs to the API implementation
 * It is generic as much as possible
 *
 * Active Object Pattern: Method Request & Future components
 * These generic reactor classes encapsulate RPC state (Method Request) and provide
 * asynchronous result access (Future). See reactor_client.md for detailed documentation.
 ************************/
namespace RpcReactor::Client {

/// Template callbacks for unary RPC client reactor. It contains all available callbacks slots
/// needed by specialized RPC client reactors.
/// @tparam ResponseT type of protobuf message the RPC handles
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveUnaryCallbacks {
  /// Function signature for ClientUnaryReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// The response ownership transfers to the callback, and carries the obligations documented in
  /// reactor_client.md, under "Message ownership".
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  /// @param response the received message, never null: a failed RPC yields an empty one, so the status is what
  ///        says whether the content is meaningful. Stays in the reactor if this slot is unbound
  using OnDoneCallback =
      std::function<void(grpc::ClientUnaryReactor*, const grpc::Status&, std::unique_ptr<ResponseT>)>;
  OnDoneCallback done;  ///< Slot for ClientUnaryReactor::OnDone event
};

/// template class for unary RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides Status())
/// @tparam ResponseT type of protobuf message the RPC handles
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
class ActiveUnaryReactor : public grpc::ClientUnaryReactor {
 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ActiveUnaryReactor(std::unique_ptr<grpc::ClientContext> context, ActiveUnaryCallbacks<ResponseT>&& cbs)
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
  void TryCancel() const {
    // (Point 2.2, 2.3) RPC termination behest
    context_->TryCancel();
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
    // (Point 3.1, 3.2, 3.3) RPC termination
    if (cbs_.done) {
      status_ = status;  // doing deep-copy unfortunately
      // (Point 3.4) TriggerEvent: OnDone, handing the response ownership over
      cbs_.done(this, status, std::move(response_));
    }
  }

 protected:
  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  std::unique_ptr<ResponseT> response_{std::make_unique<ResponseT>()};  ///< read target, moved out on OnDone

 private:
  grpc::Status status_;
  ActiveUnaryCallbacks<ResponseT> cbs_;
};

/// Selects when a reading reactor arms the read that follows a delivered message. Both modes hand
/// the message to the read_ok slot the same way, by moving an owned std::unique_ptr out, so the
/// callback signature and the application wiring are identical under either one. Only the re-arm
/// point differs, and with it the flow control. See "Read pacing modes" in reactor_client.md.
enum class ReadPacing {
  /// The reactor arms the next read itself, as the last statement of the reaction. The stream never
  /// waits for the application, which costs an unbounded queue between them.
  kContinuous,
  /// The reactor holds the RPC and arms nothing. The application calls ResumeRead() when it is done
  /// with the message, which bounds the stream to one message in flight and stalls it meanwhile.
  kTurnByTurn,
};

/// Template callbacks for stream-reader RPC client reactor. It contains all available callbacks slots
/// needed by specialized RPC client reactors.
/// @tparam ResponseT type of protobuf message the RPC handles
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveReadCallbacks {
  /// Function signature for ClientReadReactor::OnReadDone event with positive OK flag.
  /// The reactor swaps the message out before this call, so the callback owns the message and the
  /// reactor never refers to it again. Handing that ownership on carries obligations documented in
  /// reactor_client.md, under "Message ownership". Under ReadPacing::kContinuous the reactor arms the
  /// next read as soon as this returns; under kTurnByTurn it arms none until ResumeRead() is called.
  /// @param reactor instance pointer on which the event is received
  /// @param response the received message, owned by the callback. Dropped if this slot is unbound
  using OnReadDoneOkCallback =
      std::function<void(grpc::ClientReadReactor<ResponseT>*, std::unique_ptr<ResponseT>)>;
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
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides Status()).
/// @tparam ResponseT type of protobuf message the RPC handles
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
class ActiveReadReactor : public grpc::ClientReadReactor<ResponseT> {
 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  /// @param pacing when the reactor arms the read following a delivered message. Fixed for the
  ///        whole RPC, which keeps it out of the race between the application and OnReadDone()
  ActiveReadReactor(std::unique_ptr<grpc::ClientContext> context,
                    ActiveReadCallbacks<ResponseT>&& cbs,
                    const ReadPacing pacing = ReadPacing::kContinuous)
      : context_(std::move(context)),
        cbs_(std::move(cbs)),
        pacing_(pacing) {}

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
  void TryCancel() const {
    // (Point 3.2, 3.3) RPC termination behest
    context_->TryCancel();
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

  /// Arms the read that OnReadDone() left unarmed under ReadPacing::kTurnByTurn, and releases the hold
  /// it took. The application calls this once per delivered message, when it no longer needs the
  /// stream stalled. Until it does, the RPC cannot terminate: the hold suppresses OnDone(), and no
  /// outstanding read is left to report the stream ending, so an application that drops a message
  /// without resuming strands the RPC and can never legally destroy the reactor.
  /// Meaningless under ReadPacing::kContinuous, where the reactor arms its own reads and takes no hold.
  /// @return true when a read was armed. false when there was no hold to release (push mode, or no
  ///         message is currently outstanding), or when the stream is already over
  bool ResumeRead() {
    // Claiming the flag is what makes a duplicate or spurious call safe: RemoveHold() below is
    // reached only by the caller that took the hold away from OnReadDone(), never twice, so the
    // gRPC outstanding-callback count cannot underflow into an early OnDone().
    bool held = true;
    if (!read_held_.compare_exchange_strong(held, false)) return false;
    // StartRead() before RemoveHold(), so the outstanding-callback count never reaches zero between
    // the two and lets OnDone() fire while this call is still running.
    const bool armed = !stream_no_more_;
    // (Point 2.18) Restart reading
    if (armed) this->StartRead(&response_);
    // (Point 2.19) Resuming RPC. Must run exactly once per AddHold(), armed or not, or the RPC
    // stalls forever.
    this->RemoveHold();
    return armed;
  }

 protected:
  /// This event function is called by gRPC when the stream has an event. The user-side
  /// callback is then called, but on the same gRPC thread.
  /// This OnReadDone is also the relevant event acknowledging the response reception.
  /// Based on the value of the `ok` flag, the OnReadDoneOkCallback or OnReadDoneNOkCallback
  /// is called.
  /// @param ok true: a response is received. false: the stream reader is closed
  ///           (but not the RPC itself).
  void OnReadDone(const bool ok) override {
    // (Point 2.3, 2.10, 4.2) Event received from stream
    if (!ok) {
      stream_no_more_ = true;
      // (Point 4.3) OnReadDone: False
      if (cbs_.read_nok) cbs_.read_nok(this);
      return;
    }
    // (Point 2.3, 2.10) OnReadDone: true
    if (cbs_.read_ok) {
      // (Point 2.4, 2.11) extracts response, by pointer swap rather than deep-copy
      auto message = std::make_unique<ResponseT>();
      swap(*message, response_);
      if (pacing_ == ReadPacing::kTurnByTurn) {
        // (Point 2.12) Holding the RPC. Taken before the callback, because the callback may hand
        // the message to a thread that calls ResumeRead() before this reaction returns, or resume
        // inline itself. Ordered before read_held_ so no ResumeRead() can observe the flag and
        // release a hold that does not exist yet.
        this->AddHold();
        read_held_ = true;
      }
      // (Point 2.5, 2.13) TriggerEvent: OnReadDoneOk
      cbs_.read_ok(this, std::move(message));
      // (Point 2.17) The application owns the re-arm from here, through ResumeRead().
      if (pacing_ == ReadPacing::kTurnByTurn) return;
    }
    // (Point 2.6) Restart reading. Last action of the reaction, which is what removes the need for
    // a hold under ReadPacing::kContinuous. See "Read pacing modes" in reactor_client.md.
    // Deliberately outside the read_ok guard: an unbound slot has nobody to call ResumeRead(), so
    // both modes must fall through here and drain the stream rather than strand the RPC.
    this->StartRead(&response_);
  }
  /// This event function is called by gRPC when the RPC is done and no more operation is possible with that reactor
  /// instance. The OnDoneCallback is then called, but on the same gRPC thread. The received status
  /// is also copied into the reactor.
  /// @param status info coming from gRPC
  void OnDone(const grpc::Status& status) override {
    // (Point 4.4, 4.5) RPC termination
    stream_no_more_ = true;
    if (cbs_.done) {
      status_ = status;  // doing deep-copy unfortunately
      // (Point 4.6) OnDone
      cbs_.done(this, status);
    }
  }

 protected:
  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  ResponseT response_;  ///< internal read target, swapped out on each read, never exposed

 private:
  grpc::Status status_;
  ActiveReadCallbacks<ResponseT> cbs_;

  // When the reactor arms the read following a delivered message. Set once at construction and
  // never written again, so OnReadDone() and the application read it without synchronizing.
  const ReadPacing pacing_;

  // Whether a ReadPacing::kTurnByTurn hold is outstanding and still owned by OnReadDone(), waiting for
  // the ResumeRead() that releases it. Always false under ReadPacing::kContinuous, which is what makes
  // a ResumeRead() call in that mode a no-op rather than an underflow.
  // Set by gRPC thread, cleared by whichever thread calls ResumeRead().
  std::atomic_bool read_held_{false};

  // Once we got OnReadDone(false) or OnDone(), no more StartRead() must be called. Only read by
  // ResumeRead(), since every other StartRead() is issued from inside a reaction.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool stream_no_more_{false};
};

/// Template callbacks for stream-writer RPC client reactor. It contains all available callbacks slots
/// needed by specialized RPC client reactors.
/// @tparam RequestT type of protobuf message the RPC sends
/// @tparam ResponseT type of protobuf message the RPC receives as final response
template <class RequestT, class ResponseT>
requires std::derived_from<RequestT, google::protobuf::Message> &&
         std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveWriteCallbacks {
  /// Function signature for ClientWriteReactor::OnWriteDone event
  /// @param reactor instance pointer on which the event is received
  /// @param ok true if the write was successful, false otherwise
  using OnWriteDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>*, bool ok)>;
  OnWriteDoneCallback write_done;  ///< Slot for ClientWriteReactor::OnWriteDone event

  /// Function signature for ClientWriteReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// The response ownership transfers to the callback, and carries the obligations documented in
  /// reactor_client.md, under "Message ownership".
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  /// @param response the received message, never null: a failed RPC yields an empty one, so the status is what
  ///        says whether the content is meaningful. Stays in the reactor if this slot is unbound
  using OnDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>*,
                                            const grpc::Status&,
                                            std::unique_ptr<ResponseT>)>;
  OnDoneCallback done;  ///< Slot for ClientWriteReactor::OnDone event
};

/// Template class for stream-writer RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides Status())
/// @tparam RequestT type of protobuf message the RPC sends
/// @tparam ResponseT type of protobuf message the RPC receives as final response
template <class RequestT, class ResponseT>
requires std::derived_from<RequestT, google::protobuf::Message> &&
         std::derived_from<ResponseT, google::protobuf::Message>
class ActiveWriteReactor : public grpc::ClientWriteReactor<RequestT> {
 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ActiveWriteReactor(std::unique_ptr<grpc::ClientContext> context,
                     ActiveWriteCallbacks<RequestT, ResponseT>&& cbs)
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

  /// Sends a request message asynchronously on the client stream.
  /// The write operation completes asynchronously and OnWriteDone() will be called.
  /// gRPC requires that only one write be in flight at a time, so this method
  /// returns false if a write is already pending. Callers should wait for
  /// OnWriteDone() before calling SendRequest() again.
  /// The caller gives up the request to the reactor: it must be a temporary or explicitly
  /// std::move()'d, since the reactor takes ownership and the caller must not keep using it.
  /// @param request message to send, moved into the reactor
  /// @return true if the write was initiated, false if rejected (stream closed, write pending,
  ///         or the RPC has already finished/is finishing)
  bool SendRequest(RequestT&& request) {
    // The stream_no_more_ check narrows a race it cannot close: a gRPC thread can set the flag
    // right after this check passes. Which reactions set the flag is listed at its declaration.
    // ActiveBidiReactor closes the same window by claiming a hold that covers only the idle write
    // stream, but that design needs OnReadDone(false) to release it, and this reactor has no read
    // stream to supply one. See "Why the write side of ActiveWriteReactor has no hold" in
    // docs/architecture.md.
    if (stream_no_more_) return false;  // RPC already finished (or finishing)
    if (writes_done_) return false;  // Stream already closed
    if (write_pending_) return false;  // Write already in progress
    pending_request_ = std::move(request);
    write_pending_ = true;
    this->StartWrite(&pending_request_);
    return true;
  }

  /// Sends the final request message and signals the end of the client request stream in a
  /// single operation, avoiding the race between a separate SendRequest()/CloseRequestStream()
  /// pair. The caller gives up the request the same way SendRequest() requires.
  /// @param request last message to send, moved into the reactor
  /// @return true if the write was initiated, false if rejected (stream closed, write pending,
  ///         or the RPC has already finished/is finishing)
  bool SendLastRequest(RequestT&& request) {
    if (stream_no_more_) return false;  // RPC already finished (or finishing)
    if (writes_done_) return false;     // Stream already closed
    if (write_pending_) return false;   // Write already in progress
    pending_request_ = std::move(request);
    write_pending_ = true;
    // Per gRPC's contract, calling this already forbids any further StartWrite/StartWriteLast/
    // StartWritesDone, the same as CloseRequestStream() - set writes_done_ now, synchronously.
    writes_done_ = true;
    this->StartWriteLast(&pending_request_, grpc::WriteOptions());
    return true;
  }

  /// Signals the end of the client request stream.
  /// After this call, no more SendRequest() calls are allowed.
  /// @return true if the close was initiated, false if rejected (already closed, write pending,
  ///         or the RPC has already finished/is finishing). Callers should wait for OnWriteDone()
  ///         and retry.
  bool CloseRequestStream() {
    if (stream_no_more_) return false;  // Same race-narrowing guard as SendRequest(), see above
    if (writes_done_) return false;     // Already closed
    if (write_pending_) return false;   // Wait for the in-flight write to complete first
    writes_done_ = true;
    this->StartWritesDone();
    return true;
  }

  /// Sends a best-effort out-of-band cancel to the RPC. That signal is thread-safe
  /// and can be sent anytime from any thread. The goal of that signal is to provoke
  /// the `OnDone` event from the RPC.
  void TryCancel() const {
    context_->TryCancel();
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

  // REJECTED: gRPC's AddMultipleHolds(int holds) reserves a fixed hold budget upfront (before
  // StartCall()) for an operation-flow issued outside the reactions, which is what SendRequest()
  // does from the application thread. Reserving one hold for the whole write flow was prototyped on
  // 2026-07-26 and rejected: it suppresses OnDone() for the life of the RPC, and a passive
  // termination gives the application no event to release on, so the RPC never completes. Kept as
  // the concrete shape both docs refer to, not as a plan. The reasoning, and what remains open for
  // this reactor, is in docs/architecture.md.
  // void UseMultipleHolds() {
  //     this->AddMultipleHolds(/* holds */ 1);
  // }

 protected:
  /// This event function is called by gRPC when a write operation completes.
  /// The OnWriteDoneCallback is then called, but on the same gRPC thread.
  /// @param ok true if the write was successful
  void OnWriteDone(bool ok) override {
    write_pending_ = false;
    if (!ok) stream_no_more_ = true;
    if (cbs_.write_done) cbs_.write_done(this, ok);
  }

  /// This event function is called by gRPC when an explicit StartWritesDone() operation (i.e.
  /// CloseRequestStream()) completes. Not called for a close implied via StartWriteLast() (i.e.
  /// SendLastRequest()). Either way, the write side is conclusively over once this fires, so it
  /// is tracked the same as OnDone() for that purpose.
  /// @param ok true if the close was successful
  void OnWritesDoneDone(bool ok) override { stream_no_more_ = true; }

  /// This event function is called by gRPC when the RPC is done. The OnDoneCallback
  /// callback is then called, but on the same gRPC thread. The received status
  /// is also copied into the reactor.
  /// @param status info coming from gRPC
  void OnDone(const grpc::Status& status) override {
    stream_no_more_ = true;
    if (cbs_.done) {
      status_ = status;  // doing deep-copy unfortunately
      cbs_.done(this, status, std::move(response_));
    }
  }

 protected:
  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  std::unique_ptr<ResponseT> response_{std::make_unique<ResponseT>()};  ///< read target, moved out on OnDone

 private:
  grpc::Status status_;
  ActiveWriteCallbacks<RequestT, ResponseT> cbs_;

  // Storage for the request currently being written. SendRequest() moves the caller's argument
  // here so StartWrite() has a stable pointer that survives past the caller's own statement -
  // the application gives up the request once SendRequest() accepts it.
  RequestT pending_request_;

  // Flag indicating a write operation is in progress.
  // gRPC requires that only one write be in flight at a time.
  // Set by application thread via StartWrite, cleared by gRPC thread via OnWriteDone.
  std::atomic_bool write_pending_{false};

  // Flag indicating CloseRequestStream() has been called.
  // After this, no more SendRequest() calls are allowed.
  // Set by application thread, read by application thread.
  std::atomic_bool writes_done_{false};

  // Once we got OnWriteDone(false) or OnDone(), no more StartWrite() must be called.
  // Mirrors stream_no_more_ in ActiveBidiReactor, which covers both directions with one flag.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool stream_no_more_{false};
};
/// Template callbacks for bidirectional streaming RPC client reactor. It contains all available
/// callback slots needed by specialized RPC client reactors.
/// @tparam RequestT type of protobuf message the RPC sends
/// @tparam ResponseT type of protobuf message the RPC receives
template <class RequestT, class ResponseT>
requires std::derived_from<RequestT, google::protobuf::Message> &&
         std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveBidiCallbacks {
  /// Function signature for ClientBidiReactor::OnReadDone event with positive OK flag.
  /// The reactor swaps the message out before this call, so the callback owns the message and the
  /// reactor never refers to it again. Handing that ownership on carries obligations documented in
  /// reactor_client.md, under "Message ownership". Under ReadPacing::kContinuous the reactor arms the
  /// next read as soon as this returns; under kTurnByTurn it arms none until ResumeRead() is called.
  /// @param reactor instance pointer on which the event is received
  /// @param response the received message, owned by the callback. Dropped if this slot is unbound
  using OnReadDoneOkCallback =
      std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>*, std::unique_ptr<ResponseT>)>;
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

  /// Function signature for ClientBidiReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  using OnDoneCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>*, const grpc::Status&)>;
  OnDoneCallback done;  ///< Slot for ClientBidiReactor::OnDone event
};

/// Template class for bidirectional streaming RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides Status())
/// @tparam RequestT type of protobuf message the RPC sends
/// @tparam ResponseT type of protobuf message the RPC receives
template <class RequestT, class ResponseT>
requires std::derived_from<RequestT, google::protobuf::Message> &&
         std::derived_from<ResponseT, google::protobuf::Message>
class ActiveBidiReactor : public grpc::ClientBidiReactor<RequestT, ResponseT> {
 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// The derived, specialized reactor must call StartRead() after binding this reactor
  /// to the RPC via stub.async()->Method(context, this), and before StartCall().
  /// Calling StartRead() here, before that binding exists, would segfault.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  /// @param pacing when the reactor arms the read following a delivered message. Fixed for the
  ///        whole RPC, which keeps it out of the race between the application and OnReadDone()
  ActiveBidiReactor(std::unique_ptr<grpc::ClientContext> context,
                    ActiveBidiCallbacks<RequestT, ResponseT>&& cbs,
                    const ReadPacing pacing = ReadPacing::kContinuous)
      : context_(std::move(context)),
        cbs_(std::move(cbs)),
        pacing_(pacing) {}

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

  /// Activates the RPC, and reserves the hold that covers the write operations the application
  /// issues from its own thread. Shadows grpc::ClientBidiReactor::StartCall(), which the
  /// specialized reactor already calls last in its constructor, so a new specialization cannot
  /// forget to arm the hold the way it could forget a second required call.
  /// gRPC accepts a hold only before StartCall() or from inside a reaction, which is why the first
  /// one is taken here and every later one is taken in OnWriteDone().
  /// The flag is published before StartCall(), so a reaction arriving immediately after it finds a
  /// claimable hold rather than one that is outstanding but still invisible.
  void StartCall() {
    this->AddHold();
    write_idle_ = true;
    grpc::ClientBidiReactor<RequestT, ResponseT>::StartCall();
  }

  /// Sends a request message asynchronously on the bidirectional stream.
  /// The write operation completes asynchronously and OnWriteDone() will be called.
  /// gRPC requires that only one write be in flight at a time, so this method
  /// returns false if a write is already pending. Callers should wait for
  /// OnWriteDone() before calling SendRequest() again.
  /// The caller gives up the request to the reactor: it must be a temporary or explicitly
  /// std::move()'d, since the reactor takes ownership and the caller must not keep using it.
  /// @param request message to send, moved into the reactor
  /// @return true if the write was initiated, false if rejected (stream closed, write pending,
  ///         or the RPC has already finished/is finishing)
  bool SendRequest(RequestT&& request) {
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
    // being dead. Issuing a write on a terminating RPC is safe but doomed, and gRPC may never
    // complete it, which would leave the outstanding-callback count above zero and stall OnDone()
    // forever. Releasing and refusing is what keeps the RPC concludable.
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
  /// continue sending responses.
  /// @param request last message to send, moved into the reactor
  /// @return true if the write was initiated, false if rejected (stream closed, write pending,
  ///         or the RPC has already finished/is finishing)
  bool SendLastRequest(RequestT&& request) {
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
  /// The server may continue sending responses.
  /// @return true if the close was initiated, false if rejected (already closed, write pending,
  ///         or the RPC has already finished/is finishing). Callers should wait for OnWriteDone()
  ///         and retry.
  bool CloseRequestStream() {
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
  /// the `OnDone` event from the RPC.
  void TryCancel() const {
    context_->TryCancel();
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

  /// Arms the read that OnReadDone() left unarmed under ReadPacing::kTurnByTurn, and releases the hold
  /// it took. The application calls this once per delivered message, when it no longer needs the
  /// read side stalled. Until it does, the RPC cannot terminate: the hold suppresses OnDone(), and
  /// no outstanding read is left to report the stream ending, so an application that drops a
  /// message without resuming strands the RPC and can never legally destroy the reactor. The write
  /// side keeps running meanwhile, since a read hold stalls only the reads.
  /// Meaningless under ReadPacing::kContinuous, where the reactor arms its own reads and takes no hold.
  /// @return true when a read was armed. false when there was no hold to release (push mode, or no
  ///         message is currently outstanding), or when the RPC is already finished/finishing
  bool ResumeRead() {
    // Claiming the flag is what makes a duplicate or spurious call safe: RemoveHold() below is
    // reached only by the caller that took the hold away from OnReadDone(), never twice, so the
    // gRPC outstanding-callback count cannot underflow into an early OnDone().
    bool held = true;
    if (!read_held_.compare_exchange_strong(held, false)) return false;
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

 protected:
  /// This event function is called by gRPC when the stream has a read event. The user-side
  /// callback is then called, but on the same gRPC thread.
  /// Based on the value of the `ok` flag, the OnReadDoneOkCallback or OnReadDoneNOkCallback
  /// is called.
  /// @param ok true: a response is received. false: the read stream is closed.
  void OnReadDone(const bool ok) override {
    if (!ok) {
      // Published before the release below, so an OnWriteDone() concurrently arming the idle-window
      // hold observes the flag and releases what this reaction could not yet claim.
      stream_no_more_ = true;
      // The only trigger that frees a write side left idle. gRPC's contract makes a read failure a
      // reliable end-of-RPC signal for either direction, so this covers the passive terminations
      // (a deadline, or the server closing) that no application write action would ever report.
      ReleaseWriteIdleHold();
      if (cbs_.read_nok) cbs_.read_nok(this);
      return;
    }
    if (cbs_.read_ok) {
      // extracts response, by pointer swap rather than deep-copy
      auto message = std::make_unique<ResponseT>();
      swap(*message, response_);
      if (pacing_ == ReadPacing::kTurnByTurn) {
        // Holding the RPC. Taken before the callback, because the callback may hand the message to
        // a thread that calls ResumeRead() before this reaction returns, or resume inline itself.
        // Ordered before read_held_ so no ResumeRead() can observe the flag and release a hold that
        // does not exist yet.
        this->AddHold();
        read_held_ = true;
      }
      cbs_.read_ok(this, std::move(message));
      // The application owns the re-arm from here, through ResumeRead().
      if (pacing_ == ReadPacing::kTurnByTurn) return;
    }
    // Restart reading. Last action of the reaction, which is what removes the need for a hold under
    // ReadPacing::kContinuous. See "Read pacing modes" in reactor_client.md.
    // Deliberately outside the read_ok guard: an unbound slot has nobody to call ResumeRead(), so
    // both modes must fall through here and drain the stream rather than strand the RPC.
    this->StartRead(&response_);
  }

  /// This event function is called by gRPC when a write operation completes.
  /// The OnWriteDoneCallback is then called, but on the same gRPC thread.
  /// @param ok true if the write was successful
  void OnWriteDone(bool ok) override {
    if (!ok) stream_no_more_ = true;
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
  /// SendLastRequest()). Either way, the write side is conclusively over once this fires, so it
  /// is tracked the same as OnDone() for that purpose.
  /// @param ok true if the close was successful
  void OnWritesDoneDone(bool ok) override { stream_no_more_ = true; }

  /// This event function is called by gRPC when the RPC is done and no more operation is possible
  /// with that reactor instance. The OnDoneCallback is then called, but on the same gRPC thread.
  /// The received status is also copied into the reactor.
  /// @param status info coming from gRPC
  void OnDone(const grpc::Status& status) override {
    stream_no_more_ = true;
    if (cbs_.done) {
      status_ = status;  // doing deep-copy unfortunately
      cbs_.done(this, status);
    }
  }

 protected:
  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  ResponseT response_;  ///< internal read target, swapped out on each read, never exposed

 private:
  /// Releases the idle-window hold, but only for the caller that claims it, so the count cannot
  /// underflow into an early OnDone() when both reaction sides try at once.
  void ReleaseWriteIdleHold() {
    bool idle = true;
    if (write_idle_.compare_exchange_strong(idle, false)) this->RemoveHold();
  }

  grpc::Status status_;
  ActiveBidiCallbacks<RequestT, ResponseT> cbs_;

  // When the reactor arms the read following a delivered message. Set once at construction and
  // never written again, so OnReadDone() and the application read it without synchronizing.
  const ReadPacing pacing_;

  // Whether a ReadPacing::kTurnByTurn hold is outstanding and still owned by OnReadDone(), waiting for
  // the ResumeRead() that releases it. Always false under ReadPacing::kContinuous, which is what makes
  // a ResumeRead() call in that mode a no-op rather than an underflow.
  // Set by gRPC thread, cleared by whichever thread calls ResumeRead().
  std::atomic_bool read_held_{false};

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

  // Flag indicating CloseRequestStream() has been called.
  // After this, no more SendRequest() calls are allowed.
  // Set by application thread, read by application thread.
  std::atomic_bool writes_done_{false};

  // Once we got OnReadDone(false), OnWriteDone(false), or OnDone(), no more StartRead() or
  // StartWrite() must be called. Per gRPC's own contract (grpcpp/support/client_callback.h), a
  // failure on either direction means neither will succeed anymore, so one flag covers both.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool stream_no_more_{false};
};
}  // namespace RpcReactor::Client
