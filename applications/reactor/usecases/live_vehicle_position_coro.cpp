///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/live_vehicle_position_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase::coro {
namespace {
// How long redrawing the map takes. The server streams the whole rectangle in a few milliseconds, so a map this
// slow draws only a few of its fixes, and the slot drops the others.
constexpr auto kRedrawTime{std::chrono::milliseconds(5)};
}  // anonymous namespace

LiveVehiclePosition::LiveVehiclePosition(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void LiveVehiclePosition::Start(routeguide::Rectangle rect) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring: {}", protobuf_utils::ToString(rect));
    return;
  }
  roots_.Spawn(Run(std::move(rect), Activity::Begin()));
}

RpcReactor::coro::Task<> LiveVehiclePosition::Run(routeguide::Rectangle rect, [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which sends the request and arms the first read
  Reader reader{executor_, stub_, rect, CreateClientContext()};
  logger_.info("         | rpc started");
  unsigned seq = 0;
  unsigned drawn = 0;
  std::uint64_t replaced = 0;
  // (Point 2.16) The read callback copies each lent fix into the reader's slot, replacing one not drawn yet, and
  // the reactor reads the next one into the same storage as soon as it returns. Each Read() resumes on the
  // application thread with the newest fix, or waits for the next.
  while (const auto fix = co_await reader.Read()) {
    assert(OnApplicationThread());
    // Delta against list_features_pull_continuous_coro.cpp: a fix replaced while the map was redrawn is never drawn.
    for (std::uint64_t i = 0; i < reader.Overwritten(); ++i) {
      logger_.info("skipped  | #{} replaced before drawing", ++seq);
    }
    replaced += reader.Overwritten();
    ++drawn;
    logger_.info("RESPONSE | #{} {}: {}", ++seq, fix->GetTypeName(), protobuf_utils::ToString(*fix));
    // Stands for the map's redraw. It holds the application thread, as real work would.
    std::this_thread::sleep_for(kRedrawTime);
  }
  // (Point 4.7, 4.8) Read() gave nothing: the feed has ended, and the newest fix was drawn last, as the slot never
  // drops the newest one.
  assert(OnApplicationThread());
  logger_.info("         | stream ended after {} messages", seq);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status. (Point 4.11)
  // The reactor is destroyed.
  const grpc::Status status = co_await reader.Finish();
  assert(OnApplicationThread());
  logger_.info("totals   | {} fixes, {} drawn, {} replaced before drawing, last drawn #{}", seq, drawn, replaced, seq);
  assert(drawn + replaced == seq);
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::usecase::coro
