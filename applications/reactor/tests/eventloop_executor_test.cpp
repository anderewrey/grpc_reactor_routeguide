///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests EventLoopExecutor, the executor of the applications in reactor_eventloop.h, against the real
/// EventLoop: every resumption runs on the thread that runs the EventLoop handlers, in the order the handles
/// were posted, after every event queued before them.
///
/// EventLoop is a process-wide singleton that cannot restart after Halt(), and the executor is one per
/// process, so the environment below starts both once for every test.

#include <EventLoop.h>
#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_eventloop.h"

namespace {

using RpcReactor::EventConnection;
using RpcReactor::coro::EventLoopExecutor;
using RpcReactor::coro::Executor;
using RpcReactor::coro::RootTasks;
using RpcReactor::coro::Task;

constexpr auto kTimeout = std::chrono::seconds(10);

/// Runs a function on the EventLoop thread, the tests' application thread, and waits for it.
template <class F>
auto OnLoop(F function) {
  static constexpr const char* kRun = "test::run";
  using Result = decltype(function());
  std::promise<Result> result;
  const EventConnection connection{kRun, [&](const EventLoop::Event*) {
                                     if constexpr (std::is_void_v<Result>) {
                                       function();
                                       result.set_value();
                                     } else {
                                       result.set_value(function());
                                     }
                                   }};
  EventLoop::TriggerEvent(kRun);
  auto future = result.get_future();
  EXPECT_EQ(future.wait_for(kTimeout), std::future_status::ready);
  return future.get();
}

/// Starts EventLoop on a thread of its own, then the executor, told which thread that is.
class EventLoopEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    EventLoop::SetMode(EventLoop::Mode::NON_BLOCK);
    EventLoop::Run();
    app_thread = OnLoop([] { return std::this_thread::get_id(); });
    executor = std::make_unique<EventLoopExecutor>(app_thread);
  }
  void TearDown() override {
    EventLoop::Halt();
    executor.reset();
  }

  static inline std::thread::id app_thread;
  static inline std::unique_ptr<EventLoopExecutor> executor;
};

/// Completes on a thread of its own, as a gRPC callback does, which posts the handle to the executor.
class CompleteOnAnotherThread {
 public:
  explicit CompleteOnAnotherThread(std::vector<std::thread>* threads) : threads_(threads) {}
  static bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) const {
    threads_->emplace_back([handle] { EventLoopEnvironment::executor->Post(handle); });
  }
  static std::thread::id await_resume() noexcept { return std::this_thread::get_id(); }

 private:
  std::vector<std::thread>* threads_;
};

Task<> Hop(std::vector<std::thread>* threads, std::vector<std::thread::id>* resumed_on, std::promise<void>* done) {
  constexpr int kHops = 100;
  for (int i = 0; i < kHops; ++i) resumed_on->push_back(co_await CompleteOnAnotherThread(threads));
  done->set_value();
}

TEST(CoroEventLoopTest, Executor_ResumptionsFromOtherThreads_AllRunOnTheApplicationThread) {
  RootTasks roots;
  std::vector<std::thread> threads;
  std::vector<std::thread::id> resumed_on;
  std::promise<void> done;

  OnLoop([&] { roots.Spawn(Hop(&threads, &resumed_on, &done)); });
  ASSERT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
  EXPECT_EQ(OnLoop([&] { return roots.Size(); }), 0U);
  for (auto& thread : threads) thread.join();

  ASSERT_EQ(resumed_on.size(), 100U);
  for (const auto& id : resumed_on) EXPECT_EQ(id, EventLoopEnvironment::app_thread);
  EXPECT_NE(EventLoopEnvironment::app_thread, std::this_thread::get_id());
}

/// Suspends, hands out its handle, and logs its name once resumed.
// A coroutine takes its arguments by value: a reference would dangle once it suspends.
// cppcheck-suppress passedByValue
Task<> LogWhenResumed(std::string name, std::coroutine_handle<>* handle, std::vector<std::string>* log) {
  struct Suspend {
    std::coroutine_handle<>* out;
    static bool await_ready() noexcept { return false; }
    void await_suspend(std::coroutine_handle<> h) const noexcept { *out = h; }
    static void await_resume() noexcept {}
  };
  co_await Suspend{handle};
  log->push_back(name);
}

TEST(CoroEventLoopTest, Executor_Post_ResumesInPostOrder_AfterEveryEventQueuedBefore) {
  static constexpr const char* kMarker = "test::marker";
  RootTasks roots;
  std::coroutine_handle<> first;
  std::coroutine_handle<> second;
  std::vector<std::string> log;  // touched on the application thread only
  const EventConnection marker{kMarker, [&](const EventLoop::Event*) { log.emplace_back("marker"); }};

  OnLoop([&] {
    roots.Spawn(LogWhenResumed("first", &first, &log));
    roots.Spawn(LogWhenResumed("second", &second, &log));
  });

  // From another thread, as reactor callbacks run: the second posted first, then an event, then the first.
  std::thread([&] {
    EventLoopEnvironment::executor->Post(second);
    EventLoop::TriggerEvent(kMarker);
    EventLoopEnvironment::executor->Post(first);
  }).join();

  // OnLoop's own event queues after the three, so it sees them all handled.
  EXPECT_EQ(OnLoop([&] { return log; }), (std::vector<std::string>{"second", "marker", "first"}));
  EXPECT_EQ(OnLoop([&] { return roots.Size(); }), 0U);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  ::testing::AddGlobalTestEnvironment(new EventLoopEnvironment);
  return RUN_ALL_TESTS();
}
