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
 * - rg_demo::usecase::coro::WhatsHere, GetFeature under kPull through co::Unary: someone taps the map, and a
 *   card shows the name of the place there, or that nothing named is there.
 *
 * Reading the output: WhatsHere logs the answer, then the card, as its callback twin does, every line on the
 * application thread.
 *
 * See docs/client-coroutines-design.md for the design of the coroutine level, and usecases/README.md for the
 * situation behind each use case.
 ************************/

int main(int argc, char** argv) {
  assert(rg_demo::OnApplicationThread());
  spdlog::set_pattern("[%H:%M:%S.%f][%n][%t][%^%L%$] %v");
  auto logger_Main = spdlog::stdout_color_mt("Main");
  spdlog::set_default_logger(logger_Main);

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  const auto channel = grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials());
  const auto stub = routeguide::RouteGuide::NewStub(channel);

  // Created on this thread, which runs EventLoop::Run() below, and before any coroutine starts. Every use
  // case's coroutine resumes through it, so it outlives them all.
  RpcReactor::co::EventLoopExecutor executor;

  // The use case roster. The stub and the executor above must outlive every one of them.
  rg_demo::usecase::coro::WhatsHere whats_here{executor, *stub};

  // A known place, Patriots Path, as route_guide_reactor_usecases asks about.
  const auto patriots_path = rg_utils::MakePoint(407838351, -746143763);

  spdlog::info("-------------- Use case: what's here (GetFeature kPull, coroutine) --------------");
  whats_here.Start(patriots_path);
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
