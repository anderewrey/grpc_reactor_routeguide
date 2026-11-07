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
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: someone taps the map to ask what is at that point, and a card shows the answer: the
/// name of the place there, or that nothing named is there. One question gets one answer, read once,
/// where it arrives.
///
/// The variant chosen is ActiveUnaryReactor under Delivery::kPull. Either delivery would serve a single
/// answer. kPull fits here because the card is filled in the done handler and the answer is not needed
/// after that: GetResponse() swaps it out of the reactor, without a copy, and nothing has to own it
/// beyond the handler. Delivery::kPush would fit an answer handed to another component that keeps it.
///
/// Clone of get_feature_pull.h, along the use only: the reactor is the same, and the diff is the card.
class WhatsHere {
  // The reactor flavour this scenario uses starts with its Delivery approach.
  using Delivery = routeguide::GetFeature::Delivery;
  using Callbacks = routeguide::GetFeature::Callbacks<Delivery::kPull>;
  using ClientReactor = routeguide::GetFeature::ClientReactor<Delivery::kPull>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit WhatsHere(routeguide::RouteGuide::Stub& stub);

  WhatsHere(const WhatsHere&) = delete;
  WhatsHere& operator=(const WhatsHere&) = delete;
  WhatsHere(WhatsHere&&) = delete;
  WhatsHere& operator=(WhatsHere&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~WhatsHere();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param point tapped on the map
  void Start(const routeguide::Point& point);

 private:
  /// Application-thread handler for the OnDone event. The event data is the reactor, which still holds
  /// the response for GetResponse() to pull.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;
  spdlog::logger& logger_{VariantLogger("WhatsHere")};

  // Declared last, so destruction deregisters the handler before anything it reaches is gone. The
  // destructor first tears the reactor down, waiting for its OnDone() if the RPC is still in flight. The
  // event that OnDone() then queues carries only the reactor's pointer, so the queue nobody drains after
  // EventLoop::Halt() holds nothing owned, unlike under Delivery::kPush.
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
