///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <spdlog/logger.h>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::push::turn_by_turn::coro {

/// Exhibits coro::Reader, the reader without a buffer, over ActiveReadReactor under Delivery::kPush and
/// ReadPacing::kTurnByTurn, via the ListFeatures RPC. One coroutine per RPC runs on the application thread from
/// start to end: each Read() resumes it with the message the read callback handed over, owned. The read that
/// message left unarmed is armed by the next Read(), with ResumeRead(), so nothing is read until the
/// coroutine asks for it.
///
/// Coroutine twin of list_features_push_turn_by_turn.h, deliberately: the reactor and its configuration are
/// the same, and `diff list_features_push_turn_by_turn.cpp list_features_push_turn_by_turn_coro.cpp` is what
/// consuming them from a coroutine changes. The callbacks, the event names, their handlers, the read counters
/// and the reactor slot are gone, all inside the awaitable.
/// Its sibling list_features_pull_turn_by_turn_coro.h differs along the delivery only: the message is swapped
/// out with GetResponse(), which arms the next read at once.
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Reader =
      RpcReactor::coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kTurnByTurn>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  ListFeatures(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  ListFeatures(const ListFeatures&) = delete;
  ListFeatures& operator=(const ListFeatures&) = delete;
  ListFeatures(ListFeatures&&) = delete;
  ListFeatures& operator=(ListFeatures&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~ListFeatures() = default;

  /// Proxy component: spawns the coroutine, which sends the request and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Servant component, as one function: sends the request, reads the stream one message at a time, then
  /// awaits the end of the RPC.
  /// @param rect bounding the features to list, taken by value since the coroutine outlives the call to Start()
  RpcReactor::coro::Task<> Run(routeguide::Rectangle rect);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("ListFeatures.push.turn.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::push::turn_by_turn::coro
