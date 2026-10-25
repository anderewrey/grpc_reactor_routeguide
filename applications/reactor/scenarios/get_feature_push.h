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

namespace rg_demo::push {

/// Exhibits ActiveUnaryReactor under Delivery::kPush, via the GetFeature RPC. The done callback is
/// given the response as an owned object, and releases it into the event queue. The unary reactor has
/// no stream to pace, so its only flavour axis is the delivery approach.
///
/// Clone of its pull counterpart in get_feature_pull.h, deliberately, so each flavour reads whole:
/// `diff get_feature_pull.cpp get_feature_push.cpp` is the shortest description of what the delivery
/// approach changes for a single terminal response.
///
/// The scenario owns everything its RPC needs on the application side: the reactor slot, the event
/// names, the application-thread handlers, and the logger.
class GetFeature {
  // The reactor flavour this scenario exhibits starts with its Delivery approach.
  using Delivery = routeguide::GetFeature::Delivery;
  using Callbacks = routeguide::GetFeature::Callbacks<Delivery::kPush>;
  using ClientReactor = routeguide::GetFeature::ClientReactor<Delivery::kPush>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit GetFeature(routeguide::RouteGuide::Stub& stub);

  GetFeature(const GetFeature&) = delete;
  GetFeature& operator=(const GetFeature&) = delete;
  GetFeature(GetFeature&&) = delete;
  GetFeature& operator=(GetFeature&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~GetFeature();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param point to look up
  void Start(routeguide::Point point);

 private:
  /// Application-thread handler for the OnDone event. The event data is the response, so the
  /// reactor is reached through its own slot rather than through the event.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;
  spdlog::logger& logger_{VariantLogger("GetFeature.push")};

  // Declared last, so destruction deregisters the handler before anything it reaches is gone. The
  // destructor first tears the reactor down, waiting for its OnDone() if the RPC is still in flight. That
  // OnDone() still pushes an owned response into a queue nobody drains after EventLoop::Halt(), which
  // discards rather than drains (see "Message ownership" in reactor_client.md), so that response leaks.
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::push
