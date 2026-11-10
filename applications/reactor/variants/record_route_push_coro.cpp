///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/record_route_push_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::push::coro {

RecordRoute::RecordRoute(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void RecordRoute::Start(std::vector<routeguide::Point> points) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring {} points", points.size());
    return;
  }
  if (points.empty()) {
    logger_.info("         | no points to send, ignoring");
    return;
  }
  roots_.Spawn(Run(std::move(points), Activity::Begin()));
}

RpcReactor::coro::Task<> RecordRoute::Run(std::vector<routeguide::Point> points,
                                          [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which starts the RPC and takes the write-flow hold
  Writer writer{executor_, stub_, CreateClientContext()};
  logger_.info("         | rpc started");
  std::size_t sent = 0;
  for (auto& point : points) {
    // Resumed on the application thread once the write has completed: the next write may start. A rejected
    // write means the RPC has ended, so the points left are not sent.
    if (!co_await writer.Write(std::move(point))) {
      assert(OnApplicationThread());
      logger_.info("         | write rejected, {} point(s) left unsent", points.size() - sent);
      break;
    }
    assert(OnApplicationThread());
    logger_.info("         | write #{} done", ++sent);
  }
  // Delta against the callback parent: the last point is not sent with SendLastRequest(), since the loop
  // does not single it out. The request stream is closed after it, which never suspends.
  static_cast<void>(co_await writer.Close());
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status and the summary,
  // moved out of the message the done callback handed over. (Point 4.11) The reactor is destroyed.
  const auto [status, response] = co_await writer.Finish();
  assert(OnApplicationThread());
  if (status.ok()) {
    logger_.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
  } else {
    logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  }
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::push::coro
