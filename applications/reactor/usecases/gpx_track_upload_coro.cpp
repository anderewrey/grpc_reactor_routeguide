///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/gpx_track_upload_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase::coro {

GpxTrackUpload::GpxTrackUpload(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void GpxTrackUpload::Start(std::vector<routeguide::Point> track) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring {} points", track.size());
    return;
  }
  roots_.Spawn(Run(std::move(track), Activity::Begin()));
}

RpcReactor::coro::Task<> GpxTrackUpload::Run(std::vector<routeguide::Point> track,
                                             [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  // How many points the track holds, which the summary must count.
  const std::size_t track_size = track.size();
  // (Point 1.1) Create reactor, which pulls the first point, here, then each next one on a gRPC thread once
  // the write before it completed, and closes the request stream after the last one. The track is moved in.
  Source upload{executor_, stub_, std::move(track), CreateClientContext()};
  logger_.info("         | rpc started, {} points handed to the reactor", track_size);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status and the summary,
  // swapped out of the reactor rather than deep-copied. (Point 4.11) The reactor is destroyed.
  const auto [status, response] = co_await upload.Finish();
  assert(OnApplicationThread());
  if (status.ok()) {
    logger_.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
    // Delta against record_route_pull_producer_coro.cpp: the upload screen shows the summary. Every point of the
    // track was sent, and counted.
    assert(static_cast<std::size_t>(response.point_count()) == track_size);
    logger_.info("summary  | {} points, {} known places passed, {} m", response.point_count(),
                 response.feature_count(), response.distance());
  } else {
    logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  }
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::usecase::coro
