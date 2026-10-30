///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <cstddef>
#include <memory>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: a route is recorded while driving, each point sent as the GPS produces it, and the
/// summary of the trip goes to the trip history once the drive ends. Points appear over time, on the
/// application's thread, and not at the pace of the network: they can come faster than a write
/// completes, such as the fixes a GPS receiver delivers together after a tunnel.
///
/// The variant chosen is ActiveWriteReactor under Delivery::kPush and Submission::kApplicationPushes.
/// kApplicationPushes, since a point may not exist yet when the previous write completes, and a
/// next_request callback cannot answer "not yet": the application sends each point when it has one,
/// and keeps the points that arrive while a write is in flight until its OnWriteDone. kPush, since the
/// summary is handed to another component, the trip history, which keeps it: the done callback hands
/// it out owned, and the history takes it without a copy. EventLoop timers stand in for the GPS.
///
/// Clone of record_route_push.h, along the use only: the reactor is the same, and the diff is the GPS,
/// the pump that follows it, and the trip history.
class LiveRouteRecording {
  // The reactor flavour this scenario exhibits starts with its Delivery approach.
  using Delivery = routeguide::RecordRoute::Delivery;
  using Submission = routeguide::RecordRoute::Submission;
  using Callbacks = routeguide::RecordRoute::Callbacks<Delivery::kPush, Submission::kApplicationPushes>;
  using ClientReactor = routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kApplicationPushes>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit LiveRouteRecording(routeguide::RouteGuide::Stub& stub);

  LiveRouteRecording(const LiveRouteRecording&) = delete;
  LiveRouteRecording& operator=(const LiveRouteRecording&) = delete;
  LiveRouteRecording(LiveRouteRecording&&) = delete;
  LiveRouteRecording& operator=(LiveRouteRecording&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~LiveRouteRecording();

  /// Proxy component: creates the Method Request, switches the GPS on, then returns. The first point is
  /// sent once the GPS produces it.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param drive the points the GPS will produce, one fix each, in order
  void Start(std::vector<routeguide::Point> drive);

 private:
  /// Application-thread handler for a GPS fix: queues the point, and sends it at once when no write is
  /// in flight.
  void OnGpsFix(const EventLoop::Event* event);
  /// Application-thread handler for OnWriteDone, which sends the next queued point, if any.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which hands the summary to the trip history and destroys
  /// the reactor.
  void OnDone(const EventLoop::Event* event);

  /// Sends the next queued point, unless a write is in flight or no point waits. The last point of the
  /// drive goes through SendLastRequest(), which closes the stream in the same operation.
  void SendNextPoint();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;
  Activity activity_;
  // All of these belong to the application thread. The GPS takes its next fix from the drive, the queue
  // keeps the points waiting for a write, and the history keeps the summaries of past drives.
  std::vector<routeguide::Point> drive_;
  std::vector<routeguide::Point> pending_;
  std::size_t fixes_{0};
  bool write_in_flight_{false};
  std::vector<std::unique_ptr<routeguide::RecordRoute::ResponseT>> trip_history_;
  spdlog::logger& logger_{VariantLogger("LiveRouteRecording")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_gps_fix_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
