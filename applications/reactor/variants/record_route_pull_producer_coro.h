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

namespace rg_demo::pull::producer::coro {

/// Exhibits coro::Source, a range the reactor pulls from, over ActiveWriteReactor under Delivery::kPull and
/// Submission::kReactorPulls, via the RecordRoute RPC. One coroutine per RPC runs on the application thread: it
/// hands the points to the Source, and Finish() resumes it once the reactor has sent them all and the RPC has
/// ended, with the status and the summary, swapped out of the reactor.
///
/// Coroutine twin of record_route_pull_producer.h, deliberately: the reactor and its configuration are the
/// same, and `diff record_route_pull_producer.cpp record_route_pull_producer_coro.cpp` is what consuming them
/// from a coroutine changes. The callbacks, the event names, their handlers, the queue and its next_request
/// body, and the reactor slot are gone, all inside the awaitable. So are the "next" lines the callback parent
/// logs from next_request: a pull runs on a gRPC thread, and a coroutine scenario has no code there.
class RecordRoute {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its Submission.
  using Delivery = routeguide::RecordRoute::Delivery;
  using Submission = routeguide::RecordRoute::Submission;
  using Source =
      RpcReactor::coro::Source<routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>>;

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

  /// Proxy component: spawns the coroutine, whose Source pulls the first point before the coroutine suspends.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param points to stream to the server, pulled one at a time by the reactor
  void Start(std::vector<routeguide::Point> points);

 private:
  /// Servant component, as one function: hands the points to the reactor, then awaits the summary.
  /// @param points to stream, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(std::vector<routeguide::Point> points, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("RecordRoute.pull.producer.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::pull::producer::coro
