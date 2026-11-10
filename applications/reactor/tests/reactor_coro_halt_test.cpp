///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests the teardown rule of the coroutine level against the real EventLoop: with one coroutine of every
/// awaitable kind suspended while its RPC is in flight, EventLoop::Halt() stops the loop, so none of them will
/// resume, and destroying their frames then cancels every RPC and destroys every reactor only once its OnDone()
/// has run, in time, and without freeing anything gRPC still uses, which AddressSanitizer checks.
///
/// EventLoop is a process-wide singleton that cannot restart after Halt(), so this file holds one test, in a
/// process of its own.

#include <EventLoop.h>
#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"
#include "rg_service/rg_utils.h"

namespace {

using RpcReactor::EventConnection;
using RpcReactor::Client::Delivery;
using RpcReactor::Client::ReadPacing;
using RpcReactor::Client::Submission;
using RpcReactor::coro::EventLoopExecutor;
using RpcReactor::coro::RootTasks;
using RpcReactor::coro::Task;

constexpr auto kTimeout = std::chrono::seconds(10);
// Far longer than the test: a wait that only the frame's destruction ends.
constexpr auto kForever = std::chrono::milliseconds(60'000);

/// Holds every RPC open, answering nothing, until the client cancels it, then finishes it CANCELLED. Reads
/// whatever the client sends meanwhile.
class HoldService final : public routeguide::RouteGuide::CallbackService {
 public:
  /// How many RPCs started, and how many the client cancelled.
  [[nodiscard]] int Started() const { return started_.load(); }
  [[nodiscard]] int Cancelled() const { return cancelled_.load(); }

  grpc::ServerUnaryReactor* GetFeature(grpc::CallbackServerContext* /*context*/, const routeguide::Point* /*point*/,
                                       routeguide::Feature* /*feature*/) override {
    class Reactor : public grpc::ServerUnaryReactor {
     public:
      explicit Reactor(HoldService* service) : service_(service) { ++service_->started_; }
      void OnCancel() override {
        ++service_->cancelled_;
        Finish(grpc::Status::CANCELLED);
      }
      void OnDone() override { delete this; }

     private:
      HoldService* service_;
    };
    return new Reactor(this);
  }

  grpc::ServerWriteReactor<routeguide::Feature>* ListFeatures(grpc::CallbackServerContext* /*context*/,
                                                              const routeguide::Rectangle* /*rect*/) override {
    class Reactor : public grpc::ServerWriteReactor<routeguide::Feature> {
     public:
      explicit Reactor(HoldService* service) : service_(service) { ++service_->started_; }
      void OnCancel() override {
        ++service_->cancelled_;
        Finish(grpc::Status::CANCELLED);
      }
      void OnDone() override { delete this; }

     private:
      HoldService* service_;
    };
    return new Reactor(this);
  }

  grpc::ServerReadReactor<routeguide::Point>* RecordRoute(grpc::CallbackServerContext* /*context*/,
                                                          routeguide::RouteSummary* /*summary*/) override {
    class Reactor : public grpc::ServerReadReactor<routeguide::Point> {
     public:
      explicit Reactor(HoldService* service) : service_(service) {
        ++service_->started_;
        StartRead(&point_);
      }
      void OnReadDone(bool ok) override {
        if (ok) StartRead(&point_);  // the end of the client's stream changes nothing: the RPC stays held
      }
      void OnCancel() override {
        ++service_->cancelled_;
        Finish(grpc::Status::CANCELLED);
      }
      void OnDone() override { delete this; }

     private:
      HoldService* service_;
      routeguide::Point point_;
    };
    return new Reactor(this);
  }

  grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote>* RouteChat(
      grpc::CallbackServerContext* /*context*/) override {
    class Reactor : public grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote> {
     public:
      explicit Reactor(HoldService* service) : service_(service) {
        ++service_->started_;
        StartRead(&note_);
      }
      void OnReadDone(bool ok) override {
        if (ok) StartRead(&note_);
      }
      void OnCancel() override {
        ++service_->cancelled_;
        Finish(grpc::Status::CANCELLED);
      }
      void OnDone() override { delete this; }

     private:
      HoldService* service_;
      routeguide::RouteNote note_;
    };
    return new Reactor(this);
  }

 private:
  std::atomic<int> started_{0};
  std::atomic<int> cancelled_{0};
};

using Stub = routeguide::RouteGuide::Stub;

/// Awaits a unary RPC the server holds.
Task<> AwaitUnary(EventLoopExecutor* executor, Stub* stub, std::promise<void>* waiting) {
  RpcReactor::coro::Unary<routeguide::GetFeature::ClientReactor<Delivery::kPull>> call{
      *executor, *stub, rg_utils::MakePoint(1, 1)};
  waiting->set_value();
  static_cast<void>(co_await call.Finish());
  ADD_FAILURE() << "a halted loop resumes nothing";
}

/// Awaits a message the server never sends.
Task<> AwaitRead(EventLoopExecutor* executor, Stub* stub, std::promise<void>* waiting) {
  RpcReactor::coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kTurnByTurn>>
      reader{*executor, *stub, rg_utils::MakeRectangle(0, 0, 1, 1)};
  waiting->set_value();
  static_cast<void>(co_await reader.Read());
  ADD_FAILURE() << "a halted loop resumes nothing";
}

/// Writes one point, then waits with its write flow open.
Task<> AwaitBetweenWrites(EventLoopExecutor* executor, Stub* stub, std::promise<void>* waiting) {
  RpcReactor::coro::Writer<routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kApplicationPushes>>
      writer{*executor, *stub};
  static_cast<void>(co_await writer.Write(rg_utils::MakePoint(1, 1)));
  waiting->set_value();
  co_await EventLoopExecutor::After(kForever);
  ADD_FAILURE() << "a halted loop resumes nothing";
}

/// Uploads a few points, then awaits the summary the server holds.
Task<> AwaitUpload(EventLoopExecutor* executor, Stub* stub, std::promise<void>* waiting) {
  RpcReactor::coro::Source<routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>> upload{
      *executor, *stub, std::vector<routeguide::Point>{rg_utils::MakePoint(1, 1), rg_utils::MakePoint(2, 2)}};
  waiting->set_value();
  static_cast<void>(co_await upload.Finish());
  ADD_FAILURE() << "a halted loop resumes nothing";
}

using Chat = RpcReactor::coro::Bidi<
    routeguide::RouteChat::ClientReactor<Delivery::kPush, Submission::kApplicationPushes, ReadPacing::kContinuous>>;

Task<> ReadNothing(Chat* chat) {
  static_cast<void>(co_await chat->Read());
  ADD_FAILURE() << "a halted loop resumes nothing";
}

Task<> WriteThenWait(Chat* chat, std::promise<void>* waiting) {
  static_cast<void>(co_await chat->Write(rg_utils::MakeRouteNote("held", 1, 1)));
  waiting->set_value();
  co_await EventLoopExecutor::After(kForever);
  ADD_FAILURE() << "a halted loop resumes nothing";
}

/// A reading flow waiting for a reply the server never sends, and a writing flow waiting with its write flow open,
/// joined.
Task<> AwaitChat(EventLoopExecutor* executor, Stub* stub, std::promise<void>* waiting) {
  Chat chat{*executor, *stub};
  static_cast<void>(co_await RpcReactor::coro::WhenAll(ReadNothing(&chat), WriteThenWait(&chat, waiting)));
  ADD_FAILURE() << "a halted loop resumes nothing";
}

/// Runs a function on the EventLoop thread, and waits for it.
template <class F>
void OnLoop(F function) {
  static constexpr const char* kRun = "test::run";
  std::promise<void> ran;
  const EventConnection connection{kRun, [&](const EventLoop::Event*) {
                                     function();
                                     ran.set_value();
                                   }};
  EventLoop::TriggerEvent(kRun);
  ASSERT_EQ(ran.get_future().wait_for(kTimeout), std::future_status::ready);
}

class ReactorCoroHaltTest : public RouteGuideTestFixtureBase<HoldService> {};

/// @test Halting the loop with a coroutine of every awaitable kind suspended mid-RPC, then destroying their frames
/// off the loop, as an application's main() does after EventLoop::Run() returns: every RPC is cancelled, every
/// reactor is destroyed once its OnDone() has run, well under the 5 seconds after which one would be leaked, and
/// nothing gRPC still uses is freed, which AddressSanitizer checks. None of the coroutines resumes.
TEST_F(ReactorCoroHaltTest, EveryAwaitableInFlight_HaltThenDestroy_ReleasesEveryReactorAfterOnDone) {
  constexpr int kRpcs = 5;
  EventLoop::SetMode(EventLoop::Mode::NON_BLOCK);
  EventLoop::Run();
  std::thread::id app_thread;
  OnLoop([&] { app_thread = std::this_thread::get_id(); });
  auto executor = std::make_unique<EventLoopExecutor>(app_thread);
  auto roots = std::make_unique<RootTasks>();

  std::promise<void> unary;
  std::promise<void> reader;
  std::promise<void> writer;
  std::promise<void> source;
  std::promise<void> chat;
  OnLoop([&] {
    roots->Spawn(AwaitUnary(executor.get(), stub_.get(), &unary));
    roots->Spawn(AwaitRead(executor.get(), stub_.get(), &reader));
    roots->Spawn(AwaitBetweenWrites(executor.get(), stub_.get(), &writer));
    roots->Spawn(AwaitUpload(executor.get(), stub_.get(), &source));
    roots->Spawn(AwaitChat(executor.get(), stub_.get(), &chat));
  });
  for (auto* waiting : {&unary, &reader, &writer, &source, &chat}) {
    ASSERT_EQ(waiting->get_future().wait_for(kTimeout), std::future_status::ready);
  }
  // Every RPC has reached the server, which holds it.
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (test_service_.Started() < kRpcs && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  ASSERT_EQ(test_service_.Started(), kRpcs);
  EXPECT_EQ(roots->Size(), static_cast<std::size_t>(kRpcs));

  // The loop stops: the timers the writers wait on, and any resumption still to come, are discarded.
  EventLoop::Halt();

  // On this thread, as main() does once EventLoop::Run() has returned.
  const auto start = std::chrono::steady_clock::now();
  roots.reset();
  const auto elapsed = std::chrono::steady_clock::now() - start;
  executor.reset();

  // Well under the 5 seconds DestroyAfterOnDone() waits for each OnDone() before it leaks the reactor.
  EXPECT_LT(elapsed, std::chrono::seconds(3));
  const auto cancelled_by = std::chrono::steady_clock::now() + kTimeout;
  while (test_service_.Cancelled() < kRpcs && std::chrono::steady_clock::now() < cancelled_by) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  EXPECT_EQ(test_service_.Cancelled(), kRpcs);
}

}  // namespace
