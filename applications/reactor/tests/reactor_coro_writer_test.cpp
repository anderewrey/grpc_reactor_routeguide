///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests the writers of reactor_coro.h against an in-process server, with coroutines resumed on the AppLoop task
/// queue that stands in for the application's event loop. coro::Writer: every point written then the summary
/// received under both deliveries, a write that completes before the coroutine suspends, Write() after the RPC
/// ended giving false, a cancel, Finish() awaited before Close(), and a frame destroyed mid-write or between
/// writes. coro::Source: every element of a range sent in order, an empty range, ranges that are not a vector, a
/// cancel, and a frame destroyed while the server holds the RPC.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <cstddef>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <ranges>
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
using RpcReactor::Client::Submission;
using RpcReactor::coro::Executor;
using RpcReactor::coro::RootTasks;
using RpcReactor::coro::Source;
using RpcReactor::coro::Task;
using RpcReactor::coro::Writer;
template <Delivery kDelivery>
using RecordRouteWriter = Writer<routeguide::RecordRoute::ClientReactor<kDelivery, Submission::kApplicationPushes>>;
template <Delivery kDelivery>
using RecordRouteSource = Source<routeguide::RecordRoute::ClientReactor<kDelivery, Submission::kReactorPulls>>;

constexpr auto kTimeout = std::chrono::seconds(10);

// What Finish() gives, the same under both deliveries and for both writers.
static_assert(std::is_same_v<RecordRouteWriter<Delivery::kPull>::ResultT,
                             RpcReactor::coro::UnaryResult<routeguide::RouteSummary>>);
static_assert(std::is_same_v<RecordRouteWriter<Delivery::kPush>::ResultT,
                             RpcReactor::coro::UnaryResult<routeguide::RouteSummary>>);
static_assert(std::is_same_v<RecordRouteSource<Delivery::kPull>::ResultT,
                             RpcReactor::coro::UnaryResult<routeguide::RouteSummary>>);

/// The point numbered `i`, recognised by the server by its latitude.
routeguide::Point PointNumber(int i) { return rg_utils::MakePoint(i, 0); }

/// The points numbered 1 to `count`.
std::vector<routeguide::Point> Points(int count) {
  std::vector<routeguide::Point> points;
  for (int i = 1; i <= count; ++i) points.push_back(PointNumber(i));
  return points;
}

/// The latitudes 1 to `count`, which the server records for Points(count).
std::vector<int> Numbers(int count) {
  std::vector<int> numbers;
  for (int i = 1; i <= count; ++i) numbers.push_back(i);
  return numbers;
}

/// Records every point it reads, and finishes once the client closes its stream, with a summary counting them:
/// at once, or once the client cancels when held. Or finishes with an error once a given number is read.
class RecordService final : public routeguide::RouteGuide::CallbackService {
 public:
  ~RecordService() override { JoinHolds(); }

  /// Finishes with this error once this many points are read.
  void FailAfter(int received, grpc::StatusCode code) {
    fail_after_ = received;
    error_ = code;
  }

  /// Once the client closes its stream, waits until it cancels, then finishes CANCELLED.
  void HoldAtClose() { hold_ = true; }

  /// The latitudes of the points read, in order.
  std::vector<int> Received() {
    const std::lock_guard lock(mutex_);
    return received_;
  }

  /// Ends every held RPC, so none outlives the server.
  void JoinHolds() {
    stopping_ = true;
    const std::lock_guard lock(holds_mutex_);
    for (auto& hold : holds_) hold.join();
    holds_.clear();
  }

  grpc::ServerReadReactor<routeguide::Point>* RecordRoute(grpc::CallbackServerContext* context,
                                                          routeguide::RouteSummary* summary) override {
    class Record : public grpc::ServerReadReactor<routeguide::Point> {
     public:
      Record(RecordService* service, grpc::CallbackServerContext* context, routeguide::RouteSummary* summary)
          : service_(service), context_(context), summary_(summary) {
        StartRead(&point_);
      }
      void OnReadDone(bool ok) override {
        if (ok) {
          service_->Record(point_.latitude());
          if (++count_ == service_->fail_after_) {
            Finish(grpc::Status(service_->error_, "failed on purpose"));
            return;
          }
          StartRead(&point_);
          return;
        }
        if (service_->hold_ && !context_->IsCancelled()) {
          service_->Hold(context_, [this] { Finish(grpc::Status::CANCELLED); });
          return;
        }
        summary_->set_point_count(count_);
        Finish(grpc::Status::OK);
      }
      void OnDone() override { delete this; }

     private:
      RecordService* service_;
      grpc::CallbackServerContext* context_;
      routeguide::RouteSummary* summary_;
      routeguide::Point point_;
      int count_ = 0;
    };
    return new Record(this, context, summary);
  }

 private:
  void Record(int latitude) {
    const std::lock_guard lock(mutex_);
    received_.push_back(latitude);
  }

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

  std::atomic<int> fail_after_{-1};
  std::atomic<grpc::StatusCode> error_{grpc::StatusCode::OK};
  std::atomic<bool> hold_{false};
  std::atomic<bool> stopping_{false};
  std::mutex mutex_;
  std::vector<int> received_;
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
  std::vector<bool> writes;              // what each Write() gave, in order
  std::vector<std::thread::id> threads;  // each resumption ran on
  std::optional<bool> closed;            // what Close() gave, if called
  grpc::Status status;
  int point_count = -1;  // of the summary
};

/// Writes `count` points, closes, then finishes.
template <Delivery kDelivery>
Task<> WriteAllThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, int count, Seen* seen,
                          std::promise<void>* done) {
  RecordRouteWriter<kDelivery> writer{*executor, *stub};
  for (int i = 1; i <= count; ++i) {
    seen->writes.push_back(co_await writer.Write(PointNumber(i)));
    seen->threads.push_back(std::this_thread::get_id());
  }
  seen->closed = co_await writer.Close();
  auto [status, summary] = co_await writer.Finish();
  seen->threads.push_back(std::this_thread::get_id());
  seen->status = status;
  seen->point_count = summary.point_count();
  done->set_value();
}

/// Starts a write, pauses the loop thread so the write completes before the coroutine awaits it, then awaits it
/// and finishes. `posts` receives how many resumptions the executor was given meanwhile.
Task<> WriteCompletingBeforeTheAwait(Executor* executor, routeguide::RouteGuide::Stub* stub,
                                     const CountingExecutor* counter, int* posts, Seen* seen,
                                     std::promise<void>* done) {
  RecordRouteWriter<Delivery::kPull> writer{*executor, *stub};
  const int before = counter->Posts();
  auto write = writer.Write(PointNumber(1));
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  seen->writes.push_back(co_await write);
  *posts = counter->Posts() - before;
  seen->writes.push_back(co_await writer.Write(PointNumber(2)));
  seen->closed = co_await writer.Close();
  auto [status, summary] = co_await writer.Finish();
  seen->status = status;
  seen->point_count = summary.point_count();
  done->set_value();
}

/// Writes until a Write() gives false, pausing between writes so the server's error has time to arrive, then
/// writes once more, closes, and finishes.
template <Delivery kDelivery>
Task<> WriteUntilRefused(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                         std::promise<void>* done) {
  RecordRouteWriter<kDelivery> writer{*executor, *stub};
  // Bounded: 5 seconds of writes.
  for (int i = 1; i <= 1000; ++i) {
    const bool written = co_await writer.Write(PointNumber(i));
    seen->writes.push_back(written);
    if (!written) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  seen->writes.push_back(co_await writer.Write(PointNumber(0)));
  seen->closed = co_await writer.Close();
  auto [status, summary] = co_await writer.Finish();
  seen->status = status;
  done->set_value();
}

/// Writes one point, pauses the loop thread so the server's error arrives, then writes again, and finishes.
template <Delivery kDelivery>
Task<> WriteAfterTheServerError(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                                std::promise<void>* done) {
  RecordRouteWriter<kDelivery> writer{*executor, *stub};
  seen->writes.push_back(co_await writer.Write(PointNumber(1)));
  std::this_thread::sleep_for(std::chrono::milliseconds(300));
  seen->writes.push_back(co_await writer.Write(PointNumber(2)));
  auto [status, summary] = co_await writer.Finish();
  seen->status = status;
  done->set_value();
}

/// Writes one point, cancels, writes again, then finishes without closing.
template <Delivery kDelivery>
Task<> WriteCancelWrite(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                        std::promise<void>* done) {
  RecordRouteWriter<kDelivery> writer{*executor, *stub};
  seen->writes.push_back(co_await writer.Write(PointNumber(1)));
  writer.TryCancel();
  seen->writes.push_back(co_await writer.Write(PointNumber(2)));
  auto [status, summary] = co_await writer.Finish();
  seen->status = status;
  done->set_value();
}

/// Writes `count` points, then awaits Finish() without closing, after pausing the loop thread when `pause` is
/// set, so that a server which finishes on its own has done so by then.
template <Delivery kDelivery>
Task<> WriteSomeThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, int count, bool pause,
                           Seen* seen, std::promise<void>* done) {
  RecordRouteWriter<kDelivery> writer{*executor, *stub};
  for (int i = 1; i <= count; ++i) seen->writes.push_back(co_await writer.Write(PointNumber(i)));
  if (pause) std::this_thread::sleep_for(std::chrono::milliseconds(300));
  auto [status, summary] = co_await writer.Finish();
  seen->status = status;
  done->set_value();
}

/// Suspends forever: the frame is only ever destroyed.
struct Forever {
  static bool await_ready() noexcept { return false; }
  static void await_suspend(std::coroutine_handle<> /*waiter*/) noexcept {}
  static void await_resume() noexcept {}
};

/// A RecordRoute reactor whose OnWriteDone() waits before it runs, so a write is still in flight while the test
/// destroys the frame.
class SlowWriteReactor final
    : public RpcReactor::Client::ActiveWriteReactor<routeguide::Point, routeguide::RouteSummary, Delivery::kPull,
                                                    Submission::kApplicationPushes> {
  using Base = RpcReactor::Client::ActiveWriteReactor<routeguide::Point, routeguide::RouteSummary,
                                                      Delivery::kPull, Submission::kApplicationPushes>;

 public:
  using Callbacks = RpcReactor::Client::ActiveWriteCallbacks<routeguide::Point, routeguide::RouteSummary,
                                                             Delivery::kPull, Submission::kApplicationPushes>;

  SlowWriteReactor(routeguide::RouteGuide::Stub& stub, std::unique_ptr<grpc::ClientContext> context, Callbacks&& cbs)
      : Base(std::move(context), std::move(cbs)) {
    stub.async()->RecordRoute(this->context_.get(), this->response_.get(), this);
    this->StartCall();
  }

 protected:
  void OnWriteDone(bool ok) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Base::OnWriteDone(ok);
  }
};

/// Signals `writing`, then awaits a write that SlowWriteReactor keeps in flight, until its frame is destroyed.
Task<> WaitInWrite(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* writing) {
  Writer<SlowWriteReactor> writer{*executor, *stub};
  writing->set_value();  // the test queues the destruction behind this resumption, so behind the suspension
  static_cast<void>(co_await writer.Write(PointNumber(1)));
  ADD_FAILURE() << "a destroyed frame never resumes";
}

/// Writes one point, signals `writing`, then never writes again, nor resumes.
Task<> WriteOneThenStall(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* writing) {
  RecordRouteWriter<Delivery::kPush> writer{*executor, *stub};
  static_cast<void>(co_await writer.Write(PointNumber(1)));
  writing->set_value();
  co_await Forever{};
  ADD_FAILURE() << "a destroyed frame never resumes";
}

/// Uploads `points` through a Source, then finishes.
template <Delivery kDelivery, class RangeT>
Task<> Upload(Executor* executor, routeguide::RouteGuide::Stub* stub, RangeT points, Seen* seen,
              std::promise<void>* done) {
  RecordRouteSource<kDelivery> source{*executor, *stub, std::move(points)};
  auto [status, summary] = co_await source.Finish();
  seen->threads.push_back(std::this_thread::get_id());
  seen->status = status;
  seen->point_count = summary.point_count();
  done->set_value();
}

/// Uploads `count` points, hands its source out for the test to cancel, then finishes.
Task<> UploadThenCancel(Executor* executor, routeguide::RouteGuide::Stub* stub, int count,
                        RecordRouteSource<Delivery::kPull>** out, Seen* seen, std::promise<void>* done) {
  RecordRouteSource<Delivery::kPull> source{*executor, *stub, Points(count)};
  *out = &source;
  auto [status, summary] = co_await source.Finish();
  seen->status = status;
  done->set_value();
}

/// Uploads `count` points, then awaits Finish() until its frame is destroyed.
Task<> UploadAndWait(Executor* executor, routeguide::RouteGuide::Stub* stub, int count,
                     std::promise<void>* uploading) {
  RecordRouteSource<Delivery::kPush> source{*executor, *stub, Points(count)};
  uploading->set_value();
  static_cast<void>(co_await source.Finish());
  ADD_FAILURE() << "a destroyed frame never resumes";
}

class ReactorCoroWriterTest : public RouteGuideTestFixtureBase<RecordService> {
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

  /// Waits for the loop to finish what it runs, then reads how many root tasks are left.
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

  /// Destroys a frame once `make_task` signalled, and checks the reactor was released in time, posting nothing
  /// more.
  template <class MakeTask>
  void DestroyOnceSignalled(MakeTask make_task) {
    auto roots = std::make_unique<RootTasks>();
    std::promise<void> signalled;
    OnLoop([&] { roots->Spawn(make_task(&signalled)); });
    ASSERT_EQ(signalled.get_future().wait_for(kTimeout), std::future_status::ready);
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

  /// Writes `count` points through WriteAllThenFinish().
  template <Delivery kDelivery>
  Seen WriteAll(int count) {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) {
      return WriteAllThenFinish<kDelivery>(&executor_, stub_.get(), count, &seen, done);
    });
    return seen;
  }

  /// Uploads `points` through Upload().
  template <Delivery kDelivery, class RangeT>
  Seen UploadAll(RangeT points) {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) {
      return Upload<kDelivery>(&executor_, stub_.get(), std::move(points), &seen, done);
    });
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

/// @test Under kPull, every Write() resumes on the loop with true, Close() gives true, and Finish() gives OK and
/// the summary, swapped out of the reactor, counting every point the server received, in order.
TEST_F(ReactorCoroWriterTest, RecordRoute_PullWritesEveryPoint_ThenFinishGivesTheSummary) {
  const Seen seen = WriteAll<Delivery::kPull>(3);

  EXPECT_EQ(seen.writes, std::vector<bool>(3, true));
  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(4, LoopThread()));
  EXPECT_EQ(seen.closed, true);
  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 3);
  EXPECT_EQ(test_service_.Received(), Numbers(3));
}

/// @test The same under kPush, with the summary moved out of the message the done callback handed over.
TEST_F(ReactorCoroWriterTest, RecordRoute_PushWritesEveryPoint_ThenFinishGivesTheSummary) {
  const Seen seen = WriteAll<Delivery::kPush>(3);

  EXPECT_EQ(seen.writes, std::vector<bool>(3, true));
  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(4, LoopThread()));
  EXPECT_EQ(seen.closed, true);
  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 3);
  EXPECT_EQ(test_service_.Received(), Numbers(3));
}

/// @test No write at all: Close() then Finish() give an empty summary.
TEST_F(ReactorCoroWriterTest, RecordRoute_NoWrite_CloseThenFinishGivesAnEmptySummary) {
  const Seen seen = WriteAll<Delivery::kPull>(0);

  EXPECT_EQ(seen.closed, true);
  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 0);
}

/// @test A write that completes before the coroutine awaits it goes on without suspending, so without a
/// resumption, and the next write still finds the hand-over re-armed.
TEST_F(ReactorCoroWriterTest, RecordRoute_WriteCompletedBeforeTheAwait_GoesOnWithoutSuspending) {
  Seen seen;
  int posts = -1;
  RunToEnd([&](std::promise<void>* done) {
    return WriteCompletingBeforeTheAwait(&executor_, stub_.get(), &executor_, &posts, &seen, done);
  });

  EXPECT_EQ(posts, 0);
  EXPECT_EQ(seen.writes, std::vector<bool>(2, true));
  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 2);
}

/// @test Under kPull, once the server has ended the RPC with an error, a Write() gives false, the next one false
/// at once, Close() false, and Finish() the server's error.
TEST_F(ReactorCoroWriterTest, RecordRoute_PullServerError_WriteGivesFalse_FinishGivesTheError) {
  test_service_.FailAfter(2, grpc::StatusCode::NOT_FOUND);
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return WriteUntilRefused<Delivery::kPull>(&executor_, stub_.get(), &seen, done);
  });

  ASSERT_GE(seen.writes.size(), 4U);  // two accepted, one refused at least, then the one after
  EXPECT_EQ(seen.writes[0], true);
  EXPECT_EQ(seen.writes[1], true);
  EXPECT_EQ(seen.writes[seen.writes.size() - 2], false);
  EXPECT_EQ(seen.writes.back(), false);
  EXPECT_EQ(seen.closed, false);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(test_service_.Received(), Numbers(2));
}

/// @test The same under kPush.
TEST_F(ReactorCoroWriterTest, RecordRoute_PushServerError_WriteGivesFalse_FinishGivesTheError) {
  test_service_.FailAfter(2, grpc::StatusCode::NOT_FOUND);
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return WriteUntilRefused<Delivery::kPush>(&executor_, stub_.get(), &seen, done);
  });

  ASSERT_GE(seen.writes.size(), 4U);
  EXPECT_EQ(seen.writes[seen.writes.size() - 2], false);
  EXPECT_EQ(seen.writes.back(), false);
  EXPECT_EQ(seen.closed, false);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test A write started once the server's error has arrived fails, and that very Write() gives false. Finish(),
/// awaited before Close(), then gives the error without cancelling: the failed write ended the write flow.
TEST_F(ReactorCoroWriterTest, RecordRoute_WriteAfterTheServerErrorArrived_ThatWriteGivesFalse) {
  test_service_.FailAfter(1, grpc::StatusCode::NOT_FOUND);
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return WriteAfterTheServerError<Delivery::kPull>(&executor_, stub_.get(), &seen, done);
  });

  EXPECT_EQ(seen.writes, (std::vector<bool>{true, false}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test After TryCancel(), Write() gives false at once, and Finish() gives CANCELLED.
TEST_F(ReactorCoroWriterTest, RecordRoute_TryCancel_WriteGivesFalse_FinishGivesCancelled) {
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return WriteCancelWrite<Delivery::kPush>(&executor_, stub_.get(), &seen, done);
  });

  EXPECT_EQ(seen.writes, (std::vector<bool>{true, false}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test Under kPull, Finish() awaited before Close() cancels the RPC, which the server would otherwise keep open
/// waiting for the close, and gives CANCELLED.
TEST_F(ReactorCoroWriterTest, RecordRoute_PullFinishBeforeClose_EndsTheRpcCancelled) {
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return WriteSomeThenFinish<Delivery::kPull>(&executor_, stub_.get(), 2, false, &seen, done);
  });

  EXPECT_EQ(seen.writes, std::vector<bool>(2, true));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test The same under kPush, with no write at all.
TEST_F(ReactorCoroWriterTest, RecordRoute_PushFinishBeforeAnyWrite_EndsTheRpcCancelled) {
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return WriteSomeThenFinish<Delivery::kPush>(&executor_, stub_.get(), 0, false, &seen, done);
  });

  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test Finish() awaited before Close(), once the server's error has arrived, gives that error rather than
/// CANCELLED.
TEST_F(ReactorCoroWriterTest, RecordRoute_FinishBeforeCloseAfterTheServerError_GivesTheError) {
  test_service_.FailAfter(1, grpc::StatusCode::NOT_FOUND);
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return WriteSomeThenFinish<Delivery::kPull>(&executor_, stub_.get(), 1, true, &seen, done);
  });

  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test A frame destroyed while its write is in flight cancels the RPC, and destroys the reactor only once
/// OnDone() has run, without posting the destroyed coroutine. AddressSanitizer reports the reactor, or the
/// Writer the write callback reaches, freed under gRPC otherwise.
TEST_F(ReactorCoroWriterTest, RecordRoute_FrameDestroyedMidWrite_CancelsAndWaitsForOnDone) {
  DestroyOnceSignalled([&](std::promise<void>* writing) { return WaitInWrite(&executor_, stub_.get(), writing); });
}

/// @test A frame destroyed between writes, with the write flow open and its hold taken, cancels the RPC, which
/// releases the hold: OnDone() then comes at once, rather than after the timeout that leaks the reactor.
TEST_F(ReactorCoroWriterTest, RecordRoute_FrameDestroyedBetweenWrites_ReleasesTheWriteFlow) {
  DestroyOnceSignalled(
      [&](std::promise<void>* writing) { return WriteOneThenStall(&executor_, stub_.get(), writing); });
}

/// @test Under kPull, a Source sends every element of the vector it was given, in order, then Finish() resumes on
/// the loop with OK and the summary.
TEST_F(ReactorCoroWriterTest, RecordRoute_PullSourceSendsEveryElement_ThenFinishGivesTheSummary) {
  const Seen seen = UploadAll<Delivery::kPull>(Points(5));

  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(1, LoopThread()));
  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 5);
  EXPECT_EQ(test_service_.Received(), Numbers(5));
}

/// @test The same under kPush.
TEST_F(ReactorCoroWriterTest, RecordRoute_PushSourceSendsEveryElement_ThenFinishGivesTheSummary) {
  const Seen seen = UploadAll<Delivery::kPush>(Points(5));

  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 5);
  EXPECT_EQ(test_service_.Received(), Numbers(5));
}

/// @test An empty range: the reactor closes the request stream at once, and Finish() gives an empty summary.
TEST_F(ReactorCoroWriterTest, RecordRoute_SourceOfAnEmptyRange_FinishGivesAnEmptySummary) {
  const Seen seen = UploadAll<Delivery::kPull>(std::vector<routeguide::Point>{});

  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 0);
  EXPECT_TRUE(test_service_.Received().empty());
}

/// @test A range other than a vector, here a std::deque, is copied: every element is sent, in order.
TEST_F(ReactorCoroWriterTest, RecordRoute_SourceOfADeque_SendsEveryElement) {
  const auto points = Points(4);
  const Seen seen = UploadAll<Delivery::kPull>(std::deque<routeguide::Point>(points.begin(), points.end()));

  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 4);
  EXPECT_EQ(test_service_.Received(), Numbers(4));
}

/// @test A view is copied too, its function run on the constructing thread: every element is sent, in order.
TEST_F(ReactorCoroWriterTest, RecordRoute_SourceOfATransformView_SendsEveryElement) {
  const Seen seen = UploadAll<Delivery::kPush>(std::views::iota(1, 4) | std::views::transform(PointNumber));

  EXPECT_TRUE(seen.status.ok());
  EXPECT_EQ(seen.point_count, 3);
  EXPECT_EQ(test_service_.Received(), Numbers(3));
}

/// @test TryCancel() while the server holds the RPC after the last element: Finish() gives CANCELLED.
TEST_F(ReactorCoroWriterTest, RecordRoute_SourceTryCancel_FinishGivesCancelled) {
  test_service_.HoldAtClose();
  RootTasks roots;
  Seen seen;
  RecordRouteSource<Delivery::kPull>* source = nullptr;
  std::promise<void> done;
  OnLoop([&] { roots.Spawn(UploadThenCancel(&executor_, stub_.get(), 3, &source, &seen, &done)); });
  OnLoop([&] { source->TryCancel(); });
  ASSERT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
  EXPECT_EQ(RootsLeft(roots), 0U);

  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test A frame destroyed while its Finish() waits on a server that holds the RPC cancels it, and destroys the
/// reactor only once OnDone() has run, without posting the destroyed coroutine.
TEST_F(ReactorCoroWriterTest, RecordRoute_SourceFrameDestroyedWhileFinishWaits_CancelsAndWaitsForOnDone) {
  test_service_.HoldAtClose();
  DestroyOnceSignalled(
      [&](std::promise<void>* uploading) { return UploadAndWait(&executor_, stub_.get(), 3, uploading); });
}

}  // namespace
