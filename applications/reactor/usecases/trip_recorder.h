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

/// A use case: a trip recorder, the "black box" of a vehicle, stores every position report of a trip
/// in its trip log. Every report matters, since a missing one is a gap in the trip, but the vehicle sends
/// them at its own pace, and storage must not slow the feed down when it lags behind.
///
/// The variant chosen is ActiveReadReactor under Delivery::kPush and ReadPacing::kContinuous. Each report
/// is handed out owned and queued as its own event, and the reactor reads on at once, so the feed runs at
/// the server's pace whatever storage does. The application-thread handler stores each report, which takes
/// a while, so the reports it has not stored yet pile up in the event queue: that backlog is the price of
/// the variant, since nothing bounds it. ListFeatures stands in for the feed: its features are read as
/// position reports.
///
/// Clone of list_features_push_continuous.h, along the use only: the reactor is the same, and the diff is
/// what an application adds to see the backlog it accepted. Nothing is dropped, nothing waits, and the RPC
/// ends before storage has caught up, so the end of the RPC is not the end of the work.
class TripRecorder {
  // The reactor flavour this scenario uses: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush, ReadPacing::kContinuous>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit TripRecorder(routeguide::RouteGuide::Stub& stub);

  TripRecorder(const TripRecorder&) = delete;
  TripRecorder& operator=(const TripRecorder&) = delete;
  TripRecorder(TripRecorder&&) = delete;
  TripRecorder& operator=(TripRecorder&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~TripRecorder();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(const routeguide::Rectangle& rect);

 private:
  /// Application-thread handler for OnReadDoneOk, which stores one report in the trip log.
  /// Delta against list_features_push_continuous.h: storing takes a while, so reports queue behind it.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the totals and destroys the reactor. Every report
  /// was queued before this event, so the trip log holds them all by the time it runs.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // Delta against list_features_push_continuous.h: both counters are shared, so the gRPC thread can
  // measure the backlog. The read counter is incremented only on the gRPC thread, in read_ok, and the
  // stored counter only on the application thread; they stay in step because the event queue is FIFO.
  std::atomic<unsigned> read_seq_{0};
  std::atomic<unsigned> stored_seq_{0};
  // The largest backlog seen, written on the gRPC thread and read by the OnDone handler.
  std::atomic<unsigned> peak_backlog_{0};

  spdlog::logger& logger_{VariantLogger("TripRecorder")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
