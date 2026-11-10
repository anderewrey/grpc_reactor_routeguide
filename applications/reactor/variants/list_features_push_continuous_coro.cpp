///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/list_features_push_continuous_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::push::continuous::coro {

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
  // (Point 2.14) The read callback queues each message, owned, in the reader, and the reactor arms the next read
  // as soon as it returns, so the buffer grows as fast as the server sends. (Point 2.18) Each Read() resumes on
  // the application thread with the oldest message queued, or waits for the next.
  while (const auto response = co_await reader.Read()) {
    assert(OnApplicationThread());
    // (Point 2.19) update application with response. Nothing to resume: the reactor already re-armed the read
    // when the read callback returned.
    logger_.info("RESPONSE | #{} {}: {}", ++seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  }
  // (Point 4.7, 4.8) Read() gave nothing: the buffer is empty and the stream has ended
  assert(OnApplicationThread());
  logger_.info("         | stream ended after {} messages", seq);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status. (Point 4.11)
  // The reactor is destroyed.
  const grpc::Status status = co_await reader.Finish();
  assert(OnApplicationThread());
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::push::continuous::coro
