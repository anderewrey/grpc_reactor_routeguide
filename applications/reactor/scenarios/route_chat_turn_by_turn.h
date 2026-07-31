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
#include "applications/reactor/scenarios/support.h"

namespace rg_demo::turn_by_turn {

/// Exhibits ActiveBidiReactor under ReadPacing::kTurnByTurn, via the RouteChat RPC. The reactor holds
/// the read side and arms nothing until the application calls ResumeRead(). The write side keeps
/// running meanwhile, since a read hold stalls only the reads.
///
/// Clone of its continuous counterpart in route_chat_continuous.h, for the reason recorded there and
/// on the ListFeatures pair: one class instantiated twice would register the same event names twice.
class RouteChat {
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
  /// Delta against the continuous counterpart: this handler owns the re-arm of the read side, so it
  /// ends with ResumeRead() and reports it. The write side is unaffected by that hold.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnWriteDone, which paces the next write.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  /// Sends the next queued note, from this scenario's own queue.
  void SendNextNote();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<routeguide::RouteChat::ClientReactor> reactor_;
  std::vector<routeguide::RouteNote> pending_;

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  spdlog::logger& logger_{VariantLogger("RouteChat.turn")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::turn_by_turn
