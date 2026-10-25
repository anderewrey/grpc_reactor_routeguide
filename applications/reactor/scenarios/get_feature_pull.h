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

namespace rg_demo::pull {

/// Exhibits ActiveUnaryReactor under Delivery::kPull, via the GetFeature RPC. The response stays in the
/// reactor: the done callback passes the reactor pointer into the event queue, and the
/// application-thread handler pulls the response out through GetResponse(). The unary reactor has no
/// stream to pace, so its only flavour axis is the delivery approach.
///
/// Clone of its push counterpart in get_feature_push.h, deliberately, so each flavour reads whole:
/// `diff get_feature_pull.cpp get_feature_push.cpp` is the shortest description of what the delivery
/// approach changes for a single terminal response.
///
/// The scenario owns everything its RPC needs on the application side: the reactor slot, the event
/// names, the application-thread handlers, and the logger.
class GetFeature {
  // The reactor flavour this scenario exhibits starts with its Delivery approach.
  using Delivery = routeguide::GetFeature::Delivery;
  using Callbacks = routeguide::GetFeature::Callbacks<Delivery::kPull>;
  using ClientReactor = routeguide::GetFeature::ClientReactor<Delivery::kPull>;

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
  /// Application-thread handler for the OnDone event. The event data is the reactor, which still holds
  /// the response for GetResponse() to pull.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;
  spdlog::logger& logger_{VariantLogger("GetFeature.pull")};

  // Declared last, so destruction deregisters the handler before anything it reaches is gone. The
  // teardown limitation recorded in deferred-work.md does not apply here: a reactor cancelled during
  // teardown queues its own pointer, not an owned response, so nothing can leak.
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull
