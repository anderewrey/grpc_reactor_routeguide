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
#include "applications/reactor/variants/get_feature_pull_coro.h"
#include "applications/reactor/variants/list_features_pull_turn_by_turn_coro.h"
#include "applications/reactor/variants/list_features_push_turn_by_turn_coro.h"
#include "applications/reactor/demo_support.h"
#include "rg_service/rg_db.h"
#include "rg_service/rg_utils.h"
#include "rg_service/route_guide_service.h"

/************************
 * Coroutine variants of the Active Object reactors
 *
 * The coroutine twin of route_guide_active_reactor_client. It runs one scenario per awaitable of the
 * coroutine level, under variants/ with a _coro suffix and in a coro namespace, each next to the callback
 * variant whose reactor configuration it wraps, so `diff variants/<variant>.cpp variants/<variant>_coro.cpp`
 * is what consuming the reactor from a coroutine changes.
 *
 * Component mapping, against the callback variants:
 * - A scenario's Start() method = Proxy, which spawns the scenario's coroutine
 * - ClientReactor classes = Method Request, unchanged, inside an awaitable such as coro::Unary
 * - EventLoop = Scheduler + Activation Queue, unchanged: coro::EventLoopExecutor posts each coroutine to resume
 *   as an event, and its one handler resumes it on the application thread
 * - A scenario's coroutine = Servant, as one function instead of one handler per event
 * - The awaited Read() and Finish() = Future
 *
 * - rg_demo::pull::coro::GetFeature, GetFeature under kPull through coro::Unary: one request, and the coroutine
 *   resumes with the status and the response once OnDone() has run.
 * - rg_demo::pull::turn_by_turn::coro::ListFeatures, ListFeatures under kPull and kTurnByTurn through
 *   coro::Reader: each Read() swaps the next message out of the reactor, which arms the following read at once.
 * - rg_demo::push::turn_by_turn::coro::ListFeatures, ListFeatures under kPush and kTurnByTurn through
 *   coro::Reader: each Read() takes the message handed over, and only the next Read() arms the following read.
 *
 * Reading the output: each scenario logs "rpc started" when its coroutine has sent the request, then, once
 * resumed, its RESPONSE lines and "rpc ended". The thread id in the log pattern shows every line on the
 * application thread, where the callback variants also log gRPC-thread lines, such as read_ok: here no
 * scenario code runs there. A reader logs "stream ended after N messages", the total its callback parent
 * counts with its RESPONSE lines. Under kPush, each "resumed  | #k" line comes before RESPONSE #k+1, since
 * nothing is read until the coroutine asks; under kPull no such line exists, as the read is armed when the
 * message is taken.
 *
 * See docs/client-coroutines-design.md for the design of the coroutine level.
 ************************/

int main(int argc, char** argv) {
  assert(rg_demo::OnApplicationThread());
  rg_demo::SetCoroutineLogPattern();
  const auto logger_Main = spdlog::stdout_color_mt("Main");
  spdlog::set_default_logger(logger_Main);

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const auto feature_list = rg_db::GetInitialFeatures();
  const auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  const auto stub = routeguide::RouteGuide::NewStub(channel);

  // Created on this thread, which runs EventLoop::Run() below, and before any coroutine starts. Every
  // scenario's coroutine resumes through it, so it outlives them all.
  RpcReactor::coro::EventLoopExecutor executor;

  // The scenario roster. The stub and the executor above must outlive every one of them.
  rg_demo::pull::coro::GetFeature get_feature_pull{executor, *stub};
  rg_demo::pull::turn_by_turn::coro::ListFeatures list_features_pull_turn_by_turn{executor, *stub};
  rg_demo::push::turn_by_turn::coro::ListFeatures list_features_push_turn_by_turn{executor, *stub};

  // The same input as route_guide_active_reactor_client, so each run compares with its parent's.
  const auto rect = rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);
  const auto point = rg_utils::GetRandomPoint(feature_list);

  // Every scenario is started before the loop runs, so all of the RPCs are in flight together. Nothing below
  // blocks: each Start() returns at its coroutine's first suspension.
  spdlog::info("-------------- ListFeatures kPull kTurnByTurn, coroutine --------------");
  list_features_pull_turn_by_turn.Start(rect);
  spdlog::info("-------------- ListFeatures kPush kTurnByTurn, coroutine --------------");
  list_features_push_turn_by_turn.Start(rect);
  spdlog::info("-------------- GetFeature kPull, coroutine --------------");
  get_feature_pull.Start(point);
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
