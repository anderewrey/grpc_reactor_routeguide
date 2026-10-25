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
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/scenarios/support.h"

namespace rg_demo::push::per_message {

/// Exhibits ActiveBidiReactor under Delivery::kPush and ReadPacing::kPerMessage, via the RouteChat RPC.
/// Each message is handed out as an owned object, and the read_ok callback decides for each one whether
/// the reactor holds the read side until ResumeRead(), or reads on at once. This scenario uses that
/// decision for adaptive backpressure, as its ListFeatures namesake does: the read side runs freely
/// while few notes wait for the application, and is held once a small bound is reached, until the
/// application has drained them. The write side keeps running meanwhile, since a read hold stalls only
/// the reads.
///
/// The notes are sent one at a time, under the same one-write-in-flight constraint as RecordRoute,
/// while responses arrive on the independent read side at any time. The write pump is identical in
/// every RouteChat scenario, so it is the constant against which each read-side cell is shown.
///
/// Clone of its siblings, for the reason recorded on the ListFeatures scenarios: one class instantiated
/// twice would register the same event names twice. Each sibling differs from this file along one axis:
/// - route_chat_push_turn_by_turn.h, the pacing: every message is held until ResumeRead()
/// - route_chat_pull_per_message.h, the delivery: the message is pulled with GetResponse(), and the
///   per-message decision serves a filter there rather than backpressure
class RouteChat {
  // The reactor flavour this scenario exhibits: its Delivery approach only. Its ReadPacing is the
  // default, ReadPacing::kPerMessage, which its fixed-pacing siblings name explicitly instead.
  using Delivery = routeguide::RouteChat::Delivery;
  using Callbacks = routeguide::RouteChat::Callbacks<Delivery::kPush>;
  using ClientReactor = routeguide::RouteChat::ClientReactor<Delivery::kPush>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit RouteChat(routeguide::RouteGuide::Stub& stub);

  RouteChat(const RouteChat&) = delete;
  RouteChat& operator=(const RouteChat&) = delete;
  RouteChat(RouteChat&&) = delete;
  RouteChat& operator=(RouteChat&&) = delete;

  ~RouteChat() = default;

  /// Proxy component: creates the Method Request, fires the first write, then returns.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param notes to stream to the server, consumed one per OnWriteDone
  void Start(std::vector<routeguide::RouteNote> notes);

 private:
  /// Application-thread handler for OnReadDoneOk.
  /// Delta against the turn-by-turn counterpart: this handler frees the note's in-flight slot, and ends
  /// with ResumeRead() only for a note read_ok held. The write side is unaffected by that hold.
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
  // only on the application thread, so neither is shared. They stay in step because the event queue
  // is FIFO and a read is armed again only once read_ok has queued its note.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  // Notes read but not yet handled: incremented by read_ok on the gRPC thread, decremented by the
  // handler on the application thread, so it is shared, and atomic rather than locked.
  std::atomic<unsigned> in_flight_{0};
  // The sequence number of the note read_ok held, or 0 when none is: set on the gRPC thread, and
  // claimed by that note's own handler on the application thread.
  std::atomic<unsigned> held_seq_{0};

  spdlog::logger& logger_{VariantLogger("RouteChat.push.msg")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::push::per_message
