///
/// Copyright 2024-2025 anderewrey.
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
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
 * It is as much as generic possible
 ************************/
namespace RpcReactor::Client {
/// template class for unary RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// @tparam ResponseT type of protobuf message the RPC handles
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
class ProxyUnaryReactor : public grpc::ClientUnaryReactor {
 public:
  /// Function signature for ClientUnaryReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  /// @param response reference to the response message the reactor received
  using OnDoneCallback = std::function<void(ProxyUnaryReactor*, const grpc::Status&, const ResponseT&)>;
  /// all the available callbacks slots for this reactor class
  /// TODO(ajcote) Would be nicer to have these callback signatures outside of the template class. But to achieve
  ///  this, it means the callback must also be generic about the protobuf response message.
  struct callbacks {
    OnDoneCallback done;  ///< Slot for ClientUnaryReactor::OnDone event
  };

  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ProxyUnaryReactor(std::unique_ptr<grpc::ClientContext> context, callbacks&& cbs)
      : context_(std::move(context)),
        cbs_(std::move(cbs)) {}

  /// Destructor of the reactor class. It tells the gRPC connection to close the channel.
  /// If the context/channel is already closed, there's no problem to TryCancel() it again.
  ~ProxyUnaryReactor() override {
    context_->TryCancel();
  }

  /// This class cannot be copied.
  ProxyUnaryReactor(const ProxyUnaryReactor&) = delete;
  /// This class cannot be copied.
  ProxyUnaryReactor& operator=(const ProxyUnaryReactor&) = delete;
  /// This class cannot be moved.
  ProxyUnaryReactor(ProxyUnaryReactor&&) = delete;
  /// This class cannot be moved.
  ProxyUnaryReactor& operator=(ProxyUnaryReactor&&) = delete;

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
  callbacks cbs_;

  // The application MAY call (but should not) GetResponse() while a gRPC thread is on OnDone().
  // That concurrent situation should not happen by design, unless the application
  // is not waiting for the OnDoneCallback prior reading the response_;
  // Once response_ready_ is set, the response_ may be thread-safely used by the application.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool response_ready_{false};
};

/// template class for stream-reader RPC client reactor. This class is derived again by
/// specialized RPC client reactors.
/// @tparam ResponseT type of protobuf message the RPC handles
template <class ResponseT>
requires std::derived_from<ResponseT, google::protobuf::Message>
class ProxyReadReactor : public grpc::ClientReadReactor<ResponseT> {
 public:
  /// Function signature for ClientReadReactor::OnReadDone event with positive OK flag
  /// @param reactor instance pointer on which the event is received
  /// @param response reference to the response message the reactor received
  /// @retval true the response must be kept intact by the reactor until read later through GetResponse() function.
  ///         The reactor is put on hold in the meantime.
  /// @retval false the response is unwanted and the reactor can immediately execute a new read operation to obtain
  ///         a new response.
  using OnReadDoneOkCallback = std::function<bool(ProxyReadReactor*, const ResponseT&)>;
  /// Function signature for ClientReadReactor::OnReadDone event with negative OK flag
  /// @param reactor instance pointer on which the event is received
  using OnReadDoneNOkCallback = std::function<void(ProxyReadReactor*)>;
  /// Function signature for ClientReadReactor::OnDone event. This event function is called by gRPC when the RPC is
  /// done and no more operation is possible with that reactor instance.
  /// @param reactor instance pointer on which the event is received
  /// @param status reference to the reason of the event
  using OnDoneCallback = std::function<void(ProxyReadReactor*, const grpc::Status&)>;
  /// all the available callbacks slots for this reactor class
  /// TODO(ajcote) Would be nicer to have these callback signatures outside of the template class. But to achieve
  ///  this, it means the callback must also be generic about the protobuf response message.
  struct callbacks {
    OnReadDoneOkCallback ok;    ///< Slot for ClientReadReactor::OnReadDone event with positive OK flag
    OnReadDoneNOkCallback nok;  ///< Slot for ClientReadReactor::OnReadDone event with negative OK flag
    OnDoneCallback done;        ///< Slot for ClientReadReactor::OnDone event
  };

  /// Constructor of the reactor class. It moves the received objects as members.
  /// @param context given to this reactor about the ongoing RPC method
  /// @param cbs given to this reactor to use as callable functions
  ProxyReadReactor(std::unique_ptr<grpc::ClientContext> context,
                   callbacks&& cbs)
      : context_(std::move(context)),
        cbs_(std::move(cbs)) { }

  /// Destructor of the reactor class. It tells the gRPC connection to close the channel.
  /// If the context/channel is already closed, there's no problem to TryCancel() it again.
  ~ProxyReadReactor() override {
    context_->TryCancel();
  }

  /// This class cannot be copied nor moved
  ProxyReadReactor(const ProxyReadReactor&) = delete;
  /// This class cannot be copied nor moved
  ProxyReadReactor& operator=(const ProxyReadReactor&) = delete;
  /// This class cannot be copied nor moved
  ProxyReadReactor(ProxyReadReactor&&) = delete;
  /// This class cannot be copied nor moved
  ProxyReadReactor& operator=(ProxyReadReactor&&) = delete;

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
  /// @param[out] response instance to swap
  /// @return true when the returned response is valid, false otherwise.
  bool GetResponse(ResponseT& response) {
    if (!response_ready_) return false;
    // (Point 2.8, 2.14) extracts response
    swap(response_, response);  // Moving the read content on the user side
    response_ready_ = false;
    if (!read_no_more_) {
      // (Point 2.9, 2.10) Restart reading
      this->StartRead(&response_);
      // (Point 2.11) Resuming RPC
      this->RemoveHold();
    }
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
      read_no_more_ = true;
      // (Point 4.2) OnReadDone: False
      if (cbs_.nok) cbs_.nok(this);
      return;
    }
    // (Point 2.3) OnReadDone: true
    if (cbs_.ok && cbs_.ok(this, response_)) {
      // The next StartRead() is taking place outside of this gRPC event (see GetResponse above),
      // and so the RPC must be put on hold until the application thread took care of
      // the response and requested a new reading.
      // If that hold is not enforced, a concurrent OnDone event may be received
      // while the application is exactly on the point to call StartRead(). Prior
      // OnDone is got, the underlying bound callback (raw pointer) is already
      // destroyed by gRPC, so any operation request will segfault. IMO, gRPC
      // can protect itself easily against that situation, but it doesn't (gRPC 1.51.1)
      // and instead introduced that Hold mechanism: https://github.com/grpc/grpc/pull/18072
      // So the solution is to call AddHold() here and when the application thread
      // proceeded the response, calling StartRead() and then RemoveHold()
      // does the needed protection to ensure the underlying callback is still
      // living.
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
    read_no_more_ = true;
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
  callbacks cbs_;

  // The application MAY call GetResponse() while a gRPC thread is on OnReadDone().
  // That concurrent situation should not happen by design, unless the application
  // is not waiting for the OnReadDoneOkCallback prior reading the response_;
  // Once response_ready_ is set, the response_ may be thread-safely used by the application.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool response_ready_{false};

  // Once we got a OnReadDone(N-OK) or OnDone(), no more StartRead() must be called.
  // Set by gRPC thread, read by application thread.
  std::atomic_bool read_no_more_{false};
};
}  // namespace RpcReactor::Client
