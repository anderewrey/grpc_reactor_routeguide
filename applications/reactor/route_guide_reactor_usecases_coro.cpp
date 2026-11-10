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
#include <vector>

#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/usecases/gpx_track_upload_coro.h"
#include "applications/reactor/usecases/live_route_recording_coro.h"
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
 * - rg_demo::usecase::coro::GpxTrackUpload, RecordRoute under kPull and kReactorPulls through coro::Source: a
 *   recorded track is handed over for its summary, and the reactor pulls each point from it.
 * - rg_demo::usecase::coro::LiveRouteRecording, RecordRoute under kPush and kApplicationPushes through
 *   coro::Writer: the coroutine awaits each GPS fix, then its write, and hands the summary to the trip history.
 *
 * Reading the output: WhatsHere logs the answer, then the card, as its callback twin does. TurnByTurnGuidance
 * logs a "resumed  | #k driver confirmed" line once the driver confirms stop #k, and RESPONSE #k+1 never comes
 * before it, then "stream ended after N messages", the number of stops its callback twin shows.
 * GpxTrackUpload logs the summary of every point of the track. LiveRouteRecording logs each GPS fix as
 * "gps      |", sent once it came, or after waiting for a write in flight, as the tunnel's fixes do, then the
 * summary of every fix, as its callback twin does. Every line is on the application thread.
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
  rg_demo::usecase::coro::GpxTrackUpload gpx_track_upload{executor, *stub};
  rg_demo::usecase::coro::LiveRouteRecording live_route_recording{executor, *stub};

  // A known place, Patriots Path, as route_guide_reactor_usecases asks about.
  const auto patriots_path = rg_utils::MakePoint(407838351, -746143763);
  const auto new_jersey_10 = rg_utils::MakePoint(408122808, -743999179);
  // The track and the drive route_guide_reactor_usecases sends.
  const std::vector<routeguide::Point> gpx_track{patriots_path, rg_utils::MakePoint(407900000, -745600000),
                                                 rg_utils::MakePoint(408000000, -745000000), new_jersey_10};
  const std::vector<routeguide::Point> drive{new_jersey_10,
                                             rg_utils::MakePoint(408050000, -744400000),
                                             rg_utils::MakePoint(408000000, -744800000),
                                             rg_utils::MakePoint(407950000, -745200000),
                                             rg_utils::MakePoint(407900000, -745500000),
                                             rg_utils::MakePoint(407870000, -745800000),
                                             rg_utils::MakePoint(407850000, -746000000),
                                             patriots_path};
  // The route's stops, the features of the rectangle route_guide_reactor_usecases reads.
  const auto rect = rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);

  spdlog::info("-------------- Use case: what's here (GetFeature kPull, coroutine) --------------");
  whats_here.Start(patriots_path);
  spdlog::info("-------------- Use case: turn-by-turn guidance (ListFeatures kPush kTurnByTurn, coroutine) -------");
  turn_by_turn_guidance.Start(rect);
  spdlog::info("-------------- Use case: GPX track upload (RecordRoute kPull kReactorPulls, coroutine) --------------");
  gpx_track_upload.Start(gpx_track);
  spdlog::info("-------------- Use case: live route recording (RecordRoute kPush kApplicationPushes, coroutine) ----");
  live_route_recording.Start(drive);
  rg_demo::RunUntilDone();  // Scheduler component: processes queued events on this thread until every scenario ends
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
