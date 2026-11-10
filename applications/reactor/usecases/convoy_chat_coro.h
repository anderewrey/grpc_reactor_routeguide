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

namespace rg_demo::usecase::coro {

/// A use case: the drivers of a convoy exchange notes tied to their positions, such as a hazard on the road. A
/// note is sent when someone types it, so notes appear over time, and two drivers can type at the same moment.
/// Every note received must be shown, and the chat must never stall, whether or not anyone is typing.
///
/// The variant chosen is coro::Bidi over ActiveBidiReactor under Delivery::kPush,
/// Submission::kApplicationPushes and ReadPacing::kContinuous, for the reasons convoy_chat.h gives: a note
/// exists only once typed, and the read side must never wait for the screen. Here, the typing flow awaits each
/// typed note, then its write: a note typed while a write was in flight has waited for the flow, which sends it
/// as soon as the write resumes it, so no queue is kept. The reading flow shows every note the Bidi queued,
/// meanwhile. A delay awaited with EventLoopExecutor::After() stands in for the drivers typing.
///
/// The reference server keeps one note history for the whole service, as convoy_chat.h describes, so the
/// transcript varies with what ran before.
///
/// Coroutine twin of convoy_chat.h, and clone of route_chat_push_continuous_coro.h along the use only: the
/// awaitable and the join are the same, and the diff is the typing the write flow awaits before each write.
class ConvoyChat {
  // The reactor flavour this scenario uses: its Delivery approach, its Submission, then its ReadPacing mode.
  using Delivery = routeguide::RouteChat::Delivery;
  using Submission = routeguide::RouteChat::Submission;
  using ReadPacing = routeguide::RouteChat::ReadPacing;
  using Chat = RpcReactor::coro::Bidi<
      routeguide::RouteChat::ClientReactor<Delivery::kPush, Submission::kApplicationPushes, ReadPacing::kContinuous>>;

 public:
  /// @param executor resumes the scenario's coroutines on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  ConvoyChat(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  ConvoyChat(const ConvoyChat&) = delete;
  ConvoyChat& operator=(const ConvoyChat&) = delete;
  ConvoyChat(ConvoyChat&&) = delete;
  ConvoyChat& operator=(ConvoyChat&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~ConvoyChat() = default;

  /// Proxy component: spawns the coroutine, which starts the RPC, lets the drivers type, and returns at its
  /// first suspension. Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param notes the drivers will type, in order
  void Start(std::vector<routeguide::RouteNote> notes);

 private:
  /// Servant component: joins the chat screen and the typing, then awaits the status.
  /// @param notes the drivers will type, taken by value since the coroutine outlives Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(std::vector<routeguide::RouteNote> notes, Activity activity);

  /// The read flow: shows each note received in the chat, until the stream ends.
  /// @param chat the RPC, in the frame of Run(), which outlives this flow
  /// @return how many notes were shown
  RpcReactor::coro::Task<unsigned> ShowNotes(Chat* chat);

  /// The write flow: awaits each typed note, then its write, then closes the request stream once nobody has a
  /// note left to type.
  /// @param chat the RPC, in the frame of Run(), which outlives this flow
  /// @param notes the drivers will type, in order
  /// @return how many notes were typed
  RpcReactor::coro::Task<std::size_t> TypeNotes(Chat* chat, std::vector<routeguide::RouteNote> notes);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("ConvoyChat.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::usecase::coro
