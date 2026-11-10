///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <spdlog/logger.h>

#include <memory>
#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase::coro {

/// A use case: a route is recorded while driving, each point sent as the GPS produces it, and the summary of the
/// trip goes to the trip history once the drive ends. Points appear over time, and not at the pace of the
/// network: they can come faster than a write completes, such as the fixes a GPS receiver delivers together
/// after a tunnel.
///
/// The variant chosen is coro::Writer over ActiveWriteReactor under Delivery::kPush and
/// Submission::kApplicationPushes, for the reasons live_route_recording.h gives: a point may not exist yet when
/// the previous write completes, and the trip history keeps the summary it is handed. Here, the coroutine awaits
/// the next fix, then its write: a fix that came while a write was in flight has waited for the coroutine, which
/// sends it as soon as the write resumes it, so no queue is kept. A delay awaited with EventLoopExecutor::After()
/// stands in for the GPS.
///
/// Coroutine twin of live_route_recording.h, and clone of record_route_push_coro.h along the use only: the
/// awaitable is the same, and the diff is the GPS the coroutine awaits before each write, and the trip history.
class LiveRouteRecording {
  // The reactor flavour this scenario uses: its Delivery approach, then its Submission.
  using Delivery = routeguide::RecordRoute::Delivery;
  using Submission = routeguide::RecordRoute::Submission;
  using Writer =
      RpcReactor::coro::Writer<routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kApplicationPushes>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  LiveRouteRecording(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  LiveRouteRecording(const LiveRouteRecording&) = delete;
  LiveRouteRecording& operator=(const LiveRouteRecording&) = delete;
  LiveRouteRecording(LiveRouteRecording&&) = delete;
  LiveRouteRecording& operator=(LiveRouteRecording&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~LiveRouteRecording() = default;

  /// Proxy component: spawns the coroutine, which starts the RPC, switches the GPS on, and returns at its first
  /// suspension. Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param drive the points the GPS will produce, one fix each, in order
  void Start(std::vector<routeguide::Point> drive);

 private:
  /// Servant component, as one function: awaits each GPS fix, then its write, closes the request stream, then
  /// hands the summary to the trip history.
  /// @param drive the points the GPS will produce, taken by value since the coroutine outlives Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(std::vector<routeguide::Point> drive, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  // The summaries of past drives. Application thread only.
  std::vector<std::unique_ptr<routeguide::RecordRoute::ResponseT>> trip_history_;
  spdlog::logger& logger_{VariantLogger("LiveRouteRecording.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::usecase::coro
