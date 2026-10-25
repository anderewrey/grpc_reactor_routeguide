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
#include "applications/reactor/demo_support.h"

namespace rg_demo::pull::turn_by_turn {

/// Exhibits ActiveBidiReactor under Delivery::kPull and ReadPacing::kTurnByTurn, via the RouteChat RPC.
/// Each message stays in the reactor, and the reactor holds the read side and arms nothing until the
/// application pulls the message with GetResponse(). The write side keeps running meanwhile, since a
/// read hold stalls only the reads.
///
/// The notes are sent one at a time, under the same one-write-in-flight constraint as RecordRoute,
/// while responses arrive on the independent read side at any time. The write pump is identical in
/// every RouteChat scenario, so it is the constant against which each read-side cell is shown.
///
/// Clone of its siblings, for the reason recorded on the ListFeatures scenarios: one class instantiated
/// twice would register the same event names twice. Each sibling differs from this file along one axis:
/// - route_chat_pull_continuous.h, the pacing: the reactor re-arms the read side by itself
/// - route_chat_pull_per_message.h, the pacing: read_ok decides per message whether to hold
/// - route_chat_push_turn_by_turn.h, the delivery: the message is pushed, then ResumeRead() re-arms
class RouteChat {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::RouteChat::Delivery;
  using Submission = routeguide::RouteChat::Submission;
  using ReadPacing = routeguide::RouteChat::ReadPacing;
  using Callbacks =
      routeguide::RouteChat::Callbacks<Delivery::kPull, Submission::kApplicationPushes, ReadPacing::kTurnByTurn>;
  using ClientReactor =
      routeguide::RouteChat::ClientReactor<Delivery::kPull, Submission::kApplicationPushes, ReadPacing::kTurnByTurn>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit RouteChat(routeguide::RouteGuide::Stub& stub);

  RouteChat(const RouteChat&) = delete;
  RouteChat& operator=(const RouteChat&) = delete;
  RouteChat(RouteChat&&) = delete;
  RouteChat& operator=(RouteChat&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~RouteChat();

  /// Proxy component: creates the Method Request, fires the first write, then returns.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param notes to stream to the server, consumed one per OnWriteDone
  void Start(std::vector<routeguide::RouteNote> notes);

 private:
  /// Application-thread handler for OnReadDoneOk.
  /// Delta against the push counterpart: the message is still in the reactor, and this handler pulls
  /// it with GetResponse(), which also re-arms the read side. The write side is unaffected by that hold.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnWriteDone, which paces the next write.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  /// Sends the next queued note. The last note goes through SendLastRequest(), which closes the
  /// client's write side; the server may keep sending responses afterward.
  void SendNextNote();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The queue belongs to this object. Its siblings run concurrently and consume their own notes, so
  // one shared queue would let any of them drain another's.
  std::vector<routeguide::RouteNote> pending_;

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  spdlog::logger& logger_{VariantLogger("RouteChat.pull.turn")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull::turn_by_turn
