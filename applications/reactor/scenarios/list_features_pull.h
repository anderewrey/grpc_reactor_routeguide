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

namespace rg_demo::pull {

/// Exhibits ActiveReadReactor under Delivery::kPull, via the ListFeatures RPC. Each message stays in
/// the reactor: the read_ok callback keeps it by returning true, which holds the RPC and arms nothing
/// until the application pulls the message with GetResponse(). That call also arms the next read, so
/// the stream is bounded to one message in flight, as under ReadPacing::kTurnByTurn.
///
/// Clone of its turn-by-turn counterpart in list_features_turn_by_turn.h, deliberately, so each
/// flavour reads whole: `diff list_features_pull.cpp list_features_turn_by_turn.cpp` is the shortest
/// description of what the delivery approach changes when the pacing is the same. The clone is not
/// free: a bug found in one must be fixed in the others.
class ListFeatures {
  // The reactor flavour this scenario exhibits starts with its Delivery approach.
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

  ~ListFeatures() = default;

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Application-thread handler for OnReadDoneOk.
  /// Delta against the turn-by-turn counterpart: the message is still in the reactor, and this handler
  /// pulls it with GetResponse(), which also arms the next read. No read is armed between the
  /// gRPC-thread callback and that call, so read_ok #k+1 cannot appear before the pulled #k line.
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

  spdlog::logger& logger_{VariantLogger("ListFeatures.pull")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull
