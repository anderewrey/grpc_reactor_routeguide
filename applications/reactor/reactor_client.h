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

/// Template callbacks for stream-reader RPC client reactor. It contains all available callbacks slots
/// needed by specialized RPC client reactors.
/// @tparam ResponseT type of protobuf message the RPC handles
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
struct ActiveReadCallbacks {
  /// Function signature for ClientReadReactor::OnReadDone event with positive OK flag.
  /// The reactor swaps the message out before this call and re-arms its read after it, so the
  /// callback owns the message and the reactor never refers to it again. Handing that ownership on
  /// carries obligations documented in reactor_client.md, under "Message ownership".
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
  ActiveReadReactor(std::unique_ptr<grpc::ClientContext> context,
                    ActiveReadCallbacks<ResponseT>&& cbs)
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
  void TryCancel() const {
    // (Point 3.2, 3.3) RPC termination behest
    context_->TryCancel();
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
    // (Point 2.3, 4.2) Event received from stream
    if (!ok) {
      // (Point 4.3) OnReadDone: False
      if (cbs_.read_nok) cbs_.read_nok(this);
      return;
    }
    // (Point 2.3) OnReadDone: true
    if (cbs_.read_ok) {
      // (Point 2.4) extracts response, by pointer swap rather than deep-copy
      auto message = std::make_unique<ResponseT>();
      swap(*message, response_);
      // (Point 2.5) TriggerEvent: OnReadDoneOk
      cbs_.read_ok(this, std::move(message));
    }
    // (Point 2.6) Restart reading. Last action of the reaction, which is what removes the need for a
    // hold. See "Why ActiveReadReactor needs no hold" in reactor_client.md.
    this->StartRead(&response_);
  }
  /// This event function is called by gRPC when the RPC is done and no more operation is possible with that reactor
  /// instance. The OnDoneCallback is then called, but on the same gRPC thread. The received status
  /// is also copied into the reactor.
  /// @param status info coming from gRPC
  void OnDone(const grpc::Status& status) override {
    // (Point 4.4, 4.5) RPC termination
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
    // right after this check passes. Closing it needs a hold covering the write flow, taken
    // before StartCall(), which is the UseMultipleHolds() sketch below. Which reactions set the
    // flag is listed at its declaration.
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

  // UPCOMING: gRPC's AddMultipleHolds(int holds) reserves a fixed hold budget upfront (before
  // StartCall()) for an operation-flow issued outside the reactions, which is what SendRequest()
  // does from the application thread. It could close the race noted there, but the intended
  // lifetime/usage on the write side has not been designed and no application code uses it yet.
  // Left commented out until there is an application need for it.
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
  /// The reactor swaps the message out before this call and re-arms its read after it, so the
  /// callback owns the message and the reactor never refers to it again. Handing that ownership on
  /// carries obligations documented in reactor_client.md, under "Message ownership".
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
    // The stream_no_more_ check narrows a race it cannot close: a gRPC thread can set the flag
    // right after this check passes. Closing it needs a hold covering the write flow, taken
    // before StartCall(), which is the UseMultipleHolds() sketch below. Which reactions set the
    // flag is listed at its declaration.
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

  // UPCOMING: gRPC's AddMultipleHolds(int holds) reserves a fixed hold budget upfront (before
  // StartCall()) for an operation-flow issued outside the reactions, which is what SendRequest()
  // does from the application thread. It could close the race noted there, but the intended
  // lifetime/usage on the write side has not been designed and no application code uses it yet.
  // Left commented out until there is an application need for it.
  // void UseMultipleHolds() {
  //     this->AddMultipleHolds(/* holds */ 1);
  // }

 protected:
  /// This event function is called by gRPC when the stream has a read event. The user-side
  /// callback is then called, but on the same gRPC thread.
  /// Based on the value of the `ok` flag, the OnReadDoneOkCallback or OnReadDoneNOkCallback
  /// is called.
  /// @param ok true: a response is received. false: the read stream is closed.
  void OnReadDone(const bool ok) override {
    if (!ok) {
      stream_no_more_ = true;
      if (cbs_.read_nok) cbs_.read_nok(this);
      return;
    }
    if (cbs_.read_ok) {
      // extracts response, by pointer swap rather than deep-copy
      auto message = std::make_unique<ResponseT>();
      swap(*message, response_);
      cbs_.read_ok(this, std::move(message));
    }
    // Restart reading. Last action of the reaction, which is what removes the need for a hold.
    // See "Why the read side needs no hold" in reactor_client.md.
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
  ResponseT response_;  ///< internal read target, swapped out on each read, never exposed

 private:
  grpc::Status status_;
  ActiveBidiCallbacks<RequestT, ResponseT> cbs_;

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

  // Once we got OnReadDone(false), OnWriteDone(false), or OnDone(), no more StartRead() or
  // StartWrite() must be called. Per gRPC's own contract (grpcpp/support/client_callback.h), a
  // failure on either direction means neither will succeed anymore, so one flag covers both.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool stream_no_more_{false};
};
}  // namespace RpcReactor::Client
