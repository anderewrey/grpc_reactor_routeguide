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

/// A use case: a guidance screen shows the stops of a route one at a time. The next stop is fetched only
/// once the driver confirms the current one, so the screen never runs ahead of the driver, however fast
/// the server could send the route.
///
/// The variant chosen is coro::Reader over ActiveReadReactor under Delivery::kPush and ReadPacing::kTurnByTurn,
/// for the reason turn_by_turn_guidance.h gives: the stop is handed over owned, and nothing is read until the
/// coroutine asks. Here, asking is the next Read(), which the coroutine reaches only once the driver has
/// confirmed. Under Delivery::kPull, Read() would arm the next read as it takes the stop, so the stream would
/// run ahead of the driver. A delay awaited with EventLoopExecutor::After() stands in for the driver.
/// ListFeatures stands in for the route: its features are read as successive stops.
///
/// Coroutine twin of turn_by_turn_guidance.h, and clone of list_features_push_turn_by_turn_coro.h along the use
/// only: the awaitable is the same, and the diff is the screen that keeps the stop, and the confirmation the
/// coroutine awaits before its next Read().
class TurnByTurnGuidance {
  // The reactor flavour this scenario uses: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Reader =
      RpcReactor::coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kTurnByTurn>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  TurnByTurnGuidance(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  TurnByTurnGuidance(const TurnByTurnGuidance&) = delete;
  TurnByTurnGuidance& operator=(const TurnByTurnGuidance&) = delete;
  TurnByTurnGuidance(TurnByTurnGuidance&&) = delete;
  TurnByTurnGuidance& operator=(TurnByTurnGuidance&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~TurnByTurnGuidance() = default;

  /// Proxy component: spawns the coroutine, which sends the request and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Servant component, as one function: sends the request, shows each stop until the driver confirms it,
  /// then awaits the end of the RPC.
  /// @param rect bounding the features to list, taken by value since the coroutine outlives the call to Start()
  RpcReactor::coro::Task<> Run(routeguide::Rectangle rect);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("TurnByTurnGuidance.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::usecase::coro
