///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <atomic>
#include <memory>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: a guidance screen shows the stops of a route one at a time. The next stop is fetched only
/// once the driver confirms the current one, so the screen never runs ahead of the driver, however fast
/// the server could send the route.
///
/// The variant chosen is ActiveReadReactor under Delivery::kPush and ReadPacing::kTurnByTurn. Exactly one
/// stop is in flight: the reactor holds the stream after each one, and only the application releases it.
/// Delivery::kPush is required, not a preference. The stop is handed out owned, so the screen keeps it for
/// as long as the driver takes, and the driver's confirmation, a later event, calls ResumeRead(). Under
/// Delivery::kPull, GetResponse() both takes the stop out of the reactor and arms the next read, so
/// showing a stop would already fetch the next one: the stream could not be held until the confirmation.
/// EventLoop timers stand in for the driver. ListFeatures stands in for the route: its features are read
/// as successive stops.
///
/// Clone of list_features_push_turn_by_turn.h, along the use only: the reactor is the same, and the diff
/// is the screen that keeps the stop, and the confirmation that moves ResumeRead() to a later event.
class TurnByTurnGuidance {
  // The reactor flavour this scenario uses: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush, ReadPacing::kTurnByTurn>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kTurnByTurn>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit TurnByTurnGuidance(routeguide::RouteGuide::Stub& stub);

  TurnByTurnGuidance(const TurnByTurnGuidance&) = delete;
  TurnByTurnGuidance& operator=(const TurnByTurnGuidance&) = delete;
  TurnByTurnGuidance(TurnByTurnGuidance&&) = delete;
  TurnByTurnGuidance& operator=(TurnByTurnGuidance&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~TurnByTurnGuidance();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Application-thread handler for OnReadDoneOk, which shows the stop and asks the driver to confirm it.
  /// Delta against list_features_push_turn_by_turn.h: the re-arm moves to OnDriverConfirmed().
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for the driver's confirmation, which clears the screen and ends with
  /// ResumeRead(). No read is armed between the gRPC-thread callback and this call, so read_ok #k+1
  /// cannot appear before the resumed #k line.
  void OnDriverConfirmed(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared. They stay in step because the event queue
  // is FIFO and at most one OnReadDone reaction per RPC is in flight.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  // The stop on the screen, owned until the driver confirms it. Application thread only.
  std::unique_ptr<routeguide::ListFeatures::ResponseT> current_stop_;
  // Stops confirmed: incremented on the application thread before each ResumeRead(), and read by
  // read_ok on the gRPC thread, which checks that the stream never runs ahead of the driver.
  std::atomic<unsigned> confirmed_{0};

  spdlog::logger& logger_{VariantLogger("TurnByTurnGuidance")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_driver_confirmed_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
