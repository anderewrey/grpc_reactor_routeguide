///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests the core of the coroutine level, coro_task.h: Task<T>'s start, value, symmetric transfer and frame
/// ownership, RootTasks' ownership of spawned tasks, the exception policy, and an Executor resuming
/// coroutines on the single-threaded AppLoop task queue that stands in for the application's event loop.

#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <cstdint>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/tests/continuous_use_support.h"

namespace {

using RpcReactor::co::Executor;
using RpcReactor::co::RootTasks;
using RpcReactor::co::Task;

/// Counts its live instances, to show when a frame destroys what it owns. Coroutines take it by value, so
/// the frame owns a copy of its own.
class Tracked {
 public:
  explicit Tracked(int* live) : live_(live) { ++*live_; }
  Tracked(const Tracked& other) : live_(other.live_) { ++*live_; }
  Tracked& operator=(const Tracked&) = delete;
  ~Tracked() { --*live_; }

 private:
  int* live_;
};

/// Suspends the awaiting coroutine and hands its handle out, as a pending RPC operation would.
struct Suspend {
  std::coroutine_handle<>* out;
  static bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) const noexcept { *out = handle; }
  static void await_resume() noexcept {}
};

Task<int> Answer() { co_return 42; }

Task<int> Doubled(Task<int> inner) { co_return 2 * co_await std::move(inner); }

Task<std::unique_ptr<std::string>> Owned() { co_return std::make_unique<std::string>("owned"); }

Task<> Store(int* out, Task<int> inner) { *out = co_await std::move(inner); }

Task<> StoreOwned(std::string* out) { *out = *co_await Owned(); }

Task<> Count(int* steps) {
  ++*steps;
  co_return;
}

TEST(CoroTaskTest, Task_NotAwaited_BodyNeverStarts) {
  int steps = 0;
  {
    const auto task = Count(&steps);
  }
  EXPECT_EQ(steps, 0);
}

TEST(CoroTaskTest, Task_Awaited_ReturnsTheValue) {
  RootTasks roots;
  int result = 0;
  roots.Spawn(Store(&result, Answer()));
  EXPECT_EQ(result, 42);
  EXPECT_EQ(roots.Size(), 0U);
}

TEST(CoroTaskTest, Task_NestedChain_ValuesFlowBackUp) {
  RootTasks roots;
  int result = 0;
  roots.Spawn(Store(&result, Doubled(Doubled(Answer()))));
  EXPECT_EQ(result, 168);
}

TEST(CoroTaskTest, Task_MoveOnlyValue_IsMovedOut) {
  RootTasks roots;
  std::string result;
  roots.Spawn(StoreOwned(&result));
  EXPECT_EQ(result, "owned");
}

/// The stack position of a plain function called from a coroutine body: a coroutine's own locals live in
/// its heap frame, so they cannot show how deep the stack is.
[[gnu::noinline]] std::uintptr_t StackPointer() {
  return reinterpret_cast<std::uintptr_t>(__builtin_frame_address(0));
}

Task<std::uintptr_t> StackPosition() { co_return StackPointer(); }

Task<> AwaitInALoop(int iterations, std::uintptr_t* first, std::uintptr_t* last) {
  for (int i = 0; i < iterations; ++i) {
    const std::uintptr_t position = co_await StackPosition();
    if (i == 0) *first = position;
    *last = position;
  }
}

/// A child that completes without suspending resumes its awaiter by symmetric transfer, not by a nested
/// call, so a million of them in a loop run at the same stack depth: without it, the loop overflows the
/// stack, or at least the last child runs far deeper than the first.
TEST(CoroTaskTest, Task_MillionSynchronousAwaits_StackDoesNotGrow) {
  RootTasks roots;
  std::uintptr_t first = 0;
  std::uintptr_t last = 0;
  roots.Spawn(AwaitInALoop(1'000'000, &first, &last));
  ASSERT_NE(first, 0U);
  EXPECT_EQ(first, last);
}

Task<int> AnswerAfterSuspending(std::coroutine_handle<>* handle) {
  co_await Suspend{handle};
  co_return 42;
}

/// A child that suspends lets its awaiter suspend too, and its end resumes the awaiter with its value.
TEST(CoroTaskTest, Task_ChildSuspends_ItsEndResumesTheAwaiter) {
  RootTasks roots;
  std::coroutine_handle<> handle;
  int result = 0;
  roots.Spawn(Store(&result, AnswerAfterSuspending(&handle)));
  EXPECT_EQ(result, 0);
  EXPECT_EQ(roots.Size(), 1U);

  handle.resume();
  EXPECT_EQ(result, 42);
  EXPECT_EQ(roots.Size(), 0U);
}

/// Resumes a handle at once, on the thread that posts it. No executor of this level does that, since it
/// runs user code off the application thread, but it is the worst case for Task's start: the body ends on
/// another thread while its awaiter may still be inside Start().
class InlineExecutor final : public Executor {
 public:
  void Post(std::coroutine_handle<> handle) override { handle.resume(); }
};

/// Suspends, and has another thread resume it through the executor.
class ResumeFromAnotherThread {
 public:
  ResumeFromAnotherThread(Executor* executor, std::vector<std::thread>* threads)
      : executor_(executor), threads_(threads) {}
  static bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) const {
    threads_->emplace_back([executor = executor_, handle] { executor->Post(handle); });
  }
  static void await_resume() noexcept {}

 private:
  Executor* executor_;
  std::vector<std::thread>* threads_;
};

Task<int> OneAfterAnotherThread(Executor* executor, std::vector<std::thread>* threads) {
  co_await ResumeFromAnotherThread(executor, threads);
  co_return 1;
}

Task<> SumRaces(int rounds, Executor* executor, std::vector<std::thread>* threads, int* sum,
                std::promise<void>* done) {
  for (int i = 0; i < rounds; ++i) *sum += co_await OneAfterAnotherThread(executor, threads);
  done->set_value();
}

/// Each round, the body ends on a new thread, racing its awaiter's Start(): whichever comes second resumes
/// the awaiter, exactly once. A lost race resumes it twice or never, and ThreadSanitizer checks the
/// hand-over.
TEST(CoroTaskTest, Task_BodyEndsOnAnotherThreadDuringStart_AwaiterResumedOnce) {
  constexpr int kRounds = 1000;
  InlineExecutor executor;
  std::vector<std::thread> threads;
  threads.reserve(kRounds);  // never reallocated while a thread may still be starting
  int sum = 0;
  std::promise<void> done;
  {
    RootTasks roots;
    roots.Spawn(SumRaces(kRounds, &executor, &threads, &sum, &done));
    ASSERT_EQ(done.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
    for (auto& thread : threads) thread.join();
    EXPECT_EQ(roots.Size(), 0U);
  }
  EXPECT_EQ(sum, kRounds);
}

Task<> OwnsTracked(Tracked tracked, int* steps) {
  ++*steps;
  co_return;
}

TEST(CoroTaskTest, Task_DestroyedBeforeStart_DestroysItsFrame) {
  int live = 0;
  int steps = 0;
  {
    const auto task = OwnsTracked(Tracked(&live), &steps);
    EXPECT_EQ(live, 1);
  }
  EXPECT_EQ(live, 0);
  EXPECT_EQ(steps, 0);
}

TEST(CoroTaskTest, Task_MovedFrom_OwnsNothing) {
  int live = 0;
  int steps = 0;
  auto task = OwnsTracked(Tracked(&live), &steps);
  {
    const auto moved = std::move(task);
    EXPECT_EQ(live, 1);
  }
  EXPECT_EQ(live, 0);
}

Task<> AwaitThenCheck(Task<> child, const int* live, int* live_after) {
  co_await std::move(child);
  *live_after = *live;  // the child is finished, but its frame lives until its Task is destroyed
}

TEST(CoroTaskTest, Task_Finished_FrameLivesUntilTheTaskIsDestroyed) {
  RootTasks roots;
  int live = 0;
  int steps = 0;
  int live_after = -1;
  auto child = OwnsTracked(Tracked(&live), &steps);  // its own statement, so only the frame's copy lives
  roots.Spawn(AwaitThenCheck(std::move(child), &live, &live_after));
  EXPECT_EQ(steps, 1);
  EXPECT_EQ(live_after, 1);
  EXPECT_EQ(live, 0);
}

Task<> SuspendForever(Tracked tracked, int* live, std::coroutine_handle<>* handle) {
  const Tracked local(live);
  co_await Suspend{handle};
  ADD_FAILURE() << "a destroyed frame never resumes";
}

Task<> AwaitChild(Tracked tracked, Task<> child) { co_await std::move(child); }

TEST(CoroTaskTest, RootTasks_DestroyedWithSuspendedChain_DestroysEveryFrame) {
  int live = 0;
  std::coroutine_handle<> handle;
  {
    RootTasks roots;
    roots.Spawn(AwaitChild(Tracked(&live), SuspendForever(Tracked(&live), &live, &handle)));
    EXPECT_TRUE(handle);
    EXPECT_EQ(roots.Size(), 1U);
    EXPECT_EQ(live, 3);  // the parent's parameter, then the child's parameter and local
  }
  EXPECT_EQ(live, 0);
}

Task<> SuspendOnce(Tracked tracked, std::coroutine_handle<>* handle, int* steps) {
  ++*steps;
  co_await Suspend{handle};
  ++*steps;
}

TEST(CoroTaskTest, RootTasks_Spawn_RunsToTheFirstSuspension_ThenReleasesOnCompletion) {
  RootTasks roots;
  int live = 0;
  int steps = 0;
  std::coroutine_handle<> handle;
  roots.Spawn(SuspendOnce(Tracked(&live), &handle, &steps));
  EXPECT_EQ(steps, 1);
  EXPECT_EQ(roots.Size(), 1U);
  EXPECT_EQ(live, 1);

  handle.resume();
  EXPECT_EQ(steps, 2);
  EXPECT_EQ(roots.Size(), 0U);
  EXPECT_EQ(live, 0);
}

TEST(CoroTaskTest, RootTasks_SeveralRoots_EachReleasedOnItsOwnCompletion) {
  RootTasks roots;
  int live = 0;
  int steps = 0;
  std::coroutine_handle<> first;
  std::coroutine_handle<> second;
  roots.Spawn(SuspendOnce(Tracked(&live), &first, &steps));
  roots.Spawn(SuspendOnce(Tracked(&live), &second, &steps));
  roots.Spawn(OwnsTracked(Tracked(&live), &steps));
  EXPECT_EQ(roots.Size(), 2U);
  EXPECT_EQ(live, 2);

  second.resume();
  EXPECT_EQ(roots.Size(), 1U);
  EXPECT_EQ(live, 1);
  first.resume();
  EXPECT_EQ(roots.Size(), 0U);
  EXPECT_EQ(live, 0);
  EXPECT_EQ(steps, 5);
}

Task<> Throws() {
  throw std::runtime_error("boom");
  co_return;
}

TEST(CoroTaskDeathTest, Task_ExceptionEscapesTheBody_LogsAndTerminates) {
  GTEST_FLAG_SET(death_test_style, "threadsafe");
  EXPECT_DEATH(
      {
        RootTasks roots;
        roots.Spawn(Throws());
      },
      "coroutine: unhandled exception: boom");
}

/// Resumes coroutines on an AppLoop, the single-threaded task queue that stands in for the application's
/// event loop.
class AppLoopExecutor final : public Executor {
 public:
  explicit AppLoopExecutor(continuous_use_test::AppLoop* loop) : loop_(loop) {}
  void Post(std::coroutine_handle<> handle) override {
    loop_->Post([handle] { handle.resume(); });
  }

 private:
  continuous_use_test::AppLoop* loop_;
};

/// Completes on a thread of its own, as a gRPC callback does, which posts the handle to the executor.
class CompleteOnAnotherThread {
 public:
  CompleteOnAnotherThread(Executor* executor, std::vector<std::thread>* threads)
      : executor_(executor), threads_(threads) {}
  static bool await_ready() noexcept { return false; }
  void await_suspend(std::coroutine_handle<> handle) const {
    threads_->emplace_back([executor = executor_, handle] { executor->Post(handle); });
  }
  static std::thread::id await_resume() noexcept { return std::this_thread::get_id(); }

 private:
  Executor* executor_;
  std::vector<std::thread>* threads_;
};

/// Hops kHops times through other threads, each time resumed by the executor, and records where each
/// resumption ran. Runs on the AppLoop from start to end.
Task<> Hop(Executor* executor, std::vector<std::thread>* threads, std::vector<std::thread::id>* resumed_on,
           std::promise<void>* done) {
  constexpr int kHops = 100;
  for (int i = 0; i < kHops; ++i) {
    resumed_on->push_back(co_await CompleteOnAnotherThread(executor, threads));
  }
  done->set_value();
}

TEST(CoroTaskTest, Executor_ResumptionsFromOtherThreads_AllRunOnTheAppLoop) {
  continuous_use_test::AppLoop loop;
  AppLoopExecutor executor(&loop);
  RootTasks roots;
  std::vector<std::thread> threads;
  std::vector<std::thread::id> resumed_on;
  std::promise<void> done;
  std::promise<std::thread::id> loop_thread;

  // Spawned on the loop, as an application spawns its root tasks from the application thread.
  loop.Post([&] {
    loop_thread.set_value(std::this_thread::get_id());
    roots.Spawn(Hop(&executor, &threads, &resumed_on, &done));
  });
  ASSERT_EQ(done.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);

  // Released on the loop too, and only then joined: the threads post the handles, and the last one may
  // still be returning from Post() when the coroutine ends.
  std::promise<std::size_t> remaining;
  loop.Post([&] { remaining.set_value(roots.Size()); });
  EXPECT_EQ(remaining.get_future().get(), 0U);
  for (auto& thread : threads) thread.join();

  const std::thread::id app_thread = loop_thread.get_future().get();
  ASSERT_EQ(resumed_on.size(), 100U);
  for (const auto& id : resumed_on) EXPECT_EQ(id, app_thread);
  EXPECT_NE(app_thread, std::this_thread::get_id());
}

}  // namespace
