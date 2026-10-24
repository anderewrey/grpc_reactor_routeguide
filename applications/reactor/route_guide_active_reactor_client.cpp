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
#include "applications/reactor/scenarios/list_features_pull_continuous.h"
#include "applications/reactor/scenarios/list_features_pull_per_message.h"
#include "applications/reactor/scenarios/list_features_pull_turn_by_turn.h"
#include "applications/reactor/scenarios/list_features_push_continuous.h"
#include "applications/reactor/scenarios/list_features_push_per_message.h"
#include "applications/reactor/scenarios/list_features_push_turn_by_turn.h"
#include "applications/reactor/scenarios/record_route_pull.h"
#include "applications/reactor/scenarios/record_route_push.h"
#include "applications/reactor/scenarios/route_chat_pull_continuous.h"
#include "applications/reactor/scenarios/route_chat_pull_turn_by_turn.h"
#include "applications/reactor/scenarios/route_chat_push_continuous.h"
#include "applications/reactor/scenarios/route_chat_push_turn_by_turn.h"
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
 * - Status() = Future, and GetResponse() for the Delivery::kPull scenarios whose reactor keeps a
 *   response for the application to pull: the unary and write ones, and the reading ones under a
 *   pacing that holds (kTurnByTurn, or kPerMessage when read_ok keeps the message). A kPull reader
 *   under kContinuous has nothing to pull, and the Delivery::kPush scenarios' reactors hand each
 *   message out as an owned object instead
 * - AddHold/RemoveHold = Guards, wherever a read is re-armed from the application thread
 *
 * Layout: one scenario per reactor variant, one file per scenario, under scenarios/. A scenario owns
 * everything one RPC needs on the application side, so a variant added to one RPC touches no other
 * RPC's file. The flavour of a variant is named by the filename and by the enclosing namespace,
 * never by the class, so a class is named after the RPC method it drives. A reading reactor has two
 * independent axes, the Delivery (kPull, kPush) and the ReadPacing (kContinuous, kTurnByTurn,
 * kPerMessage), so its namespace names both, delivery first:
 *
 * - rg_demo::pull::GetFeature                  unary, Delivery::kPull
 * - rg_demo::push::GetFeature                  unary, Delivery::kPush
 * - rg_demo::pull::continuous::ListFeatures    read, consumes each lent message inside read_ok, into
 *                                              an aggregate reported at OnDone
 * - rg_demo::pull::turn_by_turn::ListFeatures  read, every message held, then pulled by GetResponse()
 * - rg_demo::pull::per_message::ListFeatures   read, a filter: read_ok keeps named features, to be
 *                                              pulled, and declines unnamed ones
 * - rg_demo::push::continuous::ListFeatures    read, every message queued, the reactor reads on
 * - rg_demo::push::turn_by_turn::ListFeatures  read, every message queued and held until ResumeRead()
 * - rg_demo::push::per_message::ListFeatures   read, backpressure: read_ok holds once a few messages
 *                                              wait for the application
 * - rg_demo::pull::RecordRoute                 write, Delivery::kPull
 * - rg_demo::push::RecordRoute                 write, Delivery::kPush
 * - rg_demo::pull::continuous::RouteChat       bidi, as its ListFeatures namesake on the read side
 * - rg_demo::pull::turn_by_turn::RouteChat     bidi, as its ListFeatures namesake on the read side
 * - rg_demo::push::continuous::RouteChat       bidi, as its ListFeatures namesake on the read side
 * - rg_demo::push::turn_by_turn::RouteChat     bidi, as its ListFeatures namesake on the read side
 *
 * ListFeatures covers all six cells of the two axes. RouteChat covers only the four fixed-pacing
 * cells: the read side of ActiveBidiReactor behaves exactly as ActiveReadReactor does, so a
 * kPerMessage RouteChat would repeat what the ListFeatures per-message cells already show. RouteChat's
 * own lesson is the write side, an identical pump in every cell, running next to each delivery and
 * each fixed pacing of the read side.
 *
 * A flavoured family is cloned rather than parameterised, which is the teaching device: its members
 * are files with identical structure, each cloned from its nearest sibling, so a diff between two
 * neighbours is the shortest possible description of what one axis changes with the other held:
 * - pacing, under kPush: `diff list_features_push_continuous.cpp list_features_push_turn_by_turn.cpp`,
 *   then `diff list_features_push_turn_by_turn.cpp list_features_push_per_message.cpp`
 * - pacing, under kPull: `diff list_features_pull_turn_by_turn.cpp list_features_pull_continuous.cpp`,
 *   then `diff list_features_pull_turn_by_turn.cpp list_features_pull_per_message.cpp`
 * - delivery, at one pacing: `diff list_features_pull_turn_by_turn.cpp list_features_push_turn_by_turn.cpp`,
 *   and the same pairs among the route_chat_*.cpp files
 * The delivery diffs at kContinuous and kPerMessage are also legible, but each carries a change of
 * use along with the change of delivery: a lent message that is not held must be consumed in the
 * callback, and the per-message decision serves a filter under kPull but backpressure under kPush.
 * The members of a family are given identical input data, so the difference between their
 * transcripts is line order, plus what each cell deliberately does with the messages.
 *
 * Reading the output: every message delivered to the application thread is logged twice under one
 * per-scenario sequence number, once from the gRPC-thread callback ("read_ok  | #k") and once from
 * the application-thread handler ("RESPONSE | #k"), and the thread id in the log pattern tells them
 * apart. Each logger is named after its RPC, delivery and pacing, for example ListFeatures.pull.msg.
 * - A kTurnByTurn scenario adds a "resumed  | #k" line at its ResumeRead() call site (kPush) or a
 *   "pulled   | #k" line at its GetResponse() call site (kPull), and never emits read_ok #k+1 before
 *   it. The kPerMessage scenarios follow the same rule for the messages they hold.
 * - A kContinuous kPush scenario emits its read_ok lines in a burst ahead of the matching
 *   application-thread lines, and never emits a resumed or pulled line.
 * - A kContinuous kPull scenario emits only read_ok lines, each carrying the message it consumed on
 *   the gRPC thread, and no RESPONSE line; its OnDone handler then logs a "totals   |" line with the
 *   aggregate.
 * - The kPull kPerMessage scenario logs a declined message as "skipped  | #j", numbered on its own,
 *   from the gRPC thread, with no application-thread line after it; its read_ok, pulled and RESPONSE
 *   lines count kept messages only. It ends with a "totals   |" line of kept and skipped messages.
 * - The kPush kPerMessage scenario ends each read_ok line with the number of messages in flight and
 *   "held" or "not held". Only a held message gets a "resumed  | #k" line, from its own handler once
 *   the messages before it are handled, and the ordering rule above applies to it.
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
  rg_demo::pull::continuous::ListFeatures list_features_pull_continuous{*stub};
  rg_demo::pull::turn_by_turn::ListFeatures list_features_pull_turn_by_turn{*stub};
  rg_demo::pull::per_message::ListFeatures list_features_pull_per_message{*stub};
  rg_demo::push::continuous::ListFeatures list_features_push_continuous{*stub};
  rg_demo::push::turn_by_turn::ListFeatures list_features_push_turn_by_turn{*stub};
  rg_demo::push::per_message::ListFeatures list_features_push_per_message{*stub};
  rg_demo::pull::RecordRoute record_route_pull{*stub};
  rg_demo::push::RecordRoute record_route_push{*stub};
  rg_demo::pull::continuous::RouteChat route_chat_pull_continuous{*stub};
  rg_demo::pull::turn_by_turn::RouteChat route_chat_pull_turn_by_turn{*stub};
  rg_demo::push::continuous::RouteChat route_chat_push_continuous{*stub};
  rg_demo::push::turn_by_turn::RouteChat route_chat_push_turn_by_turn{*stub};

  // A flavoured family receives the same input, so its transcripts differ in the order their lines
  // interleave, and in what each cell deliberately does with the messages, not in what it is sent.
  // The rectangle holds 100 features, 36 of them unnamed, which is what the kPull kPerMessage filter
  // and the kPull kContinuous aggregate report on.
  const auto rect = rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);
  // The second note reuses the first note's location so the server echoes it back. The RouteChat
  // scenarios receive the same notes, but their response counts still diverge: the reference server
  // keeps one note history for the whole service rather than one per RPC, so each scenario also
  // echoes what its siblings deposited. Only the ListFeatures scenarios, which read immutable server
  // state, receive identical streams.
  const std::vector<routeguide::RouteNote> notes{rg_utils::MakeRouteNote("First message", 0, 0),
                                                 rg_utils::MakeRouteNote("Second message", 0, 0),
                                                 rg_utils::MakeRouteNote("Third message", 10000000, 0)};

  // Every scenario is started before the loop runs, so all of the RPCs are in flight together and
  // their lines interleave. Nothing below blocks.
  const auto point = rg_utils::GetRandomPoint(feature_list);
  const std::vector<routeguide::Point> route{rg_utils::GetRandomPoint(feature_list),
                                             rg_utils::GetRandomPoint(feature_list),
                                             rg_utils::GetRandomPoint(feature_list)};

  spdlog::info("-------------- ListFeatures kPull kContinuous --------------");
  list_features_pull_continuous.Start(rect);
  spdlog::info("-------------- ListFeatures kPull kTurnByTurn --------------");
  list_features_pull_turn_by_turn.Start(rect);
  spdlog::info("-------------- ListFeatures kPull kPerMessage --------------");
  list_features_pull_per_message.Start(rect);
  spdlog::info("-------------- ListFeatures kPush kContinuous --------------");
  list_features_push_continuous.Start(rect);
  spdlog::info("-------------- ListFeatures kPush kTurnByTurn --------------");
  list_features_push_turn_by_turn.Start(rect);
  spdlog::info("-------------- ListFeatures kPush kPerMessage --------------");
  list_features_push_per_message.Start(rect);
  spdlog::info("-------------- GetFeature kPull --------------");
  get_feature_pull.Start(point);
  spdlog::info("-------------- GetFeature kPush --------------");
  get_feature_push.Start(point);
  spdlog::info("-------------- RecordRoute kPull --------------");
  record_route_pull.Start(route);
  spdlog::info("-------------- RecordRoute kPush --------------");
  record_route_push.Start(route);
  spdlog::info("-------------- RouteChat kPull kContinuous --------------");
  route_chat_pull_continuous.Start(notes);
  spdlog::info("-------------- RouteChat kPull kTurnByTurn --------------");
  route_chat_pull_turn_by_turn.Start(notes);
  spdlog::info("-------------- RouteChat kPush kContinuous --------------");
  route_chat_push_continuous.Start(notes);
  spdlog::info("-------------- RouteChat kPush kTurnByTurn --------------");
  route_chat_push_turn_by_turn.Start(notes);
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
