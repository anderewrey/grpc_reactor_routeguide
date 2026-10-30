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

namespace rg_demo::pull::turn_by_turn {

/// Exhibits ActiveReadReactor under Delivery::kPull and ReadPacing::kTurnByTurn, via the ListFeatures
/// RPC. Each message stays in the reactor, and the reactor holds the RPC and arms nothing until the
/// application pulls the message with GetResponse(). That call also arms the next read, so the stream
/// is bounded to one message in flight.
///
/// Clone of its siblings, deliberately, so each cell reads whole. The clone is not free: a bug found in
/// one must be fixed in the others. It is kept because one class instantiated twice would register the
/// same event names twice, and EventLoop appends handlers per name while a read event carries an owned
/// message. Each sibling differs from this file along one axis only:
/// - list_features_pull_continuous.h, the pacing: the reactor re-arms by itself, nothing to pull
/// - list_features_pull_per_message.h, the pacing: read_ok decides per message whether to hold
/// - list_features_push_turn_by_turn.h, the delivery: the message is pushed, then ResumeRead() re-arms
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull, ReadPacing::kTurnByTurn>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kTurnByTurn>;

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
  void Start(const routeguide::Rectangle& rect);

 private:
  /// Application-thread handler for OnReadDoneOk.
  /// Delta against the push counterpart: the message is still in the reactor, and this handler pulls
  /// it with GetResponse(), which also arms the next read. No read is armed between the gRPC-thread
  /// callback and that call, so read_ok #k+1 cannot appear before the pulled #k line.
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

  spdlog::logger& logger_{VariantLogger("ListFeatures.pull.turn")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull::turn_by_turn
