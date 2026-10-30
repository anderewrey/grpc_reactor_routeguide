///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests coro::Reader, the reader without a buffer of reactor_coro.h, against an in-process server, with
/// coroutines resumed on the AppLoop task queue that stands in for the application's event loop: every message
/// owned and in order under both deliveries, the end of the stream, a server error, kPull's read-ahead against
/// kPush's reading only when asked, a cancel while a read waits, and a frame destroyed mid-stream. The re-armed
/// hand-over inside the reader is tested on its own, with no RPC and no timing, in coro_task_test.cpp.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
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
using RpcReactor::Client::ReadPacing;
using RpcReactor::coro::Executor;
using RpcReactor::coro::Reader;
using RpcReactor::coro::RootTasks;
using RpcReactor::coro::Task;
template <Delivery kDelivery>
using ListFeaturesReader = Reader<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kTurnByTurn>>;

constexpr auto kTimeout = std::chrono::seconds(10);

// What Read() gives under each delivery.
static_assert(std::is_same_v<ListFeaturesReader<Delivery::kPull>::MessageT, std::optional<routeguide::Feature>>);
static_assert(std::is_same_v<ListFeaturesReader<Delivery::kPush>::MessageT, std::unique_ptr<routeguide::Feature>>);

/// Streams features named "feature 1" to "feature N", then finishes OK, with an error, or not at all: a held
/// stream stops after a given number of features, and ends only once the client cancels it.
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

  /// Ends every held stream, so none outlives the server.
  void JoinHolds() {
    stopping_ = true;
    const std::lock_guard lock(holds_mutex_);
    for (auto& hold : holds_) hold.join();
    holds_.clear();
  }

  grpc::ServerWriteReactor<routeguide::Feature>* ListFeatures(grpc::CallbackServerContext* context,
                                                              const routeguide::Rectangle*) override {
    class Stream : public grpc::ServerWriteReactor<routeguide::Feature> {
     public:
      Stream(StreamService* service, grpc::CallbackServerContext* context) : service_(service), context_(context) {
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
          service_->Hold(context_, [this] { Finish(grpc::Status::CANCELLED); });
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
  /// Runs `finish` once the client cancels, or the service stops, on a thread of its own.
  template <class F>
  void Hold(grpc::CallbackServerContext* context, F finish) {
    const std::lock_guard lock(holds_mutex_);
    holds_.emplace_back([this, context, finish] {
      // Polled, since a cancel only shows in IsCancelled().
      while (!context->IsCancelled() && !stopping_) std::this_thread::sleep_for(std::chrono::milliseconds(5));
      finish();
    });
  }

  std::atomic<int> count_{0};
  std::atomic<int> fail_after_{-1};
  std::atomic<grpc::StatusCode> error_{grpc::StatusCode::OK};
  std::atomic<int> hold_after_{-1};
  std::atomic<bool> stopping_{false};
  std::mutex holds_mutex_;
  std::vector<std::thread> holds_;
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
struct Seen {
  std::vector<std::string> names;        // of every message Read() gave, in order
  std::vector<std::thread::id> threads;  // each message was given on
  bool read_after_end_gave_nothing = false;
  grpc::Status status;
};

/// Reads the whole stream, reads once more after its end, then finishes.
template <Delivery kDelivery>
Task<> ReadAll(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen, std::promise<void>* done) {
  ListFeaturesReader<kDelivery> reader{*executor, *stub, routeguide::Rectangle{}};
  while (auto feature = co_await reader.Read()) {
    seen->names.push_back(feature->name());
    seen->threads.push_back(std::this_thread::get_id());
  }
  seen->read_after_end_gave_nothing = !co_await reader.Read();
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// Reads one message, pauses the loop thread, so no code of this coroutine runs and nothing asks for the next
/// message, then cancels and reads what is left.
template <Delivery kDelivery>
Task<> ReadOnePauseThenCancel(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                              std::promise<void>* done) {
  ListFeaturesReader<kDelivery> reader{*executor, *stub, routeguide::Rectangle{}};
  auto first = co_await reader.Read();
  if (first) seen->names.push_back(first->name());
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  reader.TryCancel();
  while (auto feature = co_await reader.Read()) seen->names.push_back(feature->name());
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// Reads one message, hands its reader out for the test to cancel, then waits in Read() until the stream ends.
template <Delivery kDelivery>
Task<> ReadUntilCancelled(Executor* executor, routeguide::RouteGuide::Stub* stub,
                          ListFeaturesReader<kDelivery>** out, std::promise<void>* waiting, Seen* seen,
                          std::promise<void>* done) {
  ListFeaturesReader<kDelivery> reader{*executor, *stub, routeguide::Rectangle{}};
  *out = &reader;
  while (auto feature = co_await reader.Read()) {
    seen->names.push_back(feature->name());
    // Set before the next Read() suspends, in the same resumption: a task the test queues behind it finds
    // the coroutine waiting.
    if (seen->names.size() == 1) waiting->set_value();
  }
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// Suspends forever: the frame is only ever destroyed.
struct Forever {
  static bool await_ready() noexcept { return false; }
  static void await_suspend(std::coroutine_handle<>) noexcept {}
  static void await_resume() noexcept {}
};

/// Reads one message and keeps it, then never asks for the next one, nor resumes.
template <Delivery kDelivery>
Task<> ReadOneThenStall(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* read_one) {
  ListFeaturesReader<kDelivery> reader{*executor, *stub, routeguide::Rectangle{}};
  [[maybe_unused]] const auto first = co_await reader.Read();
  read_one->set_value();
  co_await Forever{};
  ADD_FAILURE() << "a destroyed frame never resumes";
}

/// Reads one message, then waits in Read() for a held stream, until its frame is destroyed.
template <Delivery kDelivery>
Task<> ReadOneThenWait(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* read_one) {
  ListFeaturesReader<kDelivery> reader{*executor, *stub, routeguide::Rectangle{}};
  static_cast<void>(co_await reader.Read());
  read_one->set_value();
  static_cast<void>(co_await reader.Read());
  ADD_FAILURE() << "a destroyed frame never resumes";
}

class ReactorCoroReaderTest : public RouteGuideTestFixtureBase<StreamService> {
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
  std::size_t RootsLeft(const RootTasks& roots) {
    std::size_t left = 0;
    OnLoop([&] { left = roots.Size(); });
    return left;
  }

  /// Spawns a coroutine on the loop, and waits for it to signal `done`, then to be released.
  template <class MakeTask>
  void RunToEnd(MakeTask make_task) {
    RootTasks roots;
    std::promise<void> done;
    OnLoop([&] { roots.Spawn(make_task(&done)); });
    ASSERT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_EQ(RootsLeft(roots), 0U);
  }

  /// Reads the whole stream through ReadAll().
  template <Delivery kDelivery>
  Seen ReadWholeStream() {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) { return ReadAll<kDelivery>(&executor_, stub_.get(), &seen, done); });
    return seen;
  }

  /// Cancels from the loop while the coroutine waits in Read(), and returns what it saw.
  template <Delivery kDelivery>
  Seen CancelWhileReadWaits() {
    test_service_.HoldAfter(1);
    RootTasks roots;
    Seen seen;
    ListFeaturesReader<kDelivery>* reader = nullptr;
    std::promise<void> waiting;
    std::promise<void> done;
    OnLoop([&] {
      roots.Spawn(ReadUntilCancelled<kDelivery>(&executor_, stub_.get(), &reader, &waiting, &seen, &done));
    });
    EXPECT_EQ(waiting.get_future().wait_for(kTimeout), std::future_status::ready);
    OnLoop([&] { reader->TryCancel(); });
    EXPECT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_EQ(RootsLeft(roots), 0U);
    return seen;
  }

  /// Destroys a frame once `make_task` read one message, and checks the reactor was released in time, posting
  /// nothing more.
  template <class MakeTask>
  void DestroyAfterOneMessage(MakeTask make_task) {
    auto roots = std::make_unique<RootTasks>();
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

const std::vector<std::string> kThreeNames{"feature 1", "feature 2", "feature 3"};

/// @test Under kPull, Read() gives every message, swapped out of the reactor, in order and on the loop, then
/// nothing at the end of the stream, and nothing again after, and Finish() gives OK.
TEST_F(ReactorCoroReaderTest, ListFeatures_PullReadsEveryMessage_ThenNothing_ThenFinishOk) {
  test_service_.Send(3);
  const Seen seen = ReadWholeStream<Delivery::kPull>();

  EXPECT_EQ(seen.names, kThreeNames);
  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(3, LoopThread()));
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test Under kPush, Read() gives every message the read callback handed over, and the same end.
TEST_F(ReactorCoroReaderTest, ListFeatures_PushReadsEveryMessage_ThenNothing_ThenFinishOk) {
  test_service_.Send(3);
  const Seen seen = ReadWholeStream<Delivery::kPush>();

  EXPECT_EQ(seen.names, kThreeNames);
  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(3, LoopThread()));
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test An empty stream: the first Read() gives nothing, and Finish() gives OK.
TEST_F(ReactorCoroReaderTest, ListFeatures_EmptyStream_FirstReadGivesNothing) {
  const Seen seen = ReadWholeStream<Delivery::kPush>();

  EXPECT_TRUE(seen.names.empty());
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test A failed RPC is a value: Read() gives the messages sent before the error, then nothing, and Finish()
/// gives the error.
TEST_F(ReactorCoroReaderTest, ListFeatures_PullServerErrorMidStream_ReadsWhatCameThenFinishGivesTheError) {
  test_service_.Send(3);
  test_service_.FailAfter(2, grpc::StatusCode::NOT_FOUND);
  const Seen seen = ReadWholeStream<Delivery::kPull>();

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1", "feature 2"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test The same under kPush.
TEST_F(ReactorCoroReaderTest, ListFeatures_PushServerErrorMidStream_ReadsWhatCameThenFinishGivesTheError) {
  test_service_.Send(3);
  test_service_.FailAfter(2, grpc::StatusCode::NOT_FOUND);
  const Seen seen = ReadWholeStream<Delivery::kPush>();

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1", "feature 2"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test Under kPull, taking a message arms the next read at once: the next message arrives while the coroutine
/// pauses, so a Read() after a cancel still gives it, and only the read after it fails.
TEST_F(ReactorCoroReaderTest, ListFeatures_PullReadsAhead_TheNextMessageArrivesDuringAPause) {
  test_service_.HoldAfter(3);
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return ReadOnePauseThenCancel<Delivery::kPull>(&executor_, stub_.get(), &seen, done);
  });

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1", "feature 2"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test Under kPush, nothing is read until the coroutine asks: no message arrives during a pause, so a Read()
/// after a cancel gives nothing.
TEST_F(ReactorCoroReaderTest, ListFeatures_PushReadsOnlyWhenAsked_NothingArrivesDuringAPause) {
  test_service_.HoldAfter(3);
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return ReadOnePauseThenCancel<Delivery::kPush>(&executor_, stub_.get(), &seen, done);
  });

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test Under kPull, TryCancel() while a Read() waits on a held stream resumes it with nothing, and Finish()
/// gives CANCELLED.
TEST_F(ReactorCoroReaderTest, ListFeatures_PullTryCancel_WaitingReadGivesNothing_FinishGivesCancelled) {
  const Seen seen = CancelWhileReadWaits<Delivery::kPull>();

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test The same under kPush.
TEST_F(ReactorCoroReaderTest, ListFeatures_PushTryCancel_WaitingReadGivesNothing_FinishGivesCancelled) {
  const Seen seen = CancelWhileReadWaits<Delivery::kPush>();

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test A frame destroyed while its Read() waits on a held stream cancels the RPC, and destroys the reactor
/// only once OnDone() has run, without posting the destroyed coroutine. AddressSanitizer reports the reactor
/// freed under gRPC otherwise.
TEST_F(ReactorCoroReaderTest, ListFeatures_FrameDestroyedWhileReadWaits_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(1);
  DestroyAfterOneMessage([&](std::promise<void>* read_one) {
    return ReadOneThenWait<Delivery::kPull>(&executor_, stub_.get(), read_one);
  });
}

/// @test A frame destroyed while the reactor still holds the stream for the message it gave, under kPush, so
/// with no read armed, releases that hold: OnDone() then comes at once, rather than after the timeout that
/// leaks the reactor.
TEST_F(ReactorCoroReaderTest, ListFeatures_FrameDestroyedWithAMessageHeld_ReleasesTheHold) {
  test_service_.Send(3);
  DestroyAfterOneMessage([&](std::promise<void>* read_one) {
    return ReadOneThenStall<Delivery::kPush>(&executor_, stub_.get(), read_one);
  });
}

}  // namespace
