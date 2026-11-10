///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/live_route_recording_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "applications/reactor/reactor_eventloop.h"
#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase::coro {
namespace {
// The GPS produces a fix every kGpsPeriod, as in live_route_recording.cpp. Fixes kTunnelFirst to kTunnelLast are
// delivered together, at the time of the last one, as a receiver does with the fixes it buffered in a tunnel:
// those arrive faster than a write completes, so they wait for the coroutine.
constexpr std::chrono::milliseconds kGpsPeriod{20};
constexpr std::size_t kTunnelFirst{3};
constexpr std::size_t kTunnelLast{5};

/// When the GPS delivers fix `i`, counted from 0, after it was switched on.
std::chrono::milliseconds FixTime(std::size_t i) {
  const std::size_t fix = (i >= kTunnelFirst && i <= kTunnelLast) ? kTunnelLast : i;
  return kGpsPeriod * static_cast<int>(fix + 1);
}
}  // anonymous namespace

LiveRouteRecording::LiveRouteRecording(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void LiveRouteRecording::Start(std::vector<routeguide::Point> drive) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring {} points", drive.size());
    return;
  }
  if (drive.empty()) {
    logger_.info("         | no points to send, ignoring");
    return;
  }
  roots_.Spawn(Run(std::move(drive), Activity::Begin()));
}

RpcReactor::coro::Task<> LiveRouteRecording::Run(std::vector<routeguide::Point> drive,
                                                 [[maybe_unused]] Activity activity) {
  using Clock = std::chrono::steady_clock;
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which starts the RPC and takes the write-flow hold
  Writer writer{executor_, stub_, CreateClientContext()};
  logger_.info("         | rpc started");
  // Switches the GPS on. Nothing is sent before the first fix.
  const auto gps_on = Clock::now();
  std::size_t fixes = 0;
  for (auto& point : drive) {
    // Delta against record_route_push_coro.cpp: the GPS fix comes first. A fix already due came while the last
    // write was in flight, and waited for it.
    const auto due = gps_on + FixTime(fixes);
    ++fixes;
    if (const auto now = Clock::now(); now < due) {
      co_await RpcReactor::coro::EventLoopExecutor::After(std::chrono::ceil<std::chrono::milliseconds>(due - now));
      assert(OnApplicationThread());
      logger_.info("gps      | fix #{} sent at once", fixes);
    } else {
      logger_.info("gps      | fix #{} waited for a write in flight", fixes);
    }
    // Resumed on the application thread once the write has completed. A rejected write means the RPC has ended.
    if (!co_await writer.Write(std::move(point))) {
      assert(OnApplicationThread());
      logger_.info("         | write rejected, {} point(s) left unsent", drive.size() - fixes + 1);
      break;
    }
    assert(OnApplicationThread());
  }
  // The drive has ended: the GPS has no fix left.
  static_cast<void>(co_await writer.Close());
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status and the summary,
  // moved out of the message the done callback handed over. (Point 4.11) The reactor is destroyed.
  auto [status, response] = co_await writer.Finish();
  assert(OnApplicationThread());
  if (status.ok()) {
    logger_.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
    // Every fix of the drive was sent, and counted.
    assert(static_cast<std::size_t>(response.point_count()) == fixes);
    // Handed to the trip history, which keeps the summary it now owns.
    trip_history_.push_back(std::make_unique<routeguide::RecordRoute::ResponseT>(std::move(response)));
    logger_.info("history  | {} trip(s) recorded", trip_history_.size());
  } else {
    logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  }
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::usecase::coro
