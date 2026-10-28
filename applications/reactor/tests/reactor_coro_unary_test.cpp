///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests co::Unary, the unary awaitable of reactor_coro.h, against an in-process server, with coroutines
/// resumed on the AppLoop task queue that stands in for the application's event loop: the outcome and the
/// owned response under both deliveries, a failed RPC, an RPC that ends before Finish() is awaited, a
/// cancel, and a frame destroyed while its RPC is in flight.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/tests/continuous_use_support.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"
#include "rg_service/rg_utils.h"

namespace {

using RpcReactor::Client::Delivery;
using RpcReactor::co::Executor;
using RpcReactor::co::RootTasks;
using RpcReactor::co::Task;
using RpcReactor::co::Unary;
template <Delivery kDelivery>
using GetFeatureCall = Unary<routeguide::GetFeature::ClientReactor<kDelivery>>;

constexpr auto kTimeout = std::chrono::seconds(10);

/// Answers GetFeature with a feature named after the point's latitude, or with an error. A held answer waits
/// for Release(), or for the client's cancel.
class UnaryService final : public routeguide::RouteGuide::CallbackService {
 public:
  ~UnaryService() override { JoinAnswers(); }

  /// Holds every later answer until Release().
  void Hold() { held_ = true; }

  /// Answers every held RPC.
  void Release() {
    {
      const std::lock_guard lock(mutex_);
      released_ = true;
    }
    cv_.notify_all();
  }

  /// Answers every later RPC with this error.
  void FailWith(grpc::StatusCode code) { error_ = code; }

  /// Waits for every held answer, so none outlives the server.
  void JoinAnswers() {
    Release();
    const std::lock_guard lock(answers_mutex_);
    for (auto& answer : answers_) answer.join();
    answers_.clear();
  }

  grpc::ServerUnaryReactor* GetFeature(grpc::CallbackServerContext* context, const routeguide::Point* point,
                                       routeguide::Feature* feature) override {
    auto* reactor = context->DefaultReactor();
    const routeguide::Point request = *point;
    auto answer = [this, context, reactor, feature, request] {
      if (error_ != grpc::StatusCode::OK) {
        reactor->Finish(grpc::Status(error_, "failed on purpose"));
        return;
      }
      *feature = rg_utils::MakeFeature("place " + std::to_string(request.latitude()), request.latitude(),
                                       request.longitude());
      reactor->Finish(context->IsCancelled() ? grpc::Status::CANCELLED : grpc::Status::OK);
    };
    if (!held_) {
      answer();
      return reactor;
    }
    const std::lock_guard lock(answers_mutex_);
    answers_.emplace_back([this, context, answer] {
      std::unique_lock lock(mutex_);
      // Polled, since a cancel only shows in IsCancelled().
      while (!released_ && !context->IsCancelled()) cv_.wait_for(lock, std::chrono::milliseconds(5));
      lock.unlock();
      answer();
    });
    return reactor;
  }

 private:
  std::atomic<bool> held_{false};
  std::atomic<grpc::StatusCode> error_{grpc::StatusCode::OK};
  std::mutex mutex_;
  std::condition_variable cv_;
  bool released_ = false;
  std::mutex answers_mutex_;
  std::vector<std::thread> answers_;
};

/// Resumes coroutines on an AppLoop, and counts how many it was given.
class CountingExecutor final : public Executor {
 public:
  explicit CountingExecutor(continuous_use_test::AppLoop* loop) : loop_(loop) {}
  void Post(std::coroutine_handle<> handle) override {
    ++posts_;
    loop_->Post([handle] { handle.resume(); });
  }
  [[nodiscard]] int Posts() const { return posts_.load(); }

 private:
  continuous_use_test::AppLoop* loop_;
  std::atomic<int> posts_{0};
};

/// What a test coroutine saw, written on the loop, read by the test once `done` is ready.
template <Delivery kDelivery>
struct Seen {
  typename GetFeatureCall<kDelivery>::ResultT result;
  std::thread::id resumed_on;
};

template <Delivery kDelivery>
Task<> Call(Executor* executor, routeguide::RouteGuide::Stub* stub, routeguide::Point point, Seen<kDelivery>* seen,
            std::promise<void>* done) {
  GetFeatureCall<kDelivery> call{*executor, *stub, point};
  seen->result = co_await call.Finish();
  seen->resumed_on = std::this_thread::get_id();
  done->set_value();
}

class ReactorCoroUnaryTest : public RouteGuideTestFixtureBase<UnaryService> {
 protected:
  void TearDown() override {
    test_service_.JoinAnswers();
    RouteGuideTestFixtureBase::TearDown();
  }

  /// Runs a function on the loop, and waits for it.
  template <class F>
  void OnLoop(F function) {
    std::promise<void> ran;
    loop_.Post([&] {
      function();
      ran.set_value();
    });
    ASSERT_EQ(ran.get_future().wait_for(kTimeout), std::future_status::ready);
  }

  /// Waits for the loop to finish what it runs, then reads how many root tasks are left. A coroutine signals
  /// `done` before it ends, so a test calls this before destroying its RootTasks, which belongs to the loop.
  std::size_t RootsLeft(const RootTasks& roots) {
    std::size_t left = 0;
    OnLoop([&] { left = roots.Size(); });
    return left;
  }

  /// Runs one GetFeature through Call() on the loop, and waits for it to end and be released. A held answer is
  /// released only once the coroutine waits in Finish(), since Spawn() returns at its first suspension.
  template <Delivery kDelivery>
  Seen<kDelivery> RunCall() {
    RootTasks roots;
    Seen<kDelivery> seen;
    std::promise<void> done;
    OnLoop([&] { roots.Spawn(Call(&executor_, stub_.get(), rg_utils::MakePoint(7, 8), &seen, &done)); });
    test_service_.Release();
    EXPECT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_EQ(RootsLeft(roots), 0U);
    return seen;
  }

  /// The loop's own thread.
  std::thread::id LoopThread() {
    std::thread::id id;
    OnLoop([&] { id = std::this_thread::get_id(); });
    return id;
  }

  continuous_use_test::AppLoop loop_;
  CountingExecutor executor_{&loop_};
};

/// @test Under kPull, Finish() resumes on the loop, once, with the status and the response swapped out of the
/// reactor.
TEST_F(ReactorCoroUnaryTest, GetFeature_PullOk_ReturnsOwnedResponseOnTheLoop) {
  test_service_.Hold();
  const auto seen = RunCall<Delivery::kPull>();

  EXPECT_TRUE(seen.result.status.ok());
  EXPECT_EQ(seen.result.response.name(), "place 7");
  EXPECT_EQ(seen.resumed_on, LoopThread());
  EXPECT_EQ(executor_.Posts(), 1);
}

/// @test Under kPush, Finish() returns the message the done callback handed over.
TEST_F(ReactorCoroUnaryTest, GetFeature_PushOk_ReturnsOwnedResponseOnTheLoop) {
  test_service_.Hold();
  const auto seen = RunCall<Delivery::kPush>();

  EXPECT_TRUE(seen.result.status.ok());
  ASSERT_NE(seen.result.response, nullptr);
  EXPECT_EQ(seen.result.response->name(), "place 7");
  EXPECT_EQ(seen.resumed_on, LoopThread());
  EXPECT_EQ(executor_.Posts(), 1);
}

/// @test A failed RPC is a value: Finish() returns its status, and an empty response under kPull.
TEST_F(ReactorCoroUnaryTest, GetFeature_PullServerError_ReturnsTheStatusAndAnEmptyResponse) {
  test_service_.FailWith(grpc::StatusCode::NOT_FOUND);
  const auto seen = RunCall<Delivery::kPull>();

  EXPECT_EQ(seen.result.status.error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_TRUE(seen.result.response.name().empty());
}

/// @test Under kPush, a failed RPC still hands over a message, never null and empty.
TEST_F(ReactorCoroUnaryTest, GetFeature_PushServerError_ReturnsTheStatusAndAnEmptyMessage) {
  test_service_.FailWith(grpc::StatusCode::NOT_FOUND);
  const auto seen = RunCall<Delivery::kPush>();

  EXPECT_EQ(seen.result.status.error_code(), grpc::StatusCode::NOT_FOUND);
  ASSERT_NE(seen.result.response, nullptr);
  EXPECT_TRUE(seen.result.response->name().empty());
}

Task<> CallAfterTheEnd(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen<Delivery::kPull>* seen,
                       std::promise<void>* done) {
  GetFeatureCall<Delivery::kPull> call{*executor, *stub, rg_utils::MakePoint(7, 8)};
  // Blocks the loop until the RPC has ended, as a busy application thread would.
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (!call.Ended() && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  seen->result = co_await call.Finish();
  seen->resumed_on = std::this_thread::get_id();
  done->set_value();
}

/// @test An RPC that has ended before Finish() is awaited does not suspend the coroutine: nothing is posted.
TEST_F(ReactorCoroUnaryTest, GetFeature_EndedBeforeFinish_DoesNotSuspend) {
  RootTasks roots;
  Seen<Delivery::kPull> seen;
  std::promise<void> done;
  OnLoop([&] { roots.Spawn(CallAfterTheEnd(&executor_, stub_.get(), &seen, &done)); });
  ASSERT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
  ASSERT_EQ(RootsLeft(roots), 0U);

  EXPECT_TRUE(seen.result.status.ok());
  EXPECT_EQ(seen.result.response.name(), "place 7");
  EXPECT_EQ(executor_.Posts(), 0);
  EXPECT_EQ(seen.resumed_on, LoopThread());
}

Task<> CancelThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen<Delivery::kPull>* seen,
                        std::promise<void>* done) {
  GetFeatureCall<Delivery::kPull> call{*executor, *stub, rg_utils::MakePoint(7, 8)};
  call.TryCancel();
  seen->result = co_await call.Finish();
  done->set_value();
}

/// @test TryCancel() ends an RPC the server holds: Finish() returns CANCELLED.
TEST_F(ReactorCoroUnaryTest, GetFeature_TryCancel_FinishReturnsCancelled) {
  test_service_.Hold();
  RootTasks roots;
  Seen<Delivery::kPull> seen;
  std::promise<void> done;
  OnLoop([&] { roots.Spawn(CancelThenFinish(&executor_, stub_.get(), &seen, &done)); });
  ASSERT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
  ASSERT_EQ(RootsLeft(roots), 0U);

  EXPECT_EQ(seen.result.status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_TRUE(seen.result.response.name().empty());
}

Task<> NeverResumed(Executor* executor, routeguide::RouteGuide::Stub* stub) {
  GetFeatureCall<Delivery::kPush> call{*executor, *stub, rg_utils::MakePoint(7, 8)};
  static_cast<void>(co_await call.Finish());
  ADD_FAILURE() << "a destroyed frame never resumes";
}

/// @test A frame destroyed while its RPC is in flight, as at shutdown, cancels the RPC and destroys the reactor
/// only once OnDone() has run, without posting the destroyed coroutine. AddressSanitizer reports the reactor
/// freed under gRPC otherwise.
TEST_F(ReactorCoroUnaryTest, GetFeature_FrameDestroyedMidRpc_CancelsAndWaitsForOnDone) {
  test_service_.Hold();
  auto roots = std::make_unique<RootTasks>();
  OnLoop([&] { roots->Spawn(NeverResumed(&executor_, stub_.get())); });
  OnLoop([&] {
    EXPECT_EQ(roots->Size(), 1U);
    roots.reset();
  });
  EXPECT_EQ(executor_.Posts(), 0);
}

}  // namespace
