///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <gtest/gtest.h>

#include <grpcpp/grpcpp.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/tests/continuous_use_support.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"
#include "rg_service/rg_utils.h"

/// Support for the tests of coro::Reader, under every buffer: a service streaming numbered features, an
/// executor that counts what it resumes, and a fixture that runs coroutines on the AppLoop task queue.
namespace coro_reader_test {

inline constexpr auto kTimeout = std::chrono::seconds(10);

/// Streams features named "feature 1" to "feature N", then finishes OK, with an error, or not at all: a held
/// stream stops after a given number of features, and ends only once the client cancels it. A gated stream sends
/// nothing until the test opens the gate, so a client is known to wait for its first message.
class StreamService final : public routeguide::RouteGuide::CallbackService {
 public:
  ~StreamService() override { JoinHolds(); }

  /// Streams this many features, then finishes OK. None by default: an empty stream.
  void Send(int count) { count_ = count; }

  /// Finishes with this error once this many features are sent.
  void FailAfter(int sent, grpc::StatusCode code) {
    fail_after_ = sent;
    error_ = code;
  }

  /// Stops once this many features are sent, until the client cancels, then finishes CANCELLED.
  void HoldAfter(int sent) {
    count_ = sent;
    hold_after_ = sent;
  }

  /// Sends nothing until OpenGate(), or finishes CANCELLED should the client cancel first.
  void GateFirstMessage() { gated_ = true; }
  /// Lets a gated stream send its first message, and the rest as configured.
  void OpenGate() { gate_open_ = true; }

  /// Ends every held stream, so none outlives the server.
  void JoinHolds() {
    stopping_ = true;
    const std::lock_guard lock(holds_mutex_);
    for (auto& hold : holds_) hold.join();
    holds_.clear();
  }

  grpc::ServerWriteReactor<routeguide::Feature>* ListFeatures(grpc::CallbackServerContext* context,
                                                              const routeguide::Rectangle* /*request*/) override {
    class Stream : public grpc::ServerWriteReactor<routeguide::Feature> {
     public:
      Stream(StreamService* service, grpc::CallbackServerContext* context) : service_(service), context_(context) {
        if (service_->gated_) {
          service_->Hold(
              context_, [service = service_] { return service->gate_open_.load(); },
              [this](bool opened) {
                if (opened) {
                  Next();
                } else {
                  Finish(grpc::Status::CANCELLED);
                }
              });
          return;
        }
        Next();
      }
      void OnWriteDone(bool ok) override {
        if (!ok) {
          Finish(grpc::Status(grpc::StatusCode::UNKNOWN, "write failed"));
          return;
        }
        Next();
      }
      void OnDone() override { delete this; }

     private:
      void Next() {
        if (sent_ == service_->fail_after_) {
          Finish(grpc::Status(service_->error_, "failed on purpose"));
          return;
        }
        if (sent_ == service_->hold_after_) {
          service_->Hold(
              context_, [] { return false; }, [this](bool /*opened*/) { Finish(grpc::Status::CANCELLED); });
          return;
        }
        if (sent_ == service_->count_) {
          Finish(grpc::Status::OK);
          return;
        }
        ++sent_;
        feature_ = rg_utils::MakeFeature("feature " + std::to_string(sent_), 1, sent_);
        StartWrite(&feature_);
      }
      StreamService* service_;
      grpc::CallbackServerContext* context_;
      routeguide::Feature feature_;
      int sent_ = 0;
    };
    return new Stream(this, context);
  }

 private:
  /// Runs `then(true)` once `open()` holds, or `then(false)` once the client cancels, or the service stops, on a
  /// thread of its own.
  template <class Open, class Then>
  void Hold(grpc::CallbackServerContext* context, Open open, Then then) {
    const std::lock_guard lock(holds_mutex_);
    holds_.emplace_back([this, context, open, then] {
      // Polled, since a cancel only shows in IsCancelled().
      while (!open() && !context->IsCancelled() && !stopping_) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
      }
      then(open() && !context->IsCancelled());
    });
  }

  std::atomic<int> count_{0};
  std::atomic<int> fail_after_{-1};
  std::atomic<grpc::StatusCode> error_{grpc::StatusCode::OK};
  std::atomic<int> hold_after_{-1};
  std::atomic<bool> gated_{false};
  std::atomic<bool> gate_open_{false};
  std::atomic<bool> stopping_{false};
  std::mutex holds_mutex_;
  std::vector<std::thread> holds_;
};

/// Resumes coroutines on an AppLoop, and counts how many it was given: each one is a coroutine that suspended.
class CountingExecutor final : public RpcReactor::coro::Executor {
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

/// Suspends forever: the frame is only ever destroyed.
struct Forever {
  static bool await_ready() noexcept { return false; }
  static void await_suspend(std::coroutine_handle<> /*waiter*/) noexcept {}
  static void await_resume() noexcept {}
};

/// Runs coroutines on an AppLoop against the in-process StreamService.
class CoroReaderTestBase : public RouteGuideTestFixtureBase<StreamService> {
 protected:
  void TearDown() override {
    test_service_.JoinHolds();
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
  std::size_t RootsLeft(const RpcReactor::coro::RootTasks& roots) {
    std::size_t left = 0;
    OnLoop([&] { left = roots.Size(); });
    return left;
  }

  /// Spawns a coroutine on the loop, and waits for it to signal `done`, then to be released.
  template <class MakeTask>
  void RunToEnd(MakeTask make_task) {
    RpcReactor::coro::RootTasks roots;
    std::promise<void> done;
    OnLoop([&] { roots.Spawn(make_task(&done)); });
    ASSERT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_EQ(RootsLeft(roots), 0U);
  }

  /// Destroys a frame once `make_task` read one message, and checks the reactor was released in time, posting
  /// nothing more.
  template <class MakeTask>
  void DestroyAfterOneMessage(MakeTask make_task) {
    auto roots = std::make_unique<RpcReactor::coro::RootTasks>();
    std::promise<void> read_one;
    OnLoop([&] { roots->Spawn(make_task(&read_one)); });
    ASSERT_EQ(read_one.get_future().wait_for(kTimeout), std::future_status::ready);
    const int posts = executor_.Posts();
    const auto start = std::chrono::steady_clock::now();
    OnLoop([&] {
      EXPECT_EQ(roots->Size(), 1U);
      roots.reset();
    });
    // Well under the 5 seconds DestroyAfterOnDone() waits before it leaks the reactor.
    EXPECT_LT(std::chrono::steady_clock::now() - start, std::chrono::seconds(3));
    EXPECT_EQ(executor_.Posts(), posts);
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

}  // namespace coro_reader_test
