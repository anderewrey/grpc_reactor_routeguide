///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <cstddef>
#include <memory>
#include <optional>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: a route recorded earlier, and saved as a GPX file, is uploaded to get its summary: the
/// distance, the known places passed, and the time it took. The whole track is on hand before the upload
/// starts, and the summary is shown once, on the upload screen. A prepared vector of points stands in
/// for the parsed file.
///
/// The variant chosen is ActiveWriteReactor under Delivery::kPull and Submission::kReactorPulls. The
/// source can always answer at once, so the reactor pulls each point from next_request as soon as the
/// previous write completes, and closes the request stream itself at the end of the track: the
/// application has no pump to write, and the upload runs at the network's pace. kPull fits the summary,
/// which the upload screen reads in the done handler and does not keep.
///
/// Clone of record_route_pull_producer.h, along the use only: the reactor is the same, and the diff is
/// the track and the check of its summary.
class GpxTrackUpload {
  // The reactor flavour this scenario uses: its Delivery approach, then its Submission.
  using Delivery = routeguide::RecordRoute::Delivery;
  using Submission = routeguide::RecordRoute::Submission;
  using Callbacks = routeguide::RecordRoute::Callbacks<Delivery::kPull, Submission::kReactorPulls>;
  using ClientReactor = routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit GpxTrackUpload(routeguide::RouteGuide::Stub& stub);

  GpxTrackUpload(const GpxTrackUpload&) = delete;
  GpxTrackUpload& operator=(const GpxTrackUpload&) = delete;
  GpxTrackUpload(GpxTrackUpload&&) = delete;
  GpxTrackUpload& operator=(GpxTrackUpload&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~GpxTrackUpload();

  /// Proxy component: creates the Method Request, which pulls the first point before it returns.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param track the points of the recorded route, pulled one at a time by the reactor
  void Start(std::vector<routeguide::Point> track);

 private:
  /// Application-thread handler for OnWriteDone, which only logs: the reactor pulls the next point.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which shows the summary and destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  /// The next_request callback's body: the next queued point, or std::nullopt once the queue is empty.
  std::optional<routeguide::Point> NextPoint();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The queue belongs to this object, and to the reactor while the RPC runs: Start() fills it before
  // creating the reactor, and only next_request reads it afterward. The reactor pulls once at a time,
  // first on the application thread, then on gRPC threads, each pull after the write before it
  // completed, so the pulls never overlap.
  std::vector<routeguide::Point> pending_;
  unsigned pulled_seq_{0};
  // How many points the track holds, which the summary must count. Application thread only.
  std::size_t track_size_{0};

  spdlog::logger& logger_{VariantLogger("GpxTrackUpload")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
