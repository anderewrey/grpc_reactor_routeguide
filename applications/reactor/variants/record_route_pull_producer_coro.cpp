///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/record_route_pull_producer_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::pull::producer::coro {

RecordRoute::RecordRoute(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void RecordRoute::Start(std::vector<routeguide::Point> points) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring {} points", points.size());
    return;
  }
  roots_.Spawn(Run(std::move(points), Activity::Begin()));
}

RpcReactor::coro::Task<> RecordRoute::Run(std::vector<routeguide::Point> points,
                                          [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  const auto count = points.size();
  // (Point 1.1) Create reactor, which pulls the first point, here, then each next one on a gRPC thread once
  // the write before it completed, and closes the request stream after the last one. The points are moved in.
  Source source{executor_, stub_, std::move(points), CreateClientContext()};
  logger_.info("         | rpc started, {} points handed to the reactor", count);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status and the summary,
  // swapped out of the reactor rather than deep-copied. (Point 4.11) The reactor is destroyed.
  const auto [status, response] = co_await source.Finish();
  assert(OnApplicationThread());
  if (status.ok()) {
    logger_.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
  } else {
    logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  }
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::pull::producer::coro
