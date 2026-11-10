///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <spdlog/logger.h>

#include <cstddef>
#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::push::continuous::coro {

/// Exhibits coro::Bidi, the bidirectional join, over ActiveBidiReactor under Delivery::kPush,
/// Submission::kApplicationPushes and ReadPacing::kContinuous, via the RouteChat RPC. One coroutine per RPC owns
/// the Bidi, and joins two flows on the application thread with coro::WhenAll(): one reads every reply until the
/// stream ends, one writes every note, awaiting each write, then closes the request stream. Only the joined
/// coroutine awaits Finish(), once both flows are done.
///
/// Coroutine twin of route_chat_push_continuous.h, deliberately: the reactor and its configuration are the same,
/// and `diff route_chat_push_continuous.cpp route_chat_push_continuous_coro.cpp` is what consuming them from
/// coroutines changes. The callbacks, the event names, their handlers, the pending queue and its pump, and the
/// reactor slot are gone: the write flow's loop is the pump, and the read flow's loop the read handler. The
/// reactor still arms each read itself, and the Bidi queues every reply it hands over, so the read side never
/// waits for the reading coroutine.
class RouteChat {
  // The reactor flavour this scenario exhibits: its Delivery approach, its Submission, then its ReadPacing mode.
  using Delivery = routeguide::RouteChat::Delivery;
  using Submission = routeguide::RouteChat::Submission;
  using ReadPacing = routeguide::RouteChat::ReadPacing;
  using Chat = RpcReactor::coro::Bidi<
      routeguide::RouteChat::ClientReactor<Delivery::kPush, Submission::kApplicationPushes, ReadPacing::kContinuous>>;

 public:
  /// @param executor resumes the scenario's coroutines on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  RouteChat(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  RouteChat(const RouteChat&) = delete;
  RouteChat& operator=(const RouteChat&) = delete;
  RouteChat(RouteChat&&) = delete;
  RouteChat& operator=(RouteChat&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~RouteChat() = default;

  /// Proxy component: spawns the coroutine, which starts the RPC and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param notes to stream to the server, one write each
  void Start(std::vector<routeguide::RouteNote> notes);

 private:
  /// Servant component: joins the read flow and the write flow, then awaits the status.
  /// @param notes to stream, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(std::vector<routeguide::RouteNote> notes, Activity activity);

  /// The read flow: shows each reply, until the stream ends.
  /// @param chat the RPC, in the frame of Run(), which outlives this flow
  /// @return how many replies were shown
  RpcReactor::coro::Task<unsigned> ShowReplies(Chat* chat);

  /// The write flow: writes each note, awaiting each write, then closes the request stream.
  /// @param chat the RPC, in the frame of Run(), which outlives this flow
  /// @param notes to write, in order
  /// @return how many notes were written
  RpcReactor::coro::Task<std::size_t> SendNotes(Chat* chat, std::vector<routeguide::RouteNote> notes);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("RouteChat.push.cont.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::push::continuous::coro
