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
#include <optional>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::pull::per_message::producer {

/// Exhibits ActiveBidiReactor under Delivery::kPull, ReadPacing::kPerMessage and Submission::kReactorPulls,
/// via the RouteChat RPC. The read side is that of route_chat_pull_per_message.h, unchanged. The
/// write side is pulled: the reactor asks the next_request callback for each note, once while it starts
/// the RPC and again after each write completes, and closes the request stream itself once the queue is
/// empty. The application makes no write call, and the write side takes no hold, so the read holds
/// read_ok takes are the only ones.
///
/// Clone of route_chat_pull_per_message.h, which differs from it along the outgoing axis only: there,
/// the application pushes each note from its OnWriteDone handler. The OnWriteDone event remains, for the
/// log, but paces nothing.
class RouteChat {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its Submission. Its ReadPacing
  // is the default, ReadPacing::kPerMessage.
  using Delivery = routeguide::RouteChat::Delivery;
  using Submission = routeguide::RouteChat::Submission;
  using Callbacks = routeguide::RouteChat::Callbacks<Delivery::kPull, Submission::kReactorPulls>;
  using ClientReactor = routeguide::RouteChat::ClientReactor<Delivery::kPull, Submission::kReactorPulls>;

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

  /// Proxy component: creates the Method Request, which pulls the first note before it returns.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param notes to stream to the server, pulled one at a time by the reactor
  void Start(std::vector<routeguide::RouteNote> notes);

 private:
  /// Application-thread handler for OnReadDoneOk.
  /// Runs for kept notes only. The message is still in the reactor, and this handler pulls it with
  /// GetResponse(), which also re-arms the read side. The write side is unaffected by that hold.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnWriteDone, which only logs: the reactor pulls the next note.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the kept and declined totals and destroys the
  /// reactor.
  void OnDone(const EventLoop::Event* event);

  /// The next_request callback's body: the next queued note, or std::nullopt once the queue is empty, which
  /// closes the client's write side; the server may keep sending responses afterward.
  std::optional<routeguide::RouteNote> NextNote();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;
  Activity activity_;

  // The queue belongs to this object, and to the reactor while the RPC runs: Start() fills it before
  // creating the reactor, and only next_request reads it afterward. The reactor pulls once at a time,
  // first on the application thread, then on gRPC threads, each pull after the write before it
  // completed, so the pulls never overlap.
  std::vector<routeguide::RouteNote> pending_;
  unsigned pulled_seq_{0};

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared. Both count kept notes only.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  // Declined notes: counted by read_ok on the gRPC thread, and read by OnDone on the application
  // thread, so it is shared, and atomic rather than locked.
  std::atomic<unsigned> skipped_seq_{0};

  // The arbitrary rule: true when read_ok keeps the next note. Only read_ok reads and flips it, before
  // its hand-off, and the next read_ok can only start after a read armed later, so the reactions never
  // share it at once.
  bool keep_next_{false};

  spdlog::logger& logger_{VariantLogger("RouteChat.pull.msg.producer")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::pull::per_message::producer
