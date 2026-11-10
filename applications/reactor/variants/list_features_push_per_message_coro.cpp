///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/list_features_push_per_message_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstddef>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::push::per_message::coro {
namespace {
// How many messages may wait for the coroutine before the reader holds the stream. Small, so the bound engages
// within the rectangle's features.
constexpr std::size_t kCapacity{2};
}  // anonymous namespace

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
  Reader reader{executor_, stub_, rect, kCapacity, CreateClientContext()};
  logger_.info("         | rpc started, holding the stream once {} messages wait", kCapacity);
  unsigned seq = 0;
  // (Point 2.6, or 2.14 when not held) The read callback queues each message, owned, in the reader, and holds the
  // stream once kCapacity wait. Delta against the callback parent: the hold decision is the reader's, not a rule of
  // the scenario. (Point 2.8) Each Read() resumes on the application thread with the oldest message queued, or
  // waits for the next.
  while (const auto response = co_await reader.Read()) {
    assert(OnApplicationThread());
    // (Point 2.9) update application with response. (Point 2.10, 2.11, 2.12) The Read() that took it below the
    // capacity has already released the hold, if any: nothing to resume here.
    logger_.info("RESPONSE | #{} {}: {}, {} queued", ++seq, response->GetTypeName(),
                 protobuf_utils::ToString(*response), reader.Queued());
  }
  // (Point 4.7, 4.8) Read() gave nothing: the queue is empty and the stream has ended
  assert(OnApplicationThread());
  logger_.info("         | stream ended after {} messages", seq);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status. (Point 4.11)
  // The reactor is destroyed.
  const grpc::Status status = co_await reader.Finish();
  assert(OnApplicationThread());
  logger_.info("totals   | {} features read, stream held {} times", seq, reader.Holds());
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::push::per_message::coro
