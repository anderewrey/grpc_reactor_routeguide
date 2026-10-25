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
#include "applications/reactor/scenarios/support.h"

namespace rg_demo::pull::per_message {

/// Exhibits ActiveReadReactor under Delivery::kPull and ReadPacing::kPerMessage, via the ListFeatures
/// RPC. Each message stays in the reactor, and the read_ok callback decides for each one whether the
/// reactor holds the RPC until the application pulls it with GetResponse(), or reads on at once. This
/// scenario uses that decision as a filter: named features are kept and pulled, unnamed ones are
/// declined inside the callback and never reach the application thread.
///
/// Clone of its siblings, deliberately, so each cell reads whole. The clone is not free: a bug found in
/// one must be fixed in the others. It is kept because one class instantiated twice would register the
/// same event names twice, and EventLoop appends handlers per name while a read event carries an owned
/// message. Each sibling differs from this file along one axis only:
/// - list_features_pull_turn_by_turn.h, the pacing: every message is held until GetResponse()
/// - list_features_push_per_message.h, the delivery: the message is pushed, and the per-message
///   decision serves backpressure there rather than a filter
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach only. Its ReadPacing is the
  // default, ReadPacing::kPerMessage, which its fixed-pacing siblings name explicitly instead.
  using Delivery = routeguide::ListFeatures::Delivery;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull>;

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
  /// Application-thread handler for OnReadDoneOk.
  /// Same as the turn-by-turn counterpart: only kept messages reach it, each one still in the reactor,
  /// and it pulls them with GetResponse(), which also arms the next read. No read is armed between a
  /// keeping callback and that call, so read_ok #k+1 cannot appear before the pulled #k line.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the filter's totals and destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared. They stay in step because the event queue
  // is FIFO and at most one OnReadDone reaction per RPC is in flight. Both count kept messages only.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  // Declined messages: incremented by read_ok on the gRPC thread, read by the OnDone handler on the
  // application thread, so it is shared, and atomic rather than locked.
  std::atomic<unsigned> skipped_seq_{0};

  spdlog::logger& logger_{VariantLogger("ListFeatures.pull.msg")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull::per_message
