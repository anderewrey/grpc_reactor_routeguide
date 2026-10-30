///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/list_features_pull_turn_by_turn_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::pull::turn_by_turn::coro {

ListFeatures::ListFeatures(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void ListFeatures::Start(routeguide::Rectangle rect) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring: {}", protobuf_utils::ToString(rect));
    return;
  }
  roots_.Spawn(Run(std::move(rect)));
}

RpcReactor::coro::Task<> ListFeatures::Run(routeguide::Rectangle rect) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which sends the request and arms the first read
  Reader reader{executor_, stub_, rect, CreateClientContext()};
  logger_.info("         | rpc started");
  unsigned seq = 0;
  // (Point 2.8) Resumed on the application thread once a message is held. (Point 2.9 to 2.13) Read() swaps
  // it out of the reactor rather than deep-copying it, and its GetResponse() arms the next read and releases
  // the hold OnReadDone() took, all in one call: the next message can arrive while this one is logged.
  while (const auto response = co_await reader.Read()) {
    assert(OnApplicationThread());
    // update application with response
    logger_.info("RESPONSE | #{} {}: {}", ++seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  }
  // (Point 4.7, 4.8) Read() gave nothing: the stream has ended
  assert(OnApplicationThread());
  logger_.info("         | stream ended after {} messages", seq);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status. (Point 4.11)
  // The reactor is destroyed.
  const grpc::Status status = co_await reader.Finish();
  assert(OnApplicationThread());
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::pull::turn_by_turn::coro
