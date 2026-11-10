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

namespace rg_demo::usecase::coro {

/// A use case: a navigation app saves a region's places for offline use before a trip. Every place must be saved,
/// and the download should run at full speed, but the disk writer is slower than the network: once too many places
/// wait for it, the download must pause rather than fill memory, and resume once the writer has caught up.
///
/// The variant chosen is coro::Reader with its bounded buffer, over ActiveReadReactor under Delivery::kPush and
/// ReadPacing::kPerMessage, for the reason offline_map_download.h gives. Each place is handed over owned and queued
/// in the reader, which reads on while fewer than kMaxInFlight places wait for the writer, and holds the stream once
/// that many do; the Read() that takes the queue below that bound resumes it. ListFeatures stands in for the
/// download: the rectangle is the region.
///
/// Coroutine twin of offline_map_download.h, and clone of list_features_push_per_message_coro.h along the use only:
/// the awaitable is the same, and the diff is the writer and its bound. The callback twin takes the hold decision
/// in read_ok, from a counter it shares with its handler; here the reader takes it from its own queue, so the
/// places waiting are those queued, not counting the one the writer is saving.
class OfflineMapDownload {
  // The reactor flavour this scenario uses: its Delivery approach only. Its ReadPacing is the default,
  // ReadPacing::kPerMessage.
  using Delivery = routeguide::ListFeatures::Delivery;
  using Reader = RpcReactor::coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  OfflineMapDownload(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  OfflineMapDownload(const OfflineMapDownload&) = delete;
  OfflineMapDownload& operator=(const OfflineMapDownload&) = delete;
  OfflineMapDownload(OfflineMapDownload&&) = delete;
  OfflineMapDownload& operator=(OfflineMapDownload&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~OfflineMapDownload() = default;

  /// Proxy component: spawns the coroutine, which sends the request and returns at its first suspension.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Servant component, as one function: sends the request, saves every place, in order, which resumes the
  /// download whenever it was paused, then awaits the end of the RPC and reports the totals.
  /// @param rect bounding the features to list, taken by value since the coroutine outlives the call to Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(routeguide::Rectangle rect, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("OfflineMapDownload.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::usecase::coro
