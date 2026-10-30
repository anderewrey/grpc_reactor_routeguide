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
#include <cstddef>
#include <memory>
#include <vector>

#include "applications/reactor/demo_support.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/usecases/convoy_chat.h"
#include "applications/reactor/usecases/gpx_track_upload.h"
#include "applications/reactor/usecases/guided_tour_briefing.h"
#include "applications/reactor/usecases/live_route_recording.h"
#include "applications/reactor/usecases/live_vehicle_position.h"
#include "applications/reactor/usecases/offline_map_download.h"
#include "applications/reactor/usecases/place_search.h"
#include "applications/reactor/usecases/trip_recorder.h"
#include "applications/reactor/usecases/turn_by_turn_guidance.h"
#include "applications/reactor/usecases/unnamed_places_audit.h"
#include "applications/reactor/usecases/whats_here.h"
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
 * - rg_demo::usecase::TurnByTurnGuidance, ListFeatures under kPush and kTurnByTurn: a guidance screen
 *   shows one stop at a time, and the driver's confirmation, a later event, resumes the stream.
 * - rg_demo::usecase::GpxTrackUpload, RecordRoute under kPull and kReactorPulls: a recorded track is
 *   uploaded for its summary, and the reactor pulls each point from the prepared track.
 * - rg_demo::usecase::LiveRouteRecording, RecordRoute under kPush and kApplicationPushes: each point is
 *   sent as the GPS produces it, and the summary goes to the trip history, owned.
 * - rg_demo::usecase::ConvoyChat, RouteChat under kPush, kApplicationPushes and kContinuous: drivers send
 *   notes as they type them, and every note received is shown, without the chat ever waiting.
 * - rg_demo::usecase::GuidedTourBriefing, RouteChat under kPull, kReactorPulls and kPerMessage: a scripted
 *   tour is pulled note by note, and read_ok declines the replies already seen.
 * - rg_demo::usecase::WhatsHere, GetFeature under kPull: a tap on the map asks what is there.
 *
 * Things that appear over time, GPS fixes, a driver's confirmation, a typed note, are timed EventLoop
 * events, delivered on the application thread: nothing sleeps to wait for them. The reference server
 * keeps one RouteChat note history for the whole service, and echoes the notes left at a location to
 * whoever leaves a new note there. The tour's stone bridge is where the convoy reports debris, so the
 * convoy receives the tour's note within one run, and a later run against the same server hands the
 * tour the convoy's notes too: the chat transcripts vary with what ran before.
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
 * - TurnByTurnGuidance logs a "resumed  | #k driver confirmed" line once the driver confirms stop #k,
 *   and read_ok #k+1 never comes before it.
 * - GpxTrackUpload logs each point the reactor pulls as "next     | #k", then the summary.
 * - LiveRouteRecording logs each GPS fix as "gps      |", sent at once or queued behind a write in
 *   flight, then the summary and the trip history.
 * - ConvoyChat logs each typed note as "typed    |", sent at once or queued behind a write in flight,
 *   and ends with a "totals   |" line of notes typed and shown.
 * - GuidedTourBriefing logs each pulled script note as "next     | #k", a repeated reply as
 *   "skipped  | #j", and ends with a "totals   |" line of replies shown and repeats skipped.
 * - WhatsHere logs the answer, then the card: the name of the place, or that nothing named is there.
 *
 * See reactor_client.md for detailed pattern documentation, and usecases/README.md for the situation
 * behind each use case.
 ************************/

namespace {
// The event that starts the second wave of use cases, and when, in milliseconds after the loop starts.
constexpr auto kSecondWave{"UseCasesSecondWave"};
constexpr std::size_t kSecondWaveMs{1000};
}  // anonymous namespace

int main(int argc, char** argv) {
  assert(rg_demo::OnApplicationThread());
  spdlog::set_pattern("[%H:%M:%S.%f][%n][%t][%^%L%$] %v");
  const auto logger_Main = spdlog::stdout_color_mt("Main");
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
  rg_demo::usecase::TurnByTurnGuidance turn_by_turn_guidance{*stub};
  rg_demo::usecase::GpxTrackUpload gpx_track_upload{*stub};
  rg_demo::usecase::LiveRouteRecording live_route_recording{*stub};
  rg_demo::usecase::ConvoyChat convoy_chat{*stub};
  rg_demo::usecase::GuidedTourBriefing guided_tour_briefing{*stub};
  rg_demo::usecase::WhatsHere whats_here{*stub};

  // ListFeatures streams the 100 features of this rectangle, 36 of them unnamed. The vehicle use cases
  // read them as successive positions, and the map use cases as the places of a region.
  const auto rect = rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);
  // Two known places, Patriots Path and 101 New Jersey 10, and points between them that are none.
  const auto patriots_path = rg_utils::MakePoint(407838351, -746143763);
  const auto new_jersey_10 = rg_utils::MakePoint(408122808, -743999179);
  // A prepared track stands in for a parsed GPX file.
  const std::vector<routeguide::Point> gpx_track{patriots_path, rg_utils::MakePoint(407900000, -745600000),
                                                 rg_utils::MakePoint(408000000, -745000000), new_jersey_10};
  // The points the GPS produces while driving, one fix each.
  const std::vector<routeguide::Point> drive{new_jersey_10,
                                             rg_utils::MakePoint(408050000, -744400000),
                                             rg_utils::MakePoint(408000000, -744800000),
                                             rg_utils::MakePoint(407950000, -745200000),
                                             rg_utils::MakePoint(407900000, -745500000),
                                             rg_utils::MakePoint(407870000, -745800000),
                                             rg_utils::MakePoint(407850000, -746000000),
                                             patriots_path};
  // The notes the convoy's drivers type. Three are left at the same place, so the server echoes the
  // earlier ones back each time.
  const std::vector<routeguide::RouteNote> convoy_notes{
      rg_utils::MakeRouteNote("Convoy leaving the depot", 409000000, -744000000),
      rg_utils::MakeRouteNote("Debris on the right lane", 409100000, -744100000),
      rg_utils::MakeRouteNote("Copy, slowing down", 409100000, -744100000),
      rg_utils::MakeRouteNote("Debris cleared", 409100000, -744100000)};
  // The tour's script. It visits the mill twice, so the mill's second answer repeats its first one. Its
  // stone bridge is where the convoy later reports debris, so the convoy receives the tour's note there.
  const std::vector<routeguide::RouteNote> tour_script{
      rg_utils::MakeRouteNote("Welcome to the old mill", 410000000, -745000000),
      rg_utils::MakeRouteNote("The mill wheel dates from 1820", 410000000, -745000000),
      rg_utils::MakeRouteNote("Next stop: the stone bridge", 409100000, -744100000),
      rg_utils::MakeRouteNote("Back at the mill for lunch", 410000000, -745000000)};

  // The use cases that do not wait on time are started before the loop runs, so their RPCs are in flight
  // together and their lines interleave. Nothing below blocks.
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
  spdlog::info("-------------- Use case: GPX track upload (RecordRoute kPull kReactorPulls) --------------");
  gpx_track_upload.Start(gpx_track);
  spdlog::info("-------------- Use case: guided tour briefing (RouteChat kPull kReactorPulls kPerMessage) ----------");
  guided_tour_briefing.Start(tour_script);
  spdlog::info("-------------- Use case: what's here (GetFeature kPull) --------------");
  whats_here.Start(patriots_path);

  // The use cases that wait on time, for a driver, a GPS or people typing, start in a second wave. Their
  // timers fire on the application thread, which the use cases above keep busy for a few hundred
  // milliseconds with their slow handlers, so started at once, their events would all fire late, and
  // together. A timed event of its own starts them once that thread is free.
  const RpcReactor::EventConnection second_wave{kSecondWave, [&](const EventLoop::Event*) {
    spdlog::info("-------------- Use case: turn-by-turn guidance (ListFeatures kPush kTurnByTurn) --------------");
    turn_by_turn_guidance.Start(rect);
    spdlog::info("-------------- Use case: live route recording (RecordRoute kPush kApplicationPushes) ------------");
    live_route_recording.Start(drive);
    spdlog::info("-------------- Use case: convoy chat (RouteChat kPush kApplicationPushes kContinuous) -----------");
    convoy_chat.Start(convoy_notes);
  }};
  EventLoop::TriggerEvent(kSecondWave, kSecondWaveMs);
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
