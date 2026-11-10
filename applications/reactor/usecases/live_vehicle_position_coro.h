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

/// A use case: a dashboard draws a vehicle's position on a map, from a live feed of position fixes. A fix is worth
/// drawing only until a newer one exists, and the map takes longer to redraw than the feed takes to send the next
/// fix, so the map must skip to the newest fix rather than fall behind.
///
/// The variant chosen is coro::Reader with its latest-only buffer, over ActiveReadReactor under Delivery::kPull
/// and ReadPacing::kContinuous, for the reason live_vehicle_position.h gives: the feed never waits for the map.
/// The reader copies each lent fix into its slot, replacing one the map has not drawn yet, and each Read() resumes
/// the coroutine with the newest fix, so fixes arriving during a redraw replace each other in the slot.
/// ListFeatures stands in for the feed: its features are read as successive fixes.
///
/// Coroutine twin of live_vehicle_position.h, and clone of list_features_pull_continuous_coro.h along the use
/// only: the awaitable is the same, and the diff is the redraw. The callback twin keeps the slot itself, a
/// LatestSlot on the application side; here the slot is the reader's buffer, and Overwritten() reports the fixes
/// it replaced, which the callback twin counts with LatestSlot::Replaced().
class LiveVehiclePosition {
  // The reactor flavour this scenario uses: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Reader =
      RpcReactor::coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  LiveVehiclePosition(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  LiveVehiclePosition(const LiveVehiclePosition&) = delete;
  LiveVehiclePosition& operator=(const LiveVehiclePosition&) = delete;
  LiveVehiclePosition(LiveVehiclePosition&&) = delete;
  LiveVehiclePosition& operator=(LiveVehiclePosition&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~LiveVehiclePosition() = default;

  /// Proxy component: spawns the coroutine, which sends the request and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Servant component, as one function: sends the request, draws the newest fix each time, and counts the fixes
  /// replaced before they were drawn, then awaits the end of the RPC and reports the totals.
  /// @param rect bounding the features to list, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(routeguide::Rectangle rect, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("LiveVehiclePosition.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::usecase::coro
