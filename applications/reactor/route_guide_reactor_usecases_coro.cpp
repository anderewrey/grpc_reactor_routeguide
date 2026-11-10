///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include <EventLoop.h>
#include <gflags/gflags.h>
#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/create_channel.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>

#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/usecases/live_vehicle_position_coro.h"
#include "applications/reactor/usecases/offline_map_download_coro.h"
#include "applications/reactor/usecases/trip_recorder_coro.h"
#include "applications/reactor/usecases/turn_by_turn_guidance_coro.h"
#include "applications/reactor/usecases/whats_here_coro.h"
#include "applications/reactor/demo_support.h"
#include "rg_service/rg_utils.h"
#include "rg_service/route_guide_service.h"

/************************
 * Coroutine use cases of the Active Object reactors
 *
 * The coroutine twin of route_guide_reactor_usecases. It runs the coroutine use cases, under usecases/ with a
 * _coro suffix and in rg_demo::usecase::coro, each a clone of the coroutine variant it uses, along the use
 * only, and the twin of the callback use case it serves. Two diffs describe each one:
 * `diff usecases/<use case>.cpp usecases/<use case>_coro.cpp` is what a coroutine changes for the same
 * situation, and `diff variants/<variant>_coro.cpp usecases/<use case>_coro.cpp` is what the application adds.
 * The component mapping is that of route_guide_active_reactor_client_coro.
 *
 * - rg_demo::usecase::coro::WhatsHere, GetFeature under kPull through coro::Unary: someone taps the map, and a
 *   card shows the name of the place there, or that nothing named is there.
 * - rg_demo::usecase::coro::TurnByTurnGuidance, ListFeatures under kPush and kTurnByTurn through coro::Reader: a
 *   guidance screen shows one stop at a time, and the coroutine reads the next one only once it has awaited the
 *   driver's confirmation.
 * - rg_demo::usecase::coro::TripRecorder, ListFeatures under kPush and kContinuous through coro::Reader with its
 *   unbounded buffer: a trip recorder stores every position report, while the feed runs ahead and the reports
 *   not stored yet queue in the reader.
 * - rg_demo::usecase::coro::LiveVehiclePosition, ListFeatures under kPull and kContinuous through coro::Reader
 *   with its latest-only buffer: a map draws the newest position fix, and the fixes arriving during a redraw
 *   replace each other in the reader's slot.
 * - rg_demo::usecase::coro::OfflineMapDownload, ListFeatures under kPush and kPerMessage through coro::Reader
 *   with its bounded buffer: every place is saved, and the download pauses once three wait for the writer.
 *
 * Reading the output: WhatsHere logs the answer, then the card, as its callback twin does. TurnByTurnGuidance
 * logs a "resumed  | #k driver confirmed" line once the driver confirms stop #k, and RESPONSE #k+1 never comes
 * before it, then "stream ended after N messages", the number of stops its callback twin shows. Every line is
 * on the application thread. TripRecorder logs the backlog after each report it stores, then
 * "totals" with every report stored and the peak backlog: its callback twin counts the same reports.
 * LiveVehiclePosition logs a "skipped  | #k replaced before drawing" line for each fix it never drew, then
 * RESPONSE for the one it draws, and "totals" with every fix, drawn or replaced, as its callback twin counts.
 * OfflineMapDownload logs what waits for the writer after each place it saves, never more than three, then
 * "totals" with every place saved and the times the download paused.
 *
 * See docs/client-coroutines-design.md for the design of the coroutine level, and usecases/README.md for the
 * situation behind each use case.
 ************************/

int main(int argc, char** argv) {
  assert(rg_demo::OnApplicationThread());
  rg_demo::SetCoroutineLogPattern();
  const auto logger_Main = spdlog::stdout_color_mt("Main");
  spdlog::set_default_logger(logger_Main);

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  const auto stub = routeguide::RouteGuide::NewStub(channel);

  // Created on this thread, which runs EventLoop::Run() below, and before any coroutine starts. Every use
  // case's coroutine resumes through it, so it outlives them all.
  RpcReactor::coro::EventLoopExecutor executor;

  // The use case roster. The stub and the executor above must outlive every one of them.
  rg_demo::usecase::coro::WhatsHere whats_here{executor, *stub};
  rg_demo::usecase::coro::TurnByTurnGuidance turn_by_turn_guidance{executor, *stub};
  rg_demo::usecase::coro::TripRecorder trip_recorder{executor, *stub};
  rg_demo::usecase::coro::LiveVehiclePosition live_vehicle_position{executor, *stub};
  rg_demo::usecase::coro::OfflineMapDownload offline_map_download{executor, *stub};

  // A known place, Patriots Path, as route_guide_reactor_usecases asks about.
  const auto patriots_path = rg_utils::MakePoint(407838351, -746143763);
  // The route's stops, the features of the rectangle route_guide_reactor_usecases reads.
  const auto rect = rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);

  spdlog::info("-------------- Use case: what's here (GetFeature kPull, coroutine) --------------");
  whats_here.Start(patriots_path);
  spdlog::info("-------------- Use case: turn-by-turn guidance (ListFeatures kPush kTurnByTurn, coroutine) -------");
  turn_by_turn_guidance.Start(rect);
  spdlog::info("-------------- Use case: trip recorder (ListFeatures kPush kContinuous, coroutine) --------------");
  trip_recorder.Start(rect);
  spdlog::info("-------------- Use case: live vehicle position (ListFeatures kPull kContinuous, coroutine) ------");
  live_vehicle_position.Start(rect);
  spdlog::info("-------------- Use case: offline map download (ListFeatures kPush kPerMessage, coroutine) -------");
  offline_map_download.Start(rect);
  rg_demo::RunUntilDone();  // Scheduler component: processes queued events on this thread until every scenario ends
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
