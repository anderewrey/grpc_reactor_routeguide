///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <memory>
#include <optional>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::pull::producer {

/// Exhibits ActiveWriteReactor under Delivery::kPull and Submission::kReactorPulls, via the RecordRoute
/// RPC. The reactor pulls each point from the next_request callback, once while it starts the RPC and
/// again after each write completes, and closes the request stream itself once the queue is empty. The
/// application makes no write call, and the reactor takes no hold.
///
/// Clone of record_route_pull.h, which differs from it along the outgoing axis only: there, the
/// application pushes each point from its OnWriteDone handler. The OnWriteDone event remains, for the
/// log, but paces nothing.
class RecordRoute {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its Submission.
  using Delivery = routeguide::RecordRoute::Delivery;
  using Submission = routeguide::RecordRoute::Submission;
  using Callbacks = routeguide::RecordRoute::Callbacks<Delivery::kPull, Submission::kReactorPulls>;
  using ClientReactor = routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit RecordRoute(routeguide::RouteGuide::Stub& stub);

  RecordRoute(const RecordRoute&) = delete;
  RecordRoute& operator=(const RecordRoute&) = delete;
  RecordRoute(RecordRoute&&) = delete;
  RecordRoute& operator=(RecordRoute&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~RecordRoute();

  /// Proxy component: creates the Method Request, which pulls the first point before it returns.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param points to stream to the server, pulled one at a time by the reactor
  void Start(std::vector<routeguide::Point> points);

 private:
  /// Application-thread handler for OnWriteDone, which only logs: the reactor pulls the next point.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  /// The next_request callback's body: the next queued point, or std::nullopt once the queue is empty.
  std::optional<routeguide::Point> NextPoint();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The queue belongs to this object, and to the reactor while the RPC runs: Start() fills it before
  // creating the reactor, and only next_request reads it afterward. The reactor pulls once at a time,
  // first on the application thread, then on gRPC threads, each pull after the write before it
  // completed, so the pulls never overlap.
  std::vector<routeguide::Point> pending_;
  unsigned pulled_seq_{0};

  spdlog::logger& logger_{VariantLogger("RecordRoute.pull.producer")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull::producer
