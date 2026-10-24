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

namespace rg_demo::push::per_message {

/// Exhibits ActiveReadReactor under Delivery::kPush and ReadPacing::kPerMessage, via the ListFeatures
/// RPC. Each message is handed out as an owned object, and the read_ok callback decides for each one
/// whether the reactor holds the RPC until ResumeRead(), or reads on at once. This scenario uses that
/// decision for adaptive backpressure: the stream runs freely while few messages wait for the
/// application, and is held once a small bound is reached, until the application has drained them.
///
/// Clone of its siblings, deliberately, so each cell reads whole. The clone is not free: a bug found in
/// one must be fixed in the others. It is kept because one class instantiated twice would register the
/// same event names twice, and EventLoop appends handlers per name while a read event carries an owned
/// message. Each sibling differs from this file along one axis only:
/// - list_features_push_turn_by_turn.h, the pacing: every message is held until ResumeRead()
/// - list_features_pull_per_message.h, the delivery: the message is pulled with GetResponse(), and the
///   per-message decision serves a filter there rather than backpressure
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush, ReadPacing::kPerMessage>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kPerMessage>;

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
  /// Delta against the turn-by-turn counterpart: this handler frees the message's in-flight slot, and
  /// ends with ResumeRead() only for a message read_ok held. No read is armed between a holding
  /// callback and that call, so read_ok #k+1 cannot appear before the resumed #k line.
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

  // Messages read but not yet handled: incremented by read_ok on the gRPC thread, decremented by the
  // handler on the application thread, so it is shared, and atomic rather than locked.
  std::atomic<unsigned> in_flight_{0};
  // The sequence number of the message read_ok held, or 0 when none is: set on the gRPC thread, and
  // claimed by that message's own handler on the application thread.
  std::atomic<unsigned> held_seq_{0};

  spdlog::logger& logger_{VariantLogger("ListFeatures.push.msg")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::push::per_message
