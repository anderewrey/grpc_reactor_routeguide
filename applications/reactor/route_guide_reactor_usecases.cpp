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
#include "applications/reactor/usecases/offline_map_download.h"
#include "applications/reactor/usecases/place_search.h"
#include "applications/reactor/usecases/trip_recorder.h"
#include "applications/reactor/usecases/unnamed_places_audit.h"
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
 * - rg_demo::usecase::UnnamedPlacesAudit, ListFeatures under kPull and kContinuous: a map-quality tool
 *   counts the places with no name in a region. Each place is counted inside read_ok and forgotten.
 * - rg_demo::usecase::PlaceSearch, ListFeatures under kPull and kPerMessage: a search shows only the named
 *   places of the visible area. read_ok declines an unnamed place at no cost, and keeps a named one for
 *   the results to pull without a copy.
 * - rg_demo::usecase::OfflineMapDownload, ListFeatures under kPush and kPerMessage: a region's places are
 *   saved for offline use by a disk writer slower than the network. read_ok holds the stream once a few
 *   places wait for the writer, and the handler of the held place resumes it.
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
 * - UnnamedPlacesAudit logs only read_ok lines, from the gRPC thread, then a "totals   |" line with the
 *   places audited, unnamed and named.
 * - PlaceSearch logs an unnamed place as "skipped  | #j", numbered on its own, from the gRPC thread; its
 *   read_ok, pulled and RESPONSE lines count named places only, and a pulled #k line always comes before
 *   read_ok #k+1. It ends with a "totals   |" line of places shown and left out.
 * - OfflineMapDownload ends each read_ok line with the places waiting for the writer and "held" or "not
 *   held". Only a held place gets a "resumed  | #k" line, once the writer has saved it and every place
 *   before it, and read_ok #k+1 never comes before it. It ends with a "totals   |" line of places saved
 *   and of the times the download paused.
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
  rg_demo::usecase::UnnamedPlacesAudit unnamed_places_audit{*stub};
  rg_demo::usecase::PlaceSearch place_search{*stub};
  rg_demo::usecase::OfflineMapDownload offline_map_download{*stub};

  // ListFeatures streams the 100 features of this rectangle, 36 of them unnamed. The vehicle use cases
  // read them as successive positions, and the map use cases as the places of a region.
  const auto rect = rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);

  // Every use case is started before the loop runs, so all of the RPCs are in flight together and their
  // lines interleave. Nothing below blocks.
  spdlog::info("-------------- Use case: live vehicle position (ListFeatures kPull kContinuous) --------------");
  live_vehicle_position.Start(rect);
  spdlog::info("-------------- Use case: trip recorder (ListFeatures kPush kContinuous) --------------");
  trip_recorder.Start(rect);
  spdlog::info("-------------- Use case: unnamed places audit (ListFeatures kPull kContinuous) --------------");
  unnamed_places_audit.Start(rect);
  spdlog::info("-------------- Use case: place search (ListFeatures kPull kPerMessage) --------------");
  place_search.Start(rect);
  spdlog::info("-------------- Use case: offline map download (ListFeatures kPush kPerMessage) --------------");
  offline_map_download.Start(rect);
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
