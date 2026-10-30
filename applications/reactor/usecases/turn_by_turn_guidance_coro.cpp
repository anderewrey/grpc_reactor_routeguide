///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/turn_by_turn_guidance_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <utility>

#include "applications/reactor/reactor_eventloop.h"
#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase::coro {
namespace {
// How long the driver takes to confirm a stop. Longer than the server takes to send the next one, so
// the stream would run ahead of the driver if anything but the confirmation asked for it.
constexpr std::chrono::milliseconds kConfirmTime{2};
}  // anonymous namespace

TurnByTurnGuidance::TurnByTurnGuidance(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void TurnByTurnGuidance::Start(routeguide::Rectangle rect) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring: {}", protobuf_utils::ToString(rect));
    return;
  }
  roots_.Spawn(Run(std::move(rect)));
}

RpcReactor::coro::Task<> TurnByTurnGuidance::Run(routeguide::Rectangle rect) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which sends the request and arms the first read
  Reader reader{executor_, stub_, rect, CreateClientContext()};
  logger_.info("         | rpc started");
  unsigned seq = 0;
  // (Point 2.8) Resumed on the application thread with the stop the read callback handed over, owned.
  while (const auto stop = co_await reader.Read()) {
    assert(OnApplicationThread());
    // (Point 2.9) update application with response: the screen shows the stop, and keeps it, owned, until
    // the driver confirms it.
    logger_.info("RESPONSE | #{} {}: {}", ++seq, stop->GetTypeName(), protobuf_utils::ToString(*stop));
    // Delta against list_features_push_turn_by_turn_coro.cpp: the driver's confirmation comes first. Nothing
    // is read meanwhile, for as long as the driver takes: the reactor holds the stream until the next Read().
    co_await RpcReactor::coro::EventLoopExecutor::After(kConfirmTime);
    assert(OnApplicationThread());
    // (Point 2.10, 2.11, 2.12) The next Read() arms the read and releases the hold OnReadDone() took, with
    // ResumeRead(). The stop leaves the screen as the loop moves on.
    logger_.info("resumed  | #{} driver confirmed, arming next read", seq);
  }
  // (Point 4.7, 4.8) Read() gave nothing: the route has ended
  assert(OnApplicationThread());
  logger_.info("         | stream ended after {} messages", seq);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status. (Point 4.11)
  // The reactor is destroyed.
  const grpc::Status status = co_await reader.Finish();
  assert(OnApplicationThread());
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::usecase::coro
