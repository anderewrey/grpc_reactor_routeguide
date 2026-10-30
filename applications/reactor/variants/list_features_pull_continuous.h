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

namespace rg_demo::pull::continuous {

/// Exhibits ActiveReadReactor under Delivery::kPull and ReadPacing::kContinuous, via the ListFeatures
/// RPC. Each message is only lent to the read_ok callback, and the reactor re-arms the next read into
/// the same storage as soon as the callback returns, so the stream never waits for the application and
/// the message never reaches it. The callback consumes each message inside the reaction instead, here by
/// logging it and counting it, and the application reads the count once the RPC is done. The
/// unnamed_places_audit use case consumes it into an aggregate.
///
/// Clone of its siblings, deliberately, so each cell reads whole. The clone is not free: a bug found in
/// one must be fixed in the others. It is kept because one class instantiated twice would register the
/// same event names twice, and EventLoop appends handlers per name while a read event carries an owned
/// message. Each sibling differs from this file along one axis only:
/// - list_features_pull_turn_by_turn.h, the pacing: the message is held until GetResponse() pulls it
/// - list_features_push_continuous.h, the delivery: the message is owned, so it can be queued whole
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull, ReadPacing::kContinuous>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>;

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
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the count and destroys the reactor.
  /// Delta against the turn-by-turn counterpart: there is no handler for OnReadDoneOk, since no
  /// message is queued.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;
  Activity activity_;

  // Incremented only on the gRPC thread, in read_ok, but read by the OnDone handler on the application
  // thread, so it is shared, and atomic rather than locked: the reaction must never wait on the
  // application.
  std::atomic<unsigned> read_seq_{0};

  spdlog::logger& logger_{VariantLogger("ListFeatures.pull.cont")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull::continuous
