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

/// A use case: someone taps the map to ask what is at that point, and a card shows the answer: the
/// name of the place there, or that nothing named is there. One question gets one answer, read once,
/// where it arrives.
///
/// The variant chosen is co::Unary over ActiveUnaryReactor under Delivery::kPull, for the reason
/// whats_here.h gives: the card is filled in where the answer arrives, and nothing keeps it after that.
/// Here, that place is the line after `co_await call.Finish()`, which owns the answer swapped out of the
/// reactor.
///
/// Coroutine twin of whats_here.h, and clone of get_feature_pull_coro.h along the use only: the awaitable
/// is the same, and the diff is the card.
class WhatsHere {
  // The reactor flavour this scenario uses starts with its Delivery approach.
  using Delivery = routeguide::GetFeature::Delivery;
  using Call = RpcReactor::co::Unary<routeguide::GetFeature::ClientReactor<Delivery::kPull>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  WhatsHere(RpcReactor::co::Executor& executor, routeguide::RouteGuide::Stub& stub);

  WhatsHere(const WhatsHere&) = delete;
  WhatsHere& operator=(const WhatsHere&) = delete;
  WhatsHere(WhatsHere&&) = delete;
  WhatsHere& operator=(WhatsHere&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~WhatsHere() = default;

  /// Proxy component: spawns the coroutine, which sends the request and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param point tapped on the map
  void Start(routeguide::Point point);

 private:
  /// Servant component, as one function: sends the request, awaits the end of the RPC, and fills the card.
  /// @param point tapped on the map, taken by value since the coroutine outlives the call to Start()
  RpcReactor::co::Task<> Run(routeguide::Point point);

  RpcReactor::co::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("WhatsHere.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::co::RootTasks roots_;
};

}  // namespace rg_demo::usecase::coro
