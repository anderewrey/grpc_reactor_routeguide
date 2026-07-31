///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <memory>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/scenarios/support.h"

namespace rg_demo {

/// Exhibits ActiveWriteReactor, via the RecordRoute RPC. The write reactor has one variant: there is
/// no read stream to pace, and its single terminal response arrives with OnDone, so this scenario
/// sits at the parent namespace instead of under a flavour.
///
/// The points are sent one at a time, waiting for each OnWriteDone before sending the next, since
/// gRPC allows only one write in flight per stream. The pending queue belongs to this object, so a
/// concurrent write-side scenario cannot drain it.
class RecordRoute {
 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit RecordRoute(routeguide::RouteGuide::Stub& stub);

  RecordRoute(const RecordRoute&) = delete;
  RecordRoute& operator=(const RecordRoute&) = delete;
  RecordRoute(RecordRoute&&) = delete;
  RecordRoute& operator=(RecordRoute&&) = delete;

  ~RecordRoute() = default;

  /// Proxy component: creates the Method Request, fires the first write, then returns.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param points to stream to the server, consumed one per OnWriteDone
  void Start(std::vector<routeguide::Point> points);

 private:
  /// Application-thread handler for OnWriteDone, which paces the next write.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  /// Sends the next queued point. Called once to fire the first write after creating the reactor,
  /// and again from the OnWriteDone handler until the queue is exhausted. The last point goes
  /// through SendLastRequest(), which closes the stream in the same operation.
  void SendNextPoint();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<routeguide::RecordRoute::ClientReactor> reactor_;
  std::vector<routeguide::Point> pending_;
  spdlog::logger& logger_{routeguide::logger::Get(routeguide::RpcMethods::kRecordRoute)};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo
