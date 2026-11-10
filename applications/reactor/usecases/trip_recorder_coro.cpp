///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/trip_recorder_coro.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase::coro {
namespace {
// How long storing one report takes. The server streams the whole rectangle in a few milliseconds, so
// storage this slow is still on the first reports when the last one arrives.
constexpr auto kStoreTime{std::chrono::milliseconds(1)};
}  // anonymous namespace

TripRecorder::TripRecorder(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void TripRecorder::Start(routeguide::Rectangle rect) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring: {}", protobuf_utils::ToString(rect));
    return;
  }
  roots_.Spawn(Run(std::move(rect), Activity::Begin()));
}

RpcReactor::coro::Task<> TripRecorder::Run(routeguide::Rectangle rect, [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which sends the request and arms the first read
  Reader reader{executor_, stub_, rect, CreateClientContext()};
  logger_.info("         | rpc started");
  unsigned seq = 0;
  // Delta against list_features_push_continuous_coro.cpp: the largest backlog seen, the reports queued but not
  // stored yet, which nothing bounds.
  std::size_t peak_backlog = 0;
  // (Point 2.14) The read callback queues each report, owned, in the reader, and the reactor arms the next read as
  // soon as it returns, so the queue grows as fast as the server sends. (Point 2.18) Each Read() resumes on the
  // application thread with the oldest report queued, or waits for the next.
  while (const auto report = co_await reader.Read()) {
    assert(OnApplicationThread());
    // (Point 2.19) update application with response: store it in the trip log.
    logger_.info("RESPONSE | #{} {}: {}", ++seq, report->GetTypeName(), protobuf_utils::ToString(*report));
    // Stands for the write to storage. It holds the application thread, as real work would.
    std::this_thread::sleep_for(kStoreTime);
    // Delta against list_features_push_continuous_coro.cpp: what arrived while this report was stored.
    const std::size_t backlog = reader.Queued();
    peak_backlog = std::max(peak_backlog, backlog);
    logger_.info("stored   | #{}, backlog {}", seq, backlog);
  }
  // (Point 4.7, 4.8) Read() gave nothing: every report queued is stored, and the feed has ended. Unlike the
  // callback twin, the coroutine learns of the end only once it has caught up, so the trip log is complete here.
  assert(OnApplicationThread());
  logger_.info("         | stream ended after {} messages", seq);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status. (Point 4.11)
  // The reactor is destroyed.
  const grpc::Status status = co_await reader.Finish();
  assert(OnApplicationThread());
  logger_.info("totals   | {} reports stored in the trip log, peak backlog {}", seq, peak_backlog);
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::usecase::coro
