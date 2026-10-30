///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <atomic>
#include <memory>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: a navigation app saves a region's places for offline use before a trip. Every place must
/// be saved, and the download should run at full speed, but the disk writer is slower than the network:
/// once too many places wait for it, the download must pause rather than fill memory, and resume once
/// the writer has caught up.
///
/// The variant chosen is ActiveReadReactor under Delivery::kPush and ReadPacing::kPerMessage. Each place
/// is handed out owned and queued for the writer, and read_ok decides for each one whether the stream
/// waits: it reads on while few places wait for the writer, and holds once kMaxInFlight do, until the
/// writer has saved them and the handler of the held place calls ResumeRead(). ListFeatures stands in
/// for the download: the rectangle is the region.
///
/// Clone of list_features_push_per_message.h, along the use only: the reactor is the same, and the diff
/// is the decision, which here follows from the writer's backlog rather than from an arbitrary rule.
class OfflineMapDownload {
  // The reactor flavour this scenario uses: its Delivery approach only. Its ReadPacing is the
  // default, ReadPacing::kPerMessage.
  using Delivery = routeguide::ListFeatures::Delivery;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit OfflineMapDownload(routeguide::RouteGuide::Stub& stub);

  OfflineMapDownload(const OfflineMapDownload&) = delete;
  OfflineMapDownload& operator=(const OfflineMapDownload&) = delete;
  OfflineMapDownload(OfflineMapDownload&&) = delete;
  OfflineMapDownload& operator=(OfflineMapDownload&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~OfflineMapDownload();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(const routeguide::Rectangle& rect);

 private:
  /// Application-thread handler for OnReadDoneOk.
  /// Delta against the turn-by-turn counterpart: this handler saves the place, frees its in-flight slot,
  /// and ends with ResumeRead() only for a message read_ok held. No read is armed between a holding
  /// callback and that call, so read_ok #k+1 cannot appear before the resumed #k line.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the totals and destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared. They stay in step because the event queue
  // is FIFO and at most one OnReadDone reaction per RPC is in flight.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  // Places read but not yet saved: incremented by read_ok on the gRPC thread, decremented by the
  // handler on the application thread, so it is shared, and atomic rather than locked.
  std::atomic<unsigned> in_flight_{0};
  // How many times read_ok held the stream: gRPC thread, read by OnDone once every reaction has returned.
  std::atomic<unsigned> holds_{0};
  // The sequence number of the message read_ok held, or 0 when none is: set on the gRPC thread, and
  // claimed by that message's own handler on the application thread.
  std::atomic<unsigned> held_seq_{0};

  spdlog::logger& logger_{VariantLogger("OfflineMapDownload")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
