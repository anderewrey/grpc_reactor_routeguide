///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <spdlog/logger.h>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase::coro {

/// A use case: a trip recorder, the "black box" of a vehicle, stores every position report of a trip in its
/// trip log. Every report matters, since a missing one is a gap in the trip, but the vehicle sends them at its
/// own pace, and storage must not slow the feed down when it lags behind.
///
/// The variant chosen is coro::Reader with its unbounded buffer, over ActiveReadReactor under Delivery::kPush
/// and ReadPacing::kContinuous, for the reason trip_recorder.h gives: each report is handed over owned, and the
/// reactor reads on at once, so the feed runs at the server's pace whatever storage does. The coroutine stores
/// each report, which takes a while, so the reports it has not stored yet pile up in the reader's queue: that
/// backlog is the price of the variant, since nothing bounds it. ListFeatures stands in for the feed: its
/// features are read as position reports.
///
/// Coroutine twin of trip_recorder.h, and clone of list_features_push_continuous_coro.h along the use only: the
/// awaitable is the same, and the diff is the storage, and the backlog the coroutine reads with Queued() after
/// each report. Its callback twin measures the backlog on the gRPC thread, where no coroutine code runs.
class TripRecorder {
  // The reactor flavour this scenario uses: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Reader =
      RpcReactor::coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  TripRecorder(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  TripRecorder(const TripRecorder&) = delete;
  TripRecorder& operator=(const TripRecorder&) = delete;
  TripRecorder(TripRecorder&&) = delete;
  TripRecorder& operator=(TripRecorder&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~TripRecorder() = default;

  /// Proxy component: spawns the coroutine, which sends the request and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Servant component, as one function: sends the request, stores every report in the trip log, in order,
  /// then awaits the end of the RPC and reports the totals.
  /// @param rect bounding the features to list, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(routeguide::Rectangle rect, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("TripRecorder.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::usecase::coro
