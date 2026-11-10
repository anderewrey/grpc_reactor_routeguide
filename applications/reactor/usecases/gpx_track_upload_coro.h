///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <spdlog/logger.h>

#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase::coro {

/// A use case: a route recorded earlier, and saved as a GPX file, is uploaded to get its summary: the distance,
/// the known places passed, and the time it took. The whole track is on hand before the upload starts, and the
/// summary is shown once, on the upload screen. A prepared vector of points stands in for the parsed file.
///
/// The variant chosen is coro::Source over ActiveWriteReactor under Delivery::kPull and
/// Submission::kReactorPulls, for the reasons gpx_track_upload.h gives: the track can always answer at once, so
/// the reactor pulls each point as soon as the previous write completes, and the upload runs at the network's
/// pace. The coroutine hands the track over, and awaits only the summary.
///
/// Coroutine twin of gpx_track_upload.h, and clone of record_route_pull_producer_coro.h along the use only: the
/// awaitable is the same, and the diff is the track and the check of its summary.
class GpxTrackUpload {
  // The reactor flavour this scenario uses: its Delivery approach, then its Submission.
  using Delivery = routeguide::RecordRoute::Delivery;
  using Submission = routeguide::RecordRoute::Submission;
  using Source =
      RpcReactor::coro::Source<routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>>;

 public:
  /// @param executor resumes the scenario's coroutine on the application thread, and must outlive this object
  /// @param stub of the RouteGuide API, which must outlive this object
  GpxTrackUpload(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub);

  GpxTrackUpload(const GpxTrackUpload&) = delete;
  GpxTrackUpload& operator=(const GpxTrackUpload&) = delete;
  GpxTrackUpload(GpxTrackUpload&&) = delete;
  GpxTrackUpload& operator=(GpxTrackUpload&&) = delete;

  /// Destroys a coroutine still suspended, whose awaitable cancels its RPC and destroys the reactor once its
  /// OnDone() has run. Only once the event loop no longer runs, since a queued resumption would refer to it.
  ~GpxTrackUpload() = default;

  /// Proxy component: spawns the coroutine, whose Source pulls the first point before the coroutine suspends.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param track the points of the recorded route, pulled one at a time by the reactor
  void Start(std::vector<routeguide::Point> track);

 private:
  /// Servant component, as one function: hands the track to the reactor, then shows the summary.
  /// @param track the points of the recorded route, taken by value since the coroutine outlives Start()
  /// @param activity the scenario in progress, which the frame holds until the coroutine ends
  RpcReactor::coro::Task<> Run(std::vector<routeguide::Point> track, Activity activity);

  RpcReactor::coro::Executor& executor_;
  routeguide::RouteGuide::Stub& stub_;
  spdlog::logger& logger_{VariantLogger("GpxTrackUpload.coro")};

  // Declared last, so a coroutine still suspended is destroyed before anything it reaches is gone.
  RpcReactor::coro::RootTasks roots_;
};

}  // namespace rg_demo::usecase::coro
