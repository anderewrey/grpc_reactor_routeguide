///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <spdlog/logger.h>

#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::push::coro {

/// Exhibits coro::Writer, the awaited writes, over ActiveWriteReactor under Delivery::kPush and
/// Submission::kApplicationPushes, via the RecordRoute RPC. One coroutine per RPC runs on the application thread
/// from start to end: each Write() resumes it once its write has completed, Close() ends the request stream,
/// and Finish() resumes it with the status and the summary, moved out of the message the done callback handed
/// over.
///
/// Coroutine twin of record_route_push.h, deliberately: the reactor and its configuration are the same, and
/// `diff record_route_push.cpp record_route_push_coro.cpp` is what consuming them from a coroutine changes. The
/// callbacks, the event names, their handlers, the pending queue and its pump, and the reactor slot are gone:
/// the loop over the points is the pump, and one write is in flight at most since the coroutine cannot start
/// the next before the last one resumes it.
class RecordRoute {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its Submission.
  using Delivery = routeguide::RecordRoute::Delivery;
  using Submission = routeguide::RecordRoute::Submission;
  using Writer =
      RpcReactor::coro::Writer<routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kApplicationPushes>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  RecordRoute(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  RecordRoute(const RecordRoute&) = delete;
  RecordRoute& operator=(const RecordRoute&) = delete;
  RecordRoute(RecordRoute&&) = delete;
  RecordRoute& operator=(RecordRoute&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~RecordRoute() = default;

  /// Proxy component: spawns the coroutine, which starts the RPC and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param points to stream to the server, one write each
  void Start(std::vector<routeguide::Point> points);

 private:
  /// Servant component, as one function: writes each point, awaiting each write, closes the request stream,
  /// then awaits the summary.
  /// @param points to stream, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(std::vector<routeguide::Point> points, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("RecordRoute.push.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::push::coro
