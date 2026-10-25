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

#include "applications/reactor/demo_support.h"
#include "applications/reactor/usecases/live_vehicle_position.h"
#include "applications/reactor/usecases/trip_recorder.h"
#include "rg_service/rg_utils.h"
#include "rg_service/route_guide_service.h"

/************************
 * Use cases of the Active Object reactors
 *
 * The companion of route_guide_active_reactor_client, which runs one scenario per reactor variant and
 * nothing else. This application runs the use cases, under usecases/ and in rg_demo::usecase, each named
 * after a real-life situation rather than a variant, and each a clone of the variant it uses, along the
 * use only, so `diff variants/<variant>.cpp usecases/<use case>.cpp` is what the application adds. The
 * Active Object mapping, the event naming and the threading rules are those of the variants demo, and
 * its header comment describes them.
 *
 * - rg_demo::usecase::LiveVehiclePosition, ListFeatures under kPull and kContinuous: a map draws a vehicle's
 *   position from a live feed, and only the newest fix matters. read_ok keeps it in a LatestSlot, which
 *   the slow map takes. `diff variants/list_features_pull_continuous.cpp usecases/live_vehicle_position.cpp`
 *   is what an application adds to drop the stale fixes.
 * - rg_demo::usecase::TripRecorder, ListFeatures under kPush and kContinuous: a vehicle's black box stores
 *   every position report, and the feed must never wait for storage. `diff
 *   variants/list_features_push_continuous.cpp usecases/trip_recorder.cpp` is what an application adds to
 *   see the backlog it accepted.
 *
 * Reading the output: each logger is named after its use case, and every message is numbered per use
 * case, from the gRPC-thread callback ("read_ok  | #k") and, when it reaches the application thread, from
 * the handler ("RESPONSE | #k"). The thread id in the log pattern tells the two apart.
 * - LiveVehiclePosition logs RESPONSE lines only for the fixes its slow map drew, each the newest at the
 *   time, so their numbers skip. Its "totals   |" line counts the fixes drawn and replaced, which add up
 *   to the fixes received, and the last one drawn is always the last one received.
 * - TripRecorder ends each read_ok line with the backlog, logs a "done     |" line from the gRPC thread
 *   with the reports still queued when the RPC ends, and a "totals   |" line once the trip log has
 *   stored every one of them.
 *
 * See reactor_client.md for detailed pattern documentation, and usecases/README.md for the situation
 * behind each use case.
 ************************/

int main(int argc, char** argv) {
  assert(rg_demo::OnApplicationThread());
  spdlog::set_pattern("[%H:%M:%S.%f][%n][%t][%^%L%$] %v");
  auto logger_Main = spdlog::stdout_color_mt("Main");
  spdlog::set_default_logger(logger_Main);

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  const auto stub = routeguide::RouteGuide::NewStub(channel);

  // The use case roster. Each object registers its own event handlers here and holds them until it is
  // destroyed, so the stub above must outlive every one of them.
  rg_demo::usecase::LiveVehiclePosition live_vehicle_position{*stub};
  rg_demo::usecase::TripRecorder trip_recorder{*stub};

  // The feeds stand in for live vehicles: ListFeatures streams the 100 features of this rectangle, and
  // each use case reads them as successive positions.
  const auto rect = rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);

  // Every use case is started before the loop runs, so all of the RPCs are in flight together and their
  // lines interleave. Nothing below blocks.
  spdlog::info("-------------- Use case: live vehicle position (ListFeatures kPull kContinuous) --------------");
  live_vehicle_position.Start(rect);
  spdlog::info("-------------- Use case: trip recorder (ListFeatures kPush kContinuous) --------------");
  trip_recorder.Start(rect);
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
