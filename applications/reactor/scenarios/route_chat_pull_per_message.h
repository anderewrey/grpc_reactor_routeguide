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
#include <set>
#include <string>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/scenarios/support.h"

namespace rg_demo::pull::per_message {

/// Exhibits ActiveBidiReactor under Delivery::kPull and ReadPacing::kPerMessage, via the RouteChat RPC.
/// Each message stays in the reactor, and the read_ok callback decides for each one whether the reactor
/// holds the read side until GetResponse(), or reads on at once. This scenario uses that decision as a
/// filter: the reference server echoes the whole note history of a location each time a note arrives
/// there, so the same note comes back again and again. read_ok keeps a note the first time it is seen,
/// to be pulled, and declines the repeats. The write side keeps running meanwhile, since a read hold
/// stalls only the reads.
///
/// The notes are sent one at a time, under the same one-write-in-flight constraint as RecordRoute,
/// while responses arrive on the independent read side at any time. The write pump is identical in
/// every RouteChat scenario, so it is the constant against which each read-side cell is shown.
///
/// Clone of its siblings, for the reason recorded on the ListFeatures scenarios: one class instantiated
/// twice would register the same event names twice. Each sibling differs from this file along one axis:
/// - route_chat_pull_turn_by_turn.h, the pacing: every message is held until GetResponse()
/// - route_chat_push_per_message.h, the delivery: the message is pushed, and the per-message decision
///   serves backpressure there rather than a filter
class RouteChat {
  // The reactor flavour this scenario exhibits: its Delivery approach only. Its ReadPacing is the
  // default, ReadPacing::kPerMessage, which its fixed-pacing siblings name explicitly instead.
  using Delivery = routeguide::RouteChat::Delivery;
  using Callbacks = routeguide::RouteChat::Callbacks<Delivery::kPull>;
  using ClientReactor = routeguide::RouteChat::ClientReactor<Delivery::kPull>;

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
  /// Runs for kept notes only. The message is still in the reactor, and this handler pulls it with
  /// GetResponse(), which also re-arms the read side. The write side is unaffected by that hold.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnWriteDone, which paces the next write.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the filter's totals and destroys the reactor.
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
  // only on the application thread, so neither is shared. Both count kept notes only.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  // The notes seen so far, keyed by their text form. Only read_ok touches it, before its hand-off, and
  // the next read_ok can only start after a read armed later, so the reactions never share it at once.
  std::set<std::string> seen_;
  // Declined notes: counted by read_ok on the gRPC thread, and read by OnDone on the application
  // thread, so it is shared, and atomic rather than locked.
  std::atomic<unsigned> skipped_seq_{0};

  spdlog::logger& logger_{VariantLogger("RouteChat.pull.msg")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull::per_message
