///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests the core of the coroutine level, coro_task.h: Task<T>'s start, value, symmetric transfer and frame
/// ownership, RootTasks' ownership of spawned tasks, the exception policy, an Executor resuming coroutines
/// on the single-threaded AppLoop task queue that stands in for the application's event loop, and
/// ResumeHandover, which every awaitable uses to hand its coroutine to the executor.

#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
#include <cstddef>
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

using RpcReactor::coro::CurrentTask;
using RpcReactor::coro::Executor;
using RpcReactor::coro::ResumeHandover;
using RpcReactor::coro::RootTasks;
using RpcReactor::coro::Task;

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

// =============================================================================
// ResumeHandover, the hand-over between the end of an operation and the awaiting coroutine
// =============================================================================

/// Records the handles it is given, and resumes none.
class RecordingExecutor final : public Executor {
 public:
  void Post(std::coroutine_handle<> handle) override { posted.push_back(handle); }
  std::vector<std::coroutine_handle<>> posted;
};

/// @test An operation that ended before the coroutine awaits it lets the coroutine go on without suspending,
/// and posts nothing: what Unary's Finish() does when the RPC ended first.
TEST(CoroHandoverTest, EndedFirst_DoesNotSuspend_AndPostsNothing) {
  RecordingExecutor executor;
  ResumeHandover handover;
  handover.Complete(executor);

  EXPECT_FALSE(handover.Suspend(std::noop_coroutine()));
  EXPECT_TRUE(executor.posted.empty());
}

/// @test A coroutine that awaits first suspends, and the end of the operation posts it, once.
TEST(CoroHandoverTest, AwaitedFirst_Suspends_AndTheEndPostsItOnce) {
  RecordingExecutor executor;
  ResumeHandover handover;
  const std::coroutine_handle<> waiter = std::noop_coroutine();

  EXPECT_TRUE(handover.Suspend(waiter));
  EXPECT_TRUE(executor.posted.empty());
  handover.Complete(executor);
  ASSERT_EQ(executor.posted.size(), 1U);
  EXPECT_EQ(executor.posted.front(), waiter);
}

/// @test Once abandoned, as by an awaitable being destroyed, the end of the operation posts nothing.
TEST(CoroHandoverTest, Abandoned_TheEndPostsNothing) {
  RecordingExecutor executor;
  ResumeHandover handover;

  EXPECT_TRUE(handover.Suspend(std::noop_coroutine()));
  handover.Abandon();
  handover.Complete(executor);
  EXPECT_TRUE(executor.posted.empty());
}

/// @test Re-armed after each end, one hand-over serves wait after wait, as for each read of a stream: each
/// coroutine awaiting first suspends again, and each end posts it once.
TEST(CoroHandoverTest, Rearmed_EachAwaitSuspends_AndEachEndPostsItOnce) {
  RecordingExecutor executor;
  ResumeHandover handover;
  const std::coroutine_handle<> waiter = std::noop_coroutine();

  for (int wait = 1; wait <= 3; ++wait) {
    if (wait > 1) handover.Rearm();
    EXPECT_TRUE(handover.Suspend(waiter));
    handover.Complete(executor);
    ASSERT_EQ(executor.posted.size(), static_cast<std::size_t>(wait));
    EXPECT_EQ(executor.posted.back(), waiter);
  }
}

/// @test Re-armed, the hand-over starts afresh whichever came first: an operation that ends before its
/// coroutine awaits it lets the coroutine go on, and the next one, awaited first, suspends it.
TEST(CoroHandoverTest, Rearmed_EndedFirstThenAwaitedFirst_EachDecidedOnItsOwn) {
  RecordingExecutor executor;
  ResumeHandover handover;
  const std::coroutine_handle<> waiter = std::noop_coroutine();

  handover.Complete(executor);
  EXPECT_FALSE(handover.Suspend(waiter));
  handover.Rearm();
  handover.Complete(executor);
  EXPECT_FALSE(handover.Suspend(waiter));
  EXPECT_TRUE(executor.posted.empty());

  handover.Rearm();
  EXPECT_TRUE(handover.Suspend(waiter));
  handover.Complete(executor);
  EXPECT_EQ(executor.posted.size(), 1U);
}

// =============================================================================
// CurrentTask, the task number the logs show
// =============================================================================

/// An operation another thread ends, handing the awaiting coroutine to the executor through a ResumeHandover,
/// as every awaitable of the level does.
class HandedOverOperation {
 public:
  explicit HandedOverOperation(Executor* executor) : executor_(executor) {}
  static bool await_ready() noexcept { return false; }
  bool await_suspend(std::coroutine_handle<> waiter) noexcept { return handover_.Suspend(waiter); }
  void await_resume() const noexcept { handover_.Resumed(); }

  /// Ends the operation, from any thread.
  void End() { handover_.Complete(*executor_); }

  /// Starts the operation again once it ended, on the loop, for a coroutine that awaits it once more.
  void Rearm() { handover_.Rearm(); }

 private:
  Executor* executor_;
  ResumeHandover handover_;
};

/// What a task saw of its own number.
struct SeenTasks {
  CurrentTask::Id at_start = CurrentTask::kNone;
  CurrentTask::Id in_awaited_task = CurrentTask::kNone;
  CurrentTask::Id after_resume = CurrentTask::kNone;
};

/// Checks that a task saw its own number everywhere it looked.
void ExpectOwnTask(const SeenTasks& seen, CurrentTask::Id id) {
  EXPECT_EQ(seen.at_start, id);
  EXPECT_EQ(seen.in_awaited_task, id);
  EXPECT_EQ(seen.after_resume, id);
}

Task<> RecordTask(CurrentTask::Id* seen) {
  *seen = CurrentTask::Get();
  co_return;
}

Task<> SeeOwnTask(HandedOverOperation* operation, SeenTasks* seen, std::promise<void>* done) {
  seen->at_start = CurrentTask::Get();
  co_await RecordTask(&seen->in_awaited_task);
  co_await *operation;
  seen->after_resume = CurrentTask::Get();
  done->set_value();
}

/// @test A spawned task runs under the number Spawn() returns, in its own code and in a task it awaits. Across a
/// suspension through the hand-over, it runs under it again once resumed on the loop, and the loop thread runs
/// under no task between two resumptions. Two tasks get two numbers.
TEST(CoroTaskTest, CurrentTask_FollowsEachTaskAcrossItsSuspensions_AndIsNoneBetween) {
  continuous_use_test::AppLoop loop;
  AppLoopExecutor executor(&loop);
  RootTasks roots;
  HandedOverOperation first_operation(&executor);
  HandedOverOperation second_operation(&executor);
  SeenTasks first;
  SeenTasks second;
  std::promise<void> first_done;
  std::promise<void> second_done;
  CurrentTask::Id first_id = CurrentTask::kNone;
  CurrentTask::Id second_id = CurrentTask::kNone;
  CurrentTask::Id between = CurrentTask::kNone + 1;

  std::promise<void> spawned;
  loop.Post([&] {
    first_id = roots.Spawn(SeeOwnTask(&first_operation, &first, &first_done));
    second_id = roots.Spawn(SeeOwnTask(&second_operation, &second, &second_done));
    between = CurrentTask::Get();
    spawned.set_value();
  });
  spawned.get_future().wait();
  // Ended from this thread, as a gRPC callback would: the loop resumes each task.
  second_operation.End();
  first_operation.End();
  ASSERT_EQ(first_done.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
  ASSERT_EQ(second_done.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
  std::promise<CurrentTask::Id> after;
  loop.Post([&] { after.set_value(CurrentTask::Get()); });

  EXPECT_NE(first_id, CurrentTask::kNone);
  EXPECT_NE(first_id, second_id);
  EXPECT_EQ(between, CurrentTask::kNone);
  EXPECT_EQ(after.get_future().get(), CurrentTask::kNone);
  ExpectOwnTask(first, first_id);
  ExpectOwnTask(second, second_id);
}

Task<> WaitTwice(HandedOverOperation* first, HandedOverOperation* second, std::promise<void>* done) {
  co_await *first;
  co_await *second;
  done->set_value();
}

/// @test A task resumed on the loop that suspends again leaves the loop under no task until its next
/// resumption: the next handler the loop runs, here a probe queued right behind the resumption, sees none.
TEST(CoroTaskTest, CurrentTask_TaskSuspendedAgain_LeavesTheLoopUnderNoTask) {
  continuous_use_test::AppLoop loop;
  AppLoopExecutor executor(&loop);
  RootTasks roots;
  HandedOverOperation first(&executor);
  HandedOverOperation second(&executor);
  std::promise<void> done;

  std::promise<void> spawned;
  loop.Post([&] {
    roots.Spawn(WaitTwice(&first, &second, &done));
    spawned.set_value();
  });
  spawned.get_future().wait();
  first.End();  // queues the resumption, which runs up to the second suspension
  std::promise<CurrentTask::Id> between;
  loop.Post([&] { between.set_value(CurrentTask::Get()); });
  EXPECT_EQ(between.get_future().get(), CurrentTask::kNone);

  second.End();
  ASSERT_EQ(done.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
  std::promise<std::size_t> left;
  loop.Post([&] { left.set_value(roots.Size()); });
  EXPECT_EQ(left.get_future().get(), 0U);
}

Task<> WaitAgainAndAgain(HandedOverOperation* operation, std::vector<std::promise<void>>* resumed,
                         std::vector<CurrentTask::Id>* seen) {
  for (std::size_t wait = 0; wait < resumed->size(); ++wait) {
    if (wait > 0) operation->Rearm();
    co_await *operation;
    seen->push_back(CurrentTask::Get());
    (*resumed)[wait].set_value();
  }
}

/// @test A task that awaits one re-armed hand-over again and again runs under its own number after each
/// resumption, and leaves the loop under no task between two, as a coroutine reading a stream does.
TEST(CoroTaskTest, CurrentTask_RearmedHandover_RestoresTheTaskAtEachResumption) {
  continuous_use_test::AppLoop loop;
  AppLoopExecutor executor(&loop);
  RootTasks roots;
  HandedOverOperation operation(&executor);
  std::vector<std::promise<void>> resumed(3);
  std::vector<CurrentTask::Id> seen;
  CurrentTask::Id id = CurrentTask::kNone;

  std::promise<void> spawned;
  loop.Post([&] {
    id = roots.Spawn(WaitAgainAndAgain(&operation, &resumed, &seen));
    spawned.set_value();
  });
  spawned.get_future().wait();
  for (auto& resumption : resumed) {
    // Ended only once the task re-armed the operation, which it does before it awaits it again.
    operation.End();
    ASSERT_EQ(resumption.get_future().wait_for(std::chrono::seconds(10)), std::future_status::ready);
    std::promise<CurrentTask::Id> between;
    loop.Post([&] { between.set_value(CurrentTask::Get()); });
    EXPECT_EQ(between.get_future().get(), CurrentTask::kNone);
  }
  std::promise<std::size_t> left;
  loop.Post([&] { left.set_value(roots.Size()); });
  EXPECT_EQ(left.get_future().get(), 0U);

  EXPECT_NE(id, CurrentTask::kNone);
  EXPECT_EQ(seen, (std::vector<CurrentTask::Id>{id, id, id}));
}

}  // namespace
