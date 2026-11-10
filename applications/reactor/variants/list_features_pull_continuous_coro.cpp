///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/list_features_pull_continuous_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstdint>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::pull::continuous::coro {

ListFeatures::ListFeatures(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void ListFeatures::Start(routeguide::Rectangle rect) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring: {}", protobuf_utils::ToString(rect));
    return;
  }
  roots_.Spawn(Run(std::move(rect), Activity::Begin()));
}

RpcReactor::coro::Task<> ListFeatures::Run(routeguide::Rectangle rect, [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which sends the request and arms the first read
  Reader reader{executor_, stub_, rect, CreateClientContext()};
  logger_.info("         | rpc started");
  unsigned seq = 0;
  std::uint64_t skipped = 0;
  // (Point 2.16) The read callback copies each lent message into the reader's slot, replacing one not taken yet,
  // and the reactor reads the next one into the same storage as soon as it returns. Each Read() resumes on the
  // application thread with the newest message, or waits for the next. Delta against the callback parent: the
  // message reaches the coroutine, but only the newest; each one overwritten is logged here, since the parent
  // logged every message in the reaction.
  while (const auto response = co_await reader.Read()) {
    assert(OnApplicationThread());
    for (std::uint64_t i = 0; i < reader.Overwritten(); ++i) {
      logger_.info("skipped  | #{} overwritten before it was read", ++seq);
    }
    skipped += reader.Overwritten();
    logger_.info("RESPONSE | #{} {}: {}", ++seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  }
  // (Point 4.7, 4.8) Read() gave nothing: the slot is empty and the stream has ended, so the last message was read
  assert(OnApplicationThread());
  logger_.info("         | stream ended after {} messages", seq);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status. (Point 4.11)
  // The reactor is destroyed.
  const grpc::Status status = co_await reader.Finish();
  assert(OnApplicationThread());
  logger_.info("totals   | {} features read, {} overwritten before the coroutine took them", seq, skipped);
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::pull::continuous::coro
