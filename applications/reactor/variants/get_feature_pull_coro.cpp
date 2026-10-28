///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/get_feature_pull_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::pull::coro {

GetFeature::GetFeature(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void GetFeature::Start(routeguide::Point point) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring: {}", protobuf_utils::ToString(point));
    return;
  }
  roots_.Spawn(Run(std::move(point)));
}

RpcReactor::coro::Task<> GetFeature::Run(routeguide::Point point) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which sends the request
  Call call{executor_, stub_, point, CreateClientContext()};
  logger_.info("         | rpc started");
  // (Point 3.5) Resumed on the application thread once OnDone() has run. (Point 3.6) The response is
  // swapped out of the reactor rather than deep-copied, and the reactor destroyed.
  const auto [status, response] = co_await call.Finish();
  assert(OnApplicationThread());
  if (status.ok()) {
    // (Point 3.7) update application with response
    logger_.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
  } else {
    logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  }
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::pull::coro
