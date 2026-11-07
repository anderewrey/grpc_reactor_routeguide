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

namespace rg_demo::pull::coro {

/// Exhibits coro::Unary, the unary awaitable, over ActiveUnaryReactor under Delivery::kPull, via the GetFeature
/// RPC. One coroutine per RPC runs on the application thread from start to end: it awaits Finish(), which
/// resumes it once OnDone() has run, with the status and the response swapped out of the reactor.
///
/// Coroutine twin of get_feature_pull.h, deliberately: the reactor and its configuration are the same, and
/// `diff get_feature_pull.cpp get_feature_pull_coro.cpp` is what consuming them from a coroutine changes. The
/// done callback, the event name, its handler and the reactor slot are gone, all inside the awaitable.
class GetFeature {
  // The reactor flavour this scenario exhibits starts with its Delivery approach.
  using Delivery = routeguide::GetFeature::Delivery;
  using Call = RpcReactor::coro::Unary<routeguide::GetFeature::ClientReactor<Delivery::kPull>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  GetFeature(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  GetFeature(const GetFeature&) = delete;
  GetFeature& operator=(const GetFeature&) = delete;
  GetFeature(GetFeature&&) = delete;
  GetFeature& operator=(GetFeature&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~GetFeature() = default;

  /// Proxy component: spawns the coroutine, which sends the request and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param point to look up
  void Start(routeguide::Point point);

 private:
  /// Servant component, as one function: sends the request, awaits the end of the RPC, and logs the answer.
  /// @param point to look up, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(routeguide::Point point, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("GetFeature.pull.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::pull::coro
