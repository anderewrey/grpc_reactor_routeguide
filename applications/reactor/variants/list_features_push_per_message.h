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

namespace rg_demo::push::per_message {

/// Exhibits ActiveReadReactor under Delivery::kPush and ReadPacing::kPerMessage, via the ListFeatures
/// RPC. Each message is handed out as an owned object, and the read_ok callback decides for each one
/// whether the reactor holds the RPC until ResumeRead(), or reads on at once. The decision here follows
/// an arbitrary rule, holding every second message, which stands for whatever an application decides
/// from the message or from its own state. Every message reaches the application thread either way. The
/// offline_map_download use case decides it for a reason.
///
/// Clone of its siblings, deliberately, so each cell reads whole. The clone is not free: a bug found in
/// one must be fixed in the others. It is kept because one class instantiated twice would register the
/// same event names twice, and EventLoop appends handlers per name while a read event carries an owned
/// message. Each sibling differs from this file along one axis only:
/// - list_features_push_turn_by_turn.h, the pacing: every message is held until ResumeRead()
/// - list_features_pull_per_message.h, the delivery: the message is pulled with GetResponse(), and the
///   same rule decides which messages are kept, the others being declined
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach only. Its ReadPacing is the
  // default, ReadPacing::kPerMessage, which its fixed-pacing siblings name explicitly instead.
  using Delivery = routeguide::ListFeatures::Delivery;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush>;

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
  /// Delta against the turn-by-turn counterpart: this handler ends with ResumeRead() only for a message
  /// read_ok held. No read is armed between a holding
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

  // The arbitrary rule: true when read_ok holds the next message. Only read_ok reads and flips it, before
  // its hand-off, and the next read_ok can only start after a read armed later, so the reactions never
  // share it at once.
  bool hold_next_{false};
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
