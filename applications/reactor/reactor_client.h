///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2026 anderewrey
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
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  /// @param response reference to the response message the reactor received
  using OnDoneCallback = std::function<void(grpc::ClientUnaryReactor*, const grpc::Status&, const ResponseT&)>;
  OnDoneCallback done;  ///< Slot for ClientUnaryReactor::OnDone event
};

/// template class for unary RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides GetResponse(), Status())
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

  /// Swaps the underlying data storage of the response object.
  /// The swap mechanism is important to avoid a deep-copy of the content of the
  /// response. Having the response swapped is acceptable because the unary RPC
  /// is meant to one response only, so the swap is a good technique to speed up
  /// the response proceeding time.
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) {
    if (!response_ready_) return false;
    // (Point 3.6) extracts response
    swap(response_, response);  // Moving the read content on the user side
    response_ready_ = false;
    return true;
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
    response_ready_ = status.ok();
    if (cbs_.done) {
      status_ = status;  // doing deep-copy unfortunately
      // (Point 3.4) TriggerEvent: OnDone
      cbs_.done(this, status, response_);
    }
  }

 protected:
  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  ResponseT response_;  ///< response holder

 private:
  grpc::Status status_;
  ActiveUnaryCallbacks<ResponseT> cbs_;

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
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveReadCallbacks {
  /// Function signature for ClientReadReactor::OnReadDone event with positive OK flag
  /// @param reactor instance pointer on which the event is received
  /// @param response reference to the response message the reactor received
  /// @retval true the response must be kept intact by the reactor until read later through GetResponse() function.
  ///         The reactor is put on hold in the meantime.
  /// @retval false the response is unwanted and the reactor can immediately execute a new read operation to obtain
  ///         a new response.
  using OnReadDoneOkCallback = std::function<bool(grpc::ClientReadReactor<ResponseT>*, const ResponseT&)>;
  OnReadDoneOkCallback ok;    ///< Slot for ClientReadReactor::OnReadDone event with positive OK flag

  /// Function signature for ClientReadReactor::OnReadDone event with negative OK flag
  /// @param reactor instance pointer on which the event is received
  using OnReadDoneNOkCallback = std::function<void(grpc::ClientReadReactor<ResponseT>*)>;
  OnReadDoneNOkCallback nok;  ///< Slot for ClientReadReactor::OnReadDone event with negative OK flag

  /// Function signature for ClientReadReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  using OnDoneCallback = std::function<void(grpc::ClientReadReactor<ResponseT>*, const grpc::Status&)>;
  OnDoneCallback done;        ///< Slot for ClientReadReactor::OnDone event
};

/// Template class for stream-reader RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides GetResponse(), Status())
/// @tparam ResponseT type of protobuf message the RPC handles
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
class ActiveReadReactor : public grpc::ClientReadReactor<ResponseT> {
 public:
  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ActiveReadReactor(std::unique_ptr<grpc::ClientContext> context,
                    ActiveReadCallbacks<ResponseT>&& cbs)
      : context_(std::move(context)),
        cbs_(std::move(cbs)) { }

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

  /// Swaps the underlying data storage of the response object and resume the held RPC.
  /// The swap mechanism is important to avoid a deep-copy of the content of the
  /// response. Having the response swapped is acceptable because the stream-reader
  /// RPC is meant to be overwritten at each received response, so the swap is a
  /// good technique to speed up the response proceeding time.
  /// RemoveHold() is always called once a hold was added (by OnReadDone()'s AddHold()), whether or
  /// not reading is restarted. gRPC's hold count is a single, per-RPC counter (not one per read/
  /// write direction) that only gates OnDone() - it must be released exactly once per AddHold(),
  /// or the RPC stalls forever, even if stream_no_more_ became true while the hold was held.
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) {
    if (!response_ready_) return false;
    // (Point 2.8, 2.14) extracts response
    swap(response_, response);  // Moving the read content on the user side
    response_ready_ = false;
    if (!stream_no_more_) {
      // (Point 2.9, 2.10) Restart reading
      this->StartRead(&response_);
    }
    // (Point 2.11) Resuming RPC - must run regardless of whether reading restarted, see above.
    this->RemoveHold();
    return true;
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
  /// is called.
  /// @param ok true: a response is received. false: the stream reader is closed
  ///           (but not the RPC itself).
  void OnReadDone(const bool ok) override {
    // (Point 2.1, 2.2, 2.3, 4.1, 4.2) Event received from stream
    response_ready_ = ok;
    if (!ok) {
      stream_no_more_ = true;
      // (Point 4.2) OnReadDone: False
      if (cbs_.nok) cbs_.nok(this);
      return;
    }
    // (Point 2.3) OnReadDone: true
    if (cbs_.ok && cbs_.ok(this, response_)) {
      // Hold the RPC until the application thread calls StartRead() again from GetResponse().
      // See "Why OnReadDone holds before returning" in reactor_client.md for why this hold
      // exists: https://github.com/grpc/grpc/pull/18072
      // (Point 2.5) Holding the RPC
      this->AddHold();
      return;
    }
    response_ready_ = false;
    // (Point 2.15, 2.16) Restart reading
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
      // (Point 4.5) OnDone
      cbs_.done(this, status);
    }
  }

 protected:
  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  ResponseT response_;  ///< response holder

 private:
  grpc::Status status_;
  ActiveReadCallbacks<ResponseT> cbs_;

  // The application MAY call GetResponse() while a gRPC thread is on OnReadDone().
  // That concurrent situation should not happen by design, unless the application
  // is not waiting for the OnReadDoneOkCallback prior reading the response_;
  // Once response_ready_ is set, the response_ may be thread-safely used by the application.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool response_ready_{false};

  // Once we got OnReadDone(false) or OnDone(), no more StartRead() must be called.
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
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  /// @param response reference to the response message the reactor received
  using OnDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>*,
                                            const grpc::Status&,
                                            const ResponseT&)>;
  OnDoneCallback done;  ///< Slot for ClientWriteReactor::OnDone event
};

/// Template class for stream-writer RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides GetResponse(), Status())
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
  ///         or a prior write/the RPC has already failed)
  bool SendRequest(RequestT&& request) {
    if (stream_no_more_) return false;  // A prior write failed, or the RPC is done
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
  ///         or a prior write/the RPC has already failed)
  bool SendLastRequest(RequestT&& request) {
    if (stream_no_more_) return false;  // A prior write failed, or the RPC is done
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
  ///         or the RPC has already failed) - callers should wait for OnWriteDone() and retry.
  bool CloseRequestStream() {
    if (stream_no_more_) return false;  // Same guard as SendRequest(), see above
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

  /// Swaps the underlying data storage of the response object.
  /// The swap mechanism is important to avoid a deep-copy of the content of the
  /// response. Having the response swapped is acceptable because the client-streaming RPC
  /// is meant to receive one final response only, so the swap is a good technique to speed up
  /// the response proceeding time.
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) {
    if (!response_ready_) return false;
    swap(response_, response);  // Moving the read content on the user side
    response_ready_ = false;
    return true;
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

  // UPCOMING: gRPC's AddMultipleHolds(int holds) reserves a fixed hold budget upfront (before
  // StartCall()) for independent operation-flows happening outside the reactions - unlike the
  // single-hold, per-message pattern already used for reads elsewhere in this file. Not used by
  // any application code in this project yet, and the intended lifetime/usage on the write side
  // has not been designed. Left commented out until there is an application need for it.
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
    response_ready_ = status.ok();
    if (cbs_.done) {
      status_ = status;  // doing deep-copy unfortunately
      cbs_.done(this, status, response_);
    }
  }

 protected:
  std::unique_ptr<grpc::ClientContext> context_;  ///< gRPC client context for this RPC
  ResponseT response_;  ///< response holder

 private:
  grpc::Status status_;
  ActiveWriteCallbacks<RequestT, ResponseT> cbs_;

  // Storage for the request currently being written. SendRequest() moves the caller's argument
  // here so StartWrite() has a stable pointer that survives past the caller's own statement -
  // the application gives up the request once SendRequest() accepts it.
  RequestT pending_request_;

  // Flag indicating the response is ready to be read via GetResponse()
  // Set by gRPC thread, read by application thread.
  std::atomic_bool response_ready_{false};

  // Flag indicating a write operation is in progress.
  // gRPC requires that only one write be in flight at a time.
  // Set by application thread via StartWrite, cleared by gRPC thread via OnWriteDone.
  std::atomic_bool write_pending_{false};

  // Flag indicating CloseRequestStream() has been called.
  // After this, no more SendRequest() calls are allowed.
  // Set by application thread, read by application thread.
  std::atomic_bool writes_done_{false};

  // Once we got OnWriteDone(false) or OnDone(), no more StartWrite() must be called.
  // Mirrors stream_no_more_ on the read side (ActiveReadReactor/ActiveBidiReactor).
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
  /// @param reactor instance pointer on which the event is received
  /// @param response reference to the response message the reactor received
  /// @retval true the response must be kept intact by the reactor until read later through GetResponse() function.
  ///         The reactor is put on hold in the meantime.
  /// @retval false the response is unwanted and the reactor can immediately execute a new read operation to obtain
  ///         a new response.
  using OnReadDoneOkCallback = std::function<bool(grpc::ClientBidiReactor<RequestT, ResponseT>*, const ResponseT&)>;
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
/// Active Object components: Method Request (encapsulates RPC state) & Future (provides GetResponse(), Status())
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
  ActiveBidiReactor(std::unique_ptr<grpc::ClientContext> context,
                    ActiveBidiCallbacks<RequestT, ResponseT>&& cbs)
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
    // stream_no_more_ is set by OnReadDone(false), OnWriteDone(false), and OnDone() - per gRPC's
    // own contract (grpcpp/support/client_callback.h), a failure on either read or write means no
    // new read/write operation will succeed, so one flag covers both directions. This narrows but
    // does not fully close the race: the flag could still flip to true right after this check
    // passes. A full fix needs the same AddHold()/RemoveHold() protection OnReadDone() uses for
    // the read side.
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
  /// pair. The caller gives up the request the same way SendRequest() requires. The server may
  /// continue sending responses.
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
  /// The server may continue sending responses.
  /// @return true if the close was initiated, false if rejected (already closed, write pending,
  ///         or the RPC has already finished/is finishing) - callers should wait for
  ///         OnWriteDone() and retry.
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

  /// Swaps the underlying data storage of the response object and resume the held RPC.
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
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) {
    if (!response_ready_) return false;
    swap(response_, response);  // Moving the read content on the user side
    response_ready_ = false;
    if (!stream_no_more_) {
      // Restart reading
      this->StartRead(&response_);
    }
    // Resuming RPC - must run regardless of whether reading restarted, see above.
    this->RemoveHold();
    return true;
  }

  /// Obtain the status of the RPC set by the `OnDone` event. Calling this
  /// function at any other moment is meaningless.
  /// @return reference to the grpc::Status object
  const grpc::Status& Status() { return status_; }

  // UPCOMING: gRPC's AddMultipleHolds(int holds) reserves a fixed hold budget upfront (before
  // StartCall()) for independent operation-flows happening outside the reactions - e.g. one hold
  // for the read-flow and one for the write-flow, as an alternative to the single-hold, per-message
  // pattern already used for reads in OnReadDone()/GetResponse() below. This could close the
  // remaining race noted in SendRequest()/CloseRequestStream() (see comments there), but the
  // lifetime/usage on the write side has not been designed and no application code uses it yet.
  // Left commented out until there is an application need for it.
  // void UseMultipleHolds() {
  //     this->AddMultipleHolds(/* holds */ 2);
  // }

 protected:
  /// This event function is called by gRPC when the stream has a read event. The user-side
  /// callback is then called, but on the same gRPC thread.
  /// Based on the value of the `ok` flag, the OnReadDoneOkCallback or OnReadDoneNOkCallback
  /// is called.
  /// @param ok true: a response is received. false: the read stream is closed.
  void OnReadDone(const bool ok) override {
    response_ready_ = ok;
    if (!ok) {
      stream_no_more_ = true;
      if (cbs_.read_nok) cbs_.read_nok(this);
      return;
    }
    if (cbs_.read_ok && cbs_.read_ok(this, response_)) {
      // Hold the RPC until the application thread calls StartRead() again from GetResponse().
      // See "Why OnReadDone holds before returning" in reactor_client.md for why this hold
      // exists: https://github.com/grpc/grpc/pull/18072
      this->AddHold();
      return;
    }
    response_ready_ = false;
    this->StartRead(&response_);
  }

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
  ResponseT response_;  ///< response holder

 private:
  grpc::Status status_;
  ActiveBidiCallbacks<RequestT, ResponseT> cbs_;

  // Storage for the request currently being written. SendRequest() moves the caller's argument
  // here so StartWrite() has a stable pointer that survives past the caller's own statement -
  // the application gives up the request once SendRequest() accepts it.
  RequestT pending_request_;

  // The application MAY call GetResponse() while a gRPC thread is on OnReadDone().
  // That concurrent situation should not happen by design, unless the application
  // is not waiting for the OnReadDoneOkCallback prior reading the response_;
  // Once response_ready_ is set, the response_ may be thread-safely used by the application.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool response_ready_{false};

  // Once we got OnReadDone(false), OnWriteDone(false), or OnDone(), no more StartRead() or
  // StartWrite() must be called. Per gRPC's own contract (grpcpp/support/client_callback.h), a
  // failure on either direction means neither will succeed anymore, so one flag covers both.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool stream_no_more_{false};

  // Flag indicating a write operation is in progress.
  // gRPC requires that only one write be in flight at a time.
  // Set by application thread via StartWrite, cleared by gRPC thread via OnWriteDone.
  std::atomic_bool write_pending_{false};

  // Flag indicating CloseRequestStream() has been called.
  // After this, no more SendRequest() calls are allowed.
  // Set by application thread, read by application thread.
  std::atomic_bool writes_done_{false};
};
}  // namespace RpcReactor::Client
