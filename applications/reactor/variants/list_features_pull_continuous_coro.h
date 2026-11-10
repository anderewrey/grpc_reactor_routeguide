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

namespace rg_demo::pull::continuous::coro {

/// Exhibits coro::Reader with its latest-only buffer, over ActiveReadReactor under Delivery::kPull and
/// ReadPacing::kContinuous, via the ListFeatures RPC. One coroutine per RPC runs on the application thread from
/// start to end. The reactor lends each message to the read callback and reads the next one into the same storage
/// as soon as it returns, so the stream never waits for the coroutine: the reader copies each message into its one
/// slot, overwriting the message the coroutine has not taken, and each Read() takes the newest one, or waits.
///
/// Coroutine twin of list_features_pull_continuous.h, deliberately: the reactor and its configuration are the
/// same, and `diff list_features_pull_continuous.cpp list_features_pull_continuous_coro.cpp` is what consuming
/// them from a coroutine changes. The parent consumes every message inside the reaction, on the gRPC thread, where
/// a coroutine's code never runs, so the twin consumes on the application thread the messages it takes, and counts
/// those it did not as overwritten. Its sibling list_features_push_continuous_coro.h differs along the delivery
/// only: each message is handed over owned, so the reader queues them all.
class ListFeatures {
  // The reactor flavour this scenario exhibits: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Reader =
      RpcReactor::coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>>;

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
  /// Servant component, as one function: sends the request, takes the newest message each time, and counts those
  /// overwritten in between, then awaits the end of the RPC.
  /// @param rect bounding the features to list, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(routeguide::Rectangle rect, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("ListFeatures.pull.cont.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::pull::continuous::coro
