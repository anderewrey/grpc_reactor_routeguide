///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <memory>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/scenarios/support.h"

namespace rg_demo {

/// Exhibits ActiveUnaryReactor, via the GetFeature RPC. The unary reactor is the one type with no
/// variants and none possible: it delivers a single terminal response and has no stream to pace,
/// which is why this scenario sits at the parent namespace instead of under a flavour.
///
/// The scenario owns everything its RPC needs on the application side: the reactor slot, the event
/// names, the application-thread handlers, and the logger.
class GetFeature {
 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit GetFeature(routeguide::RouteGuide::Stub& stub);

  GetFeature(const GetFeature&) = delete;
  GetFeature& operator=(const GetFeature&) = delete;
  GetFeature(GetFeature&&) = delete;
  GetFeature& operator=(GetFeature&&) = delete;

  ~GetFeature() = default;

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param point to look up
  void Start(routeguide::Point point);

 private:
  /// Application-thread handler for the OnDone event. The event data is the response, so the
  /// reactor is reached through its own slot rather than through the event.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<routeguide::GetFeature::ClientReactor> reactor_;
  spdlog::logger& logger_{routeguide::logger::Get(routeguide::RpcMethods::kGetFeature)};

  // Declared last, so destruction deregisters the handler before anything it reaches is gone. The
  // teardown limitation recorded in deferred-work.md is unchanged by that ordering: a reactor
  // cancelled during teardown still pushes a response into a queue nobody drains.
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo
