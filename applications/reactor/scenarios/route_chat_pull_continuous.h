///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/scenarios/support.h"

namespace rg_demo::pull::continuous {

/// Exhibits ActiveBidiReactor under Delivery::kPull and ReadPacing::kContinuous, via the RouteChat RPC.
/// Each message is only lent to the read_ok callback, and the reactor re-arms the read side into the
/// same storage as soon as the callback returns, so the message never reaches the application. The
/// callback consumes each message inside the reaction instead, into an aggregate the application reads
/// once the RPC is done.
///
/// The notes are sent one at a time, under the same one-write-in-flight constraint as RecordRoute,
/// while responses arrive on the independent read side at any time. The write pump is identical in
/// every RouteChat scenario, so it is the constant against which each read-side cell is shown.
///
/// Clone of its siblings, for the reason recorded on the ListFeatures scenarios: one class instantiated
/// twice would register the same event names twice. Each sibling differs from this file along one axis:
/// - route_chat_pull_turn_by_turn.h, the pacing: the message is held until GetResponse() pulls it
/// - route_chat_push_continuous.h, the delivery: the message is owned, so it can be queued whole
class RouteChat {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::RouteChat::Delivery;
  using Submission = routeguide::RouteChat::Submission;
  using ReadPacing = routeguide::RouteChat::ReadPacing;
  using Callbacks =
      routeguide::RouteChat::Callbacks<Delivery::kPull, Submission::kApplicationPushes, ReadPacing::kContinuous>;
  using ClientReactor =
      routeguide::RouteChat::ClientReactor<Delivery::kPull, Submission::kApplicationPushes, ReadPacing::kContinuous>;

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
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnWriteDone, which paces the next write.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the aggregate and destroys the reactor.
  /// Delta against the turn-by-turn counterpart: there is no handler for OnReadDoneOk, since no
  /// message is queued.
  void OnDone(const EventLoop::Event* event);

  /// Sends the next queued note. The last note goes through SendLastRequest(), which closes the
  /// client's write side; the server may keep sending responses afterward.
  void SendNextNote();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The queue belongs to this object. Its siblings run concurrently and consume their own notes, so
  // one shared queue would let any of them drain another's.
  std::vector<routeguide::RouteNote> pending_;

  // The aggregate. Incremented only on the gRPC thread, in read_ok, but read by the OnDone handler on
  // the application thread, so both are shared, and atomic rather than locked: the reaction must never
  // wait on the application.
  std::atomic<unsigned> read_seq_{0};
  std::atomic<std::size_t> message_chars_{0};

  spdlog::logger& logger_{VariantLogger("RouteChat.pull.cont")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull::continuous
