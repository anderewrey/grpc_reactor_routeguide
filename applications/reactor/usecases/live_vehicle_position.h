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

#include "applications/reactor/latest_slot.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: a dashboard draws a vehicle's position on a map, from a live feed of position fixes. A
/// fix is worth drawing only until a newer one exists, and the map takes longer to redraw than the feed
/// takes to send the next fix, so the map must skip to the newest fix rather than fall behind.
///
/// The variant chosen is ActiveReadReactor under Delivery::kPull and ReadPacing::kContinuous, with a
/// LatestSlot on the application side. The feed never waits for the map: read_ok copies each lent fix into
/// the slot, replacing one the map has not drawn yet, and asks for an event only when the slot was empty.
/// The application-thread handler draws whatever fix is newest, so fixes arriving during a redraw replace
/// each other in the slot. ListFeatures stands in for the feed: its features are read as successive fixes.
///
/// Clone of list_features_pull_continuous.h, along the use only: the reactor is the same, and the diff is
/// what an application adds to keep the newest message and drop the stale ones. The reactor drops nothing,
/// so dropping is a decision of the application, taken in its own code.
class LiveVehiclePosition {
  // The reactor flavour this scenario uses: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull, ReadPacing::kContinuous>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit LiveVehiclePosition(routeguide::RouteGuide::Stub& stub);

  LiveVehiclePosition(const LiveVehiclePosition&) = delete;
  LiveVehiclePosition& operator=(const LiveVehiclePosition&) = delete;
  LiveVehiclePosition(LiveVehiclePosition&&) = delete;
  LiveVehiclePosition& operator=(LiveVehiclePosition&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~LiveVehiclePosition();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(const routeguide::Rectangle& rect);

 private:
  /// One position fix: a message copied out of the reactor, with its number in the feed.
  struct Fix {
    unsigned seq;
    routeguide::Feature feature;
  };

  /// Application-thread handler for the slot's notification: draws the newest fix, if any is left.
  /// Delta against list_features_pull_continuous.h: the application handles messages, but only the newest.
  void OnLatestFix(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which draws the fix still in the slot, if any, reports the
  /// totals and destroys the reactor.
  void OnDone(const EventLoop::Event* event);
  /// Redraws the vehicle on the map at one fix, which takes a while.
  void Draw(const Fix& fix);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // Written by read_ok on the gRPC thread, and taken by the handlers on the application thread.
  RpcReactor::LatestSlot<Fix> latest_;
  // Incremented only on the gRPC thread, in read_ok, but read by the OnDone handler.
  std::atomic<unsigned> read_seq_{0};
  // Application thread only.
  unsigned drawn_{0};
  unsigned last_drawn_seq_{0};

  spdlog::logger& logger_{VariantLogger("LiveVehiclePosition")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_latest_fix_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
