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

namespace rg_demo::push::continuous {

/// Exhibits ActiveReadReactor under Delivery::kPush and ReadPacing::kContinuous, via the ListFeatures
/// RPC. Each message is handed out as an owned object, and the reactor arms each following read itself,
/// as the last statement of its own reaction, so the stream never waits for the application.
///
/// Clone of its siblings, deliberately, so each cell reads whole. The clone is not free: a bug found in
/// one must be fixed in the others. It is kept because one class instantiated twice would register the
/// same event names twice, and EventLoop appends handlers per name while a read event carries an owned
/// message. Each sibling differs from this file along one axis only:
/// - list_features_push_turn_by_turn.h, the pacing: the application re-arms with ResumeRead()
/// - list_features_pull_continuous.h, the delivery: the message is only lent, so it is consumed in the
///   callback rather than queued
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush, ReadPacing::kContinuous>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit ListFeatures(routeguide::RouteGuide::Stub& stub);

  ListFeatures(const ListFeatures&) = delete;
  ListFeatures& operator=(const ListFeatures&) = delete;
  ListFeatures(ListFeatures&&) = delete;
  ListFeatures& operator=(ListFeatures&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~ListFeatures();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Application-thread handler for OnReadDoneOk. Logs the message a second time, under the
  /// sequence number its gRPC-thread read_ok line already carried, so the lines pair up across
  /// threads.
  void OnReadOk(const EventLoop::Event* event);
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

  spdlog::logger& logger_{VariantLogger("ListFeatures.push.cont")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::push::continuous
