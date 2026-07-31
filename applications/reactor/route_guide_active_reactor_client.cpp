///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024 anderewrey
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

#include "applications/reactor/scenarios/get_feature_pull.h"
#include "applications/reactor/scenarios/get_feature_push.h"
#include "applications/reactor/scenarios/list_features_continuous.h"
#include "applications/reactor/scenarios/list_features_pull.h"
#include "applications/reactor/scenarios/list_features_turn_by_turn.h"
#include "applications/reactor/scenarios/record_route_pull.h"
#include "applications/reactor/scenarios/record_route_push.h"
#include "applications/reactor/scenarios/route_chat_continuous.h"
#include "applications/reactor/scenarios/route_chat_pull.h"
#include "applications/reactor/scenarios/route_chat_turn_by_turn.h"
#include "applications/reactor/scenarios/support.h"
#include "rg_service/rg_db.h"
#include "rg_service/rg_utils.h"
#include "rg_service/route_guide_service.h"

/************************
 * Active Object Pattern Implementation
 *
 * This application demonstrates the Active Object pattern combined with the Reactor pattern for
 * event-driven asynchronous RPC handling.
 *
 * Component mapping:
 * - A scenario's Start() method = Proxy
 * - ClientReactor classes (ActiveUnaryReactor, ActiveReadReactor, ActiveWriteReactor,
 *   ActiveBidiReactor) = Method Request
 * - EventLoop = Scheduler + Activation Queue
 * - A scenario's application-thread handlers = Servant (business logic placeholder)
 * - Status() = Future, and GetResponse() for the Delivery::kPull scenarios, whose reactors keep each
 *   response for the application to pull. The Delivery::kPush scenarios' reactors hand each message
 *   out as an owned object instead
 * - AddHold/RemoveHold = Guards, wherever a read is re-armed from the application thread
 *
 * Layout: one scenario per reactor variant, one file per scenario, under scenarios/. A scenario owns
 * everything one RPC needs on the application side, so a variant added to one RPC touches no other
 * RPC's file. The flavour of a variant is named by the filename and by the enclosing namespace,
 * never by the class, so a class is named after the RPC method it drives:
 *
 * - rg_demo::pull::GetFeature           unary, Delivery::kPull
 * - rg_demo::push::GetFeature           unary, Delivery::kPush
 * - rg_demo::pull::ListFeatures         read, Delivery::kPull, paced by the read_ok return value
 * - rg_demo::continuous::ListFeatures   read, Delivery::kPush with ReadPacing::kContinuous
 * - rg_demo::turn_by_turn::ListFeatures read, Delivery::kPush with ReadPacing::kTurnByTurn
 * - rg_demo::pull::RecordRoute          write, Delivery::kPull
 * - rg_demo::push::RecordRoute          write, Delivery::kPush
 * - rg_demo::pull::RouteChat            bidi, Delivery::kPull, paced by the read_ok return value
 * - rg_demo::continuous::RouteChat      bidi, Delivery::kPush with ReadPacing::kContinuous
 * - rg_demo::turn_by_turn::RouteChat    bidi, Delivery::kPush with ReadPacing::kTurnByTurn
 *
 * A flavoured family is cloned rather than parameterised, which is the teaching device: its members
 * are files with identical structure, so `diff list_features_continuous.cpp
 * list_features_turn_by_turn.cpp` is the shortest possible description of what the pacing mode
 * changes, and `diff list_features_pull.cpp list_features_turn_by_turn.cpp` of what the delivery
 * approach changes at the same pacing. The members of a family are given identical input data, so
 * the difference between their transcripts is line order.
 *
 * Reading the output: every delivered message is logged twice under one per-scenario sequence
 * number, once from the gRPC-thread callback ("read_ok  | #k") and once from the application-thread
 * handler ("RESPONSE | #k"), and the thread id in the log pattern tells them apart. A kTurnByTurn
 * scenario adds a "resumed  | #k" line at its ResumeRead() call site, and never emits read_ok #k+1
 * before it. A pull scenario adds a "pulled   | #k" line at its GetResponse() call site, under the same
 * rule. A kContinuous scenario emits its read_ok lines in a burst ahead of the matching
 * application-thread lines, and never emits a resumed or pulled line.
 *
 * Note: This demo uses simple logging as Servant logic. Production applications would implement
 * actual business logic in the event handlers.
 *
 * See reactor_client.md for detailed pattern documentation.
 ************************/

int main(int argc, char** argv) {
  assert(rg_demo::OnApplicationThread());
  spdlog::set_pattern("[%H:%M:%S.%f][%n][%t][%^%L%$] %v");
  auto logger_Main = spdlog::stdout_color_mt("Main");
  spdlog::set_default_logger(logger_Main);

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const auto feature_list = rg_db::GetInitialFeatures();
  const auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  const auto stub = routeguide::RouteGuide::NewStub(channel);

  // The scenario roster. Each object registers its own event handlers here and holds them until it
  // is destroyed, so the stub above must outlive every one of them.
  rg_demo::pull::GetFeature get_feature_pull{*stub};
  rg_demo::push::GetFeature get_feature_push{*stub};
  rg_demo::pull::ListFeatures list_features_pull{*stub};
  rg_demo::continuous::ListFeatures list_features_continuous{*stub};
  rg_demo::turn_by_turn::ListFeatures list_features_turn_by_turn{*stub};
  rg_demo::pull::RecordRoute record_route_pull{*stub};
  rg_demo::push::RecordRoute record_route_push{*stub};
  rg_demo::pull::RouteChat route_chat_pull{*stub};
  rg_demo::continuous::RouteChat route_chat_continuous{*stub};
  rg_demo::turn_by_turn::RouteChat route_chat_turn_by_turn{*stub};

  // A flavoured family receives the same input, so its transcripts differ only in the order their
  // lines interleave, not in what they report.
  const auto rect = rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);
  // The second note reuses the first note's location so the server echoes it back. The RouteChat
  // scenarios receive the same notes, but their response counts still diverge: the reference server
  // keeps one note history for the whole service rather than one per RPC, so each scenario also
  // echoes what its counterpart deposited. Only the ListFeatures scenarios, which read immutable
  // server state, produce transcripts that match line for line.
  const std::vector<routeguide::RouteNote> notes{rg_utils::MakeRouteNote("First message", 0, 0),
                                                 rg_utils::MakeRouteNote("Second message", 0, 0),
                                                 rg_utils::MakeRouteNote("Third message", 10000000, 0)};

  // Every scenario is started before the loop runs, so all of the RPCs are in flight together and
  // their lines interleave. Nothing below blocks.
  const auto point = rg_utils::GetRandomPoint(feature_list);
  const std::vector<routeguide::Point> route{rg_utils::GetRandomPoint(feature_list),
                                             rg_utils::GetRandomPoint(feature_list),
                                             rg_utils::GetRandomPoint(feature_list)};

  spdlog::info("-------------- ListFeatures kPull --------------");
  list_features_pull.Start(rect);
  spdlog::info("-------------- ListFeatures kPush kContinuous --------------");
  list_features_continuous.Start(rect);
  spdlog::info("-------------- ListFeatures kPush kTurnByTurn --------------");
  list_features_turn_by_turn.Start(rect);
  spdlog::info("-------------- GetFeature kPull --------------");
  get_feature_pull.Start(point);
  spdlog::info("-------------- GetFeature kPush --------------");
  get_feature_push.Start(point);
  spdlog::info("-------------- RecordRoute kPull --------------");
  record_route_pull.Start(route);
  spdlog::info("-------------- RecordRoute kPush --------------");
  record_route_push.Start(route);
  spdlog::info("-------------- RouteChat kPull --------------");
  route_chat_pull.Start(notes);
  spdlog::info("-------------- RouteChat kPush kContinuous --------------");
  route_chat_continuous.Start(notes);
  spdlog::info("-------------- RouteChat kPush kTurnByTurn --------------");
  route_chat_turn_by_turn.Start(notes);
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
