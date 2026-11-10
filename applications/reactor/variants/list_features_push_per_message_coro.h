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

namespace rg_demo::push::per_message::coro {

/// Exhibits coro::Reader with its bounded buffer, over ActiveReadReactor under Delivery::kPush and
/// ReadPacing::kPerMessage, via the ListFeatures RPC. One coroutine per RPC runs on the application thread from
/// start to end. The reader takes the per-message decision itself: each message is handed over owned and queued
/// inside the reader, which lets the reactor read on while fewer than kCapacity messages wait, and holds the
/// stream once that many do, until a Read() takes one.
///
/// Coroutine twin of list_features_push_per_message.h, deliberately: the reactor and its configuration are the
/// same, and `diff list_features_push_per_message.cpp list_features_push_per_message_coro.cpp` is what consuming
/// them from a coroutine changes. The parent decides in read_ok, on the gRPC thread, by an arbitrary rule; a
/// coroutine's code never runs there, so the decision is the reader's own, from its backlog, and the coroutine only
/// chooses the capacity. Its sibling list_features_push_continuous_coro.h differs along the pacing only: the
/// stream never waits, and nothing bounds the queue.
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach only. Its ReadPacing is the default,
  // ReadPacing::kPerMessage.
  using Delivery = routeguide::ListFeatures::Delivery;
  using Reader = RpcReactor::coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush>>;

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
  /// Servant component, as one function: sends the request, takes every message in order, which releases the
  /// stream whenever it was held, then awaits the end of the RPC.
  /// @param rect bounding the features to list, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(routeguide::Rectangle rect, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("ListFeatures.push.msg.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::push::per_message::coro
