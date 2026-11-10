///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests coro::Reader with its bounded buffer, the reader of reactor_coro.h over a reactor under Delivery::kPush and
/// ReadPacing::kPerMessage, against an in-process server, with coroutines resumed on the AppLoop task queue that
/// stands in for the application's event loop: every message owned and in order with never more than the capacity
/// queued, the stream held at the capacity and resumed by Read(), a waiting Read() at a capacity of one, the end of
/// a held stream, an early Finish() while the stream is held or a message is in flight, and a frame destroyed with
/// the stream held. Every coroutine records the thread it resumes on after each co_await, and every test checks it
/// is the loop's.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_coro.h"
#include "applications/reactor/tests/coro_reader_test_support.h"

namespace {

using RpcReactor::Client::Delivery;
using RpcReactor::Client::ReadPacing;
using RpcReactor::coro::Buffer;
using RpcReactor::coro::BufferFor;
using RpcReactor::coro::BufferOf;
using RpcReactor::coro::Executor;
using RpcReactor::coro::Reader;
using RpcReactor::coro::RootTasks;
using RpcReactor::coro::Task;
using coro_reader_test::Forever;
using coro_reader_test::kTimeout;

using BoundedReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kPerMessage>;
using BoundedReader = Reader<BoundedReactor>;

static_assert(BufferFor(Delivery::kPush, ReadPacing::kPerMessage) == Buffer::kBounded);
static_assert(BufferOf(static_cast<BoundedReactor*>(nullptr)) == Buffer::kBounded);
static_assert(std::is_same_v<BoundedReader::MessageT, std::optional<routeguide::Feature>>);

/// Polls `condition` every 5 milliseconds, for kTimeout at most.
/// @return whether it held in time
template <class F>
bool PollUntil(F condition) {
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (!condition()) {
    if (std::chrono::steady_clock::now() > deadline) return false;
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
  return true;
}

/// What a test coroutine saw, written on the loop, read by the test once `done` is ready.
struct Seen {
  std::vector<std::string> names;        // of every message Read() gave, in order
  std::vector<std::thread::id> resumed;  // the thread after each co_await
  std::size_t most_queued = 0;           // the largest Queued() seen
  std::size_t queued_while_held = 0;     // Queued() once the stream has been held a while
  int arrived_while_held = 0;            // messages that reached the reader by then
  std::uint64_t holds = 0;               // Holds() at the end of the stream
  std::chrono::steady_clock::duration finish_took{};  // from an early Finish() to its resumption
  bool read_after_end_gave_nothing = false;
  grpc::Status status;

  /// Called after each co_await: records the thread the coroutine resumed on.
  void Resumed() { resumed.push_back(std::this_thread::get_id()); }
};

/// A kPush, kPerMessage ListFeatures reactor whose reactions the test can observe and hold: it counts the messages
/// that have reached the reader, and can hold the second one back, before the reactor hands it over, until the
/// test releases it. One test at a time uses it, hence its static state.
class HookedReactor final
    : public RpcReactor::Client::ActiveReadReactor<routeguide::Feature, Delivery::kPush, ReadPacing::kPerMessage> {
  using Base = RpcReactor::Client::ActiveReadReactor<routeguide::Feature, Delivery::kPush, ReadPacing::kPerMessage>;

 public:
  using Callbacks =
      RpcReactor::Client::ActiveReadCallbacks<routeguide::Feature, Delivery::kPush, ReadPacing::kPerMessage>;

  HookedReactor(routeguide::RouteGuide::Stub& stub, std::unique_ptr<grpc::ClientContext> context,
                const routeguide::Rectangle& request, Callbacks&& cbs)
      : Base(std::move(context), std::move(cbs)) {
    stub.async()->ListFeatures(this->context_.get(), &request, this);
    this->StartRead(&this->response_);
    this->StartCall();
  }

  /// Starts a test: nothing has arrived, and the second message is held back or not.
  static void Reset(bool hold_second) {
    hold_second_ = hold_second;
    second_arrived_ = false;
    arrived_ = 0;
  }
  /// Lets the second message reach the reader.
  static void ReleaseSecond() { hold_second_ = false; }
  /// @return once the second message has arrived, before the reader has it, or false after kTimeout
  static bool WaitForTheSecond() { return PollUntil([] { return second_arrived_.load(); }); }
  /// @return the messages that reached the reader so far
  static int Arrived() { return arrived_.load(); }

 protected:
  void OnReadDone(bool ok) override {
    if (ok) {
      if (++reads_ == 2) {
        second_arrived_ = true;
        PollUntil([] { return !hold_second_.load(); });
      }
      ++arrived_;  // before the hand-over: the reader may arm the next read before this reaction returns
    }
    Base::OnReadDone(ok);
  }

 private:
  int reads_ = 0;  // only touched by OnReadDone(), whose runs the read between them orders
  static inline std::atomic<bool> hold_second_{false};
  static inline std::atomic<bool> second_arrived_{false};
  static inline std::atomic<int> arrived_{0};
};
using HookedReader = Reader<HookedReactor>;

/// How ReadAll() reads: the reader's capacity, and how often the coroutine pauses, never when 0.
struct Pacing {
  std::size_t capacity;
  std::size_t pause_every;
};

/// Reads the whole stream, blocking after the first message until the queue is full, so the stream is held at least
/// once, then pausing a millisecond every `pacing.pause_every` messages, never when 0, and records the largest
/// backlog, reads once more after the end, then finishes.
Task<> ReadAll(Executor* executor, routeguide::RouteGuide::Stub* stub, Pacing pacing, Seen* seen,
               std::promise<void>* done) {
  BoundedReader reader{*executor, *stub, routeguide::Rectangle{}, pacing.capacity};
  while (auto feature = co_await reader.Read()) {
    seen->Resumed();
    seen->names.push_back(feature->name());
    if (pacing.pause_every > 0 && seen->names.size() == 1) {
      EXPECT_TRUE(PollUntil([&] { return reader.Queued() >= pacing.capacity; }));
    }
    if (pacing.pause_every > 0 && seen->names.size() % pacing.pause_every == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    seen->most_queued = std::max(seen->most_queued, reader.Queued());
  }
  seen->Resumed();
  seen->holds = reader.Holds();
  seen->read_after_end_gave_nothing = !co_await reader.Read();
  seen->Resumed();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Reads one message, blocks the loop thread until the queue is full, then a while longer, recording what the
/// stream did meanwhile, then reads to the end and finishes.
Task<> ReadOneHeldAWhileThenReadOn(Executor* executor, routeguide::RouteGuide::Stub* stub, std::size_t capacity,
                                   Seen* seen, std::promise<void>* done) {
  HookedReader reader{*executor, *stub, routeguide::Rectangle{}, capacity};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  seen->Resumed();
  EXPECT_TRUE(PollUntil([&] { return reader.Queued() == capacity; }));
  // Long enough for a stream that is not held to send the rest.
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  seen->queued_while_held = reader.Queued();
  seen->arrived_while_held = HookedReactor::Arrived();
  while (auto feature = co_await reader.Read()) {
    seen->Resumed();
    seen->names.push_back(feature->name());
    seen->most_queued = std::max(seen->most_queued, reader.Queued());
  }
  seen->Resumed();
  seen->holds = reader.Holds();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Reads one message, blocks the loop thread until the queue is full, so the stream is held, then awaits Finish().
Task<> ReadOneUntilHeldThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, std::size_t capacity,
                                  Seen* seen, std::promise<void>* done) {
  BoundedReader reader{*executor, *stub, routeguide::Rectangle{}, capacity};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  seen->Resumed();
  EXPECT_TRUE(PollUntil([&] { return reader.Queued() == capacity && reader.Holds() > 0; }));
  seen->queued_while_held = reader.Queued();
  const auto finish = std::chrono::steady_clock::now();
  seen->status = co_await reader.Finish();
  seen->finish_took = std::chrono::steady_clock::now() - finish;
  seen->Resumed();
  done->set_value();
}

/// Reads one message, waits until the second one has arrived and is held back before the reader has it, calls
/// Finish(), which closes the queue, then lets the second one through, and awaits the end.
Task<> FinishWhileTheSecondIsInFlight(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                                      std::promise<void>* done) {
  HookedReader reader{*executor, *stub, routeguide::Rectangle{}, 2};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  seen->Resumed();
  EXPECT_TRUE(HookedReactor::WaitForTheSecond());
  auto finish = reader.Finish();
  HookedReactor::ReleaseSecond();
  seen->status = co_await finish;
  seen->Resumed();
  done->set_value();
}

/// Reads one message, blocks the loop thread until the queue is full, then never reads again, nor resumes.
Task<> ReadOneUntilHeldThenStall(Executor* executor, routeguide::RouteGuide::Stub* stub, std::size_t capacity,
                                 std::promise<void>* read_one) {
  BoundedReader reader{*executor, *stub, routeguide::Rectangle{}, capacity};
  static_cast<void>(co_await reader.Read());
  EXPECT_TRUE(PollUntil([&] { return reader.Queued() == capacity && reader.Holds() > 0; }));
  read_one->set_value();
  co_await Forever{};
  ADD_FAILURE() << "a destroyed frame never resumes";
}

/// Reads one message, then waits in Read() for a held stream, until its frame is destroyed.
Task<> ReadOneThenWait(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* read_one) {
  BoundedReader reader{*executor, *stub, routeguide::Rectangle{}, 2};
  static_cast<void>(co_await reader.Read());
  read_one->set_value();
  static_cast<void>(co_await reader.Read());
  ADD_FAILURE() << "a destroyed frame never resumes";
}

class ReactorCoroBoundedReaderTest : public coro_reader_test::CoroReaderTestBase {
 protected:
  void SetUp() override {
    CoroReaderTestBase::SetUp();
    HookedReactor::Reset(false);
  }

  /// Runs `make_task` to its end, checks it resumed on the loop each time, and returns what it saw.
  template <class MakeTask>
  Seen Run(MakeTask make_task) {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) { return make_task(&seen, done); });
    ExpectResumedOnTheLoop(seen);
    return seen;
  }

  /// Every co_await of the coroutine resumed it on the loop's thread.
  void ExpectResumedOnTheLoop(const Seen& seen) {
    EXPECT_FALSE(seen.resumed.empty());
    EXPECT_EQ(seen.resumed, std::vector<std::thread::id>(seen.resumed.size(), LoopThread()));
  }

  /// On a gated stream, so the coroutine's first Read() is known to wait: opens the gate once it waits, then reads
  /// the whole stream at this capacity, and returns what the coroutine saw.
  Seen ReadAllAfterAWaitingRead(std::size_t capacity) {
    test_service_.GateFirstMessage();
    RootTasks roots;
    Seen seen;
    std::promise<void> done;
    OnLoop([&] {
      roots.Spawn(ReadAll(&executor_, stub_.get(), Pacing{capacity, 0}, &seen, &done));
      EXPECT_EQ(roots.Size(), 1U);  // Spawn() returned at the first Read(), which waits for the gate
    });
    EXPECT_EQ(executor_.Posts(), 0);
    test_service_.OpenGate();
    EXPECT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_EQ(RootsLeft(roots), 0U);
    ExpectResumedOnTheLoop(seen);
    return seen;
  }

  /// An early Finish() once the stream is held: checks it gave CANCELLED, saying it dropped the queue, in well
  /// under the time DestroyAfterOnDone() waits before it leaks the reactor.
  void ExpectFinishWhileHeldEndsCancelled(std::size_t capacity) {
    const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
      return ReadOneUntilHeldThenFinish(&executor_, stub_.get(), capacity, s, done);
    });
    EXPECT_LT(seen.finish_took, std::chrono::seconds(3));
    EXPECT_EQ(seen.names, std::vector<std::string>{"feature 1"});
    EXPECT_EQ(seen.queued_while_held, capacity);
    EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
    EXPECT_EQ(seen.status.error_message(), BoundedReader::kDroppedMessage);
  }
};

/// The names "feature 1" to "feature n".
std::vector<std::string> Names(int n) {
  std::vector<std::string> names;
  for (int i = 1; i <= n; ++i) names.push_back("feature " + std::to_string(i));
  return names;
}

/// @test Read() gives every message, owned, in order and on the loop, the queue never holds more than the
/// capacity, and the stream is held along the way, then Read() gives nothing, and Finish() gives OK. Two thousand
/// messages, so the read callback fills the queue while the coroutine drains it and releases the hold, which is
/// where ThreadSanitizer finds a queue touched without the lock.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_ReadsEveryMessageInOrder_NeverMoreThanTheCapacityQueued) {
  constexpr int kMessages = 2000;
  test_service_.Send(kMessages);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadAll(&executor_, stub_.get(), Pacing{3, 50}, s, done);
  });

  EXPECT_EQ(seen.names, Names(kMessages));
  EXPECT_LE(seen.most_queued, 3U);
  EXPECT_GT(seen.holds, 0U);
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test An empty stream: the first Read() gives nothing, and Finish() gives OK.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_EmptyStream_FirstReadGivesNothing) {
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadAll(&executor_, stub_.get(), Pacing{3, 0}, s, done);
  });

  EXPECT_TRUE(seen.names.empty());
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test The stream pauses at the capacity and resumes on Read(): while the coroutine blocks after the first
/// message, the reader takes two more, then holds the stream, so nothing else arrives however long it blocks;
/// reading on resumes it, and every message comes, in order.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_SlowCoroutine_StreamPausesAtTheCapacity_AndResumesOnRead) {
  test_service_.Send(6);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadOneHeldAWhileThenReadOn(&executor_, stub_.get(), 2, s, done);
  });

  EXPECT_EQ(seen.queued_while_held, 2U);
  EXPECT_EQ(seen.arrived_while_held, 3);
  EXPECT_EQ(seen.names, Names(6));
  EXPECT_LE(seen.most_queued, 2U);
  EXPECT_GE(seen.holds, 1U);
  EXPECT_TRUE(seen.status.ok());
}

/// @test At a capacity of one, every message holds the stream, and a Read() waiting on the empty queue is the one
/// that takes it, so the Read() woken by a message must release the hold too: the gate keeps the first Read()
/// waiting. Reading never stalls, and every message comes.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_CapacityOne_WokenReadReleasesTheHold) {
  test_service_.Send(3);
  const Seen seen = ReadAllAfterAWaitingRead(1);

  EXPECT_EQ(seen.names, Names(3));
  EXPECT_EQ(seen.holds, 3U);
  EXPECT_TRUE(seen.status.ok());
}

/// @test A stream that ends with an error while held still gives every message queued, then nothing, and Finish()
/// gives the error: the end is reported once Read() has released the hold.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_ErrorWhileHeld_DrainsTheQueueThenFinishGivesTheError) {
  test_service_.Send(5);
  test_service_.FailAfter(4, grpc::StatusCode::NOT_FOUND);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadOneHeldAWhileThenReadOn(&executor_, stub_.get(), 2, s, done);
  });

  EXPECT_EQ(seen.queued_while_held, 2U);
  EXPECT_EQ(seen.names, Names(4));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test An early Finish() while the stream is held at a capacity of two drops the queue, releases the hold, ends
/// the RPC, and gives CANCELLED, without hanging: a hold left in place would keep OnDone() from ever coming.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_FinishWhileHeld_GivesCancelled_WithoutHanging) {
  test_service_.Send(10);
  ExpectFinishWhileHeldEndsCancelled(2);
}

/// @test The same at a capacity of one, on a stream the server holds open, which only the cancel can end.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_FinishWhileHeldAtCapacityOneOnAnOpenStream_GivesCancelled) {
  test_service_.HoldAfter(10);
  ExpectFinishWhileHeldEndsCancelled(1);
}

/// @test A message in flight when an early Finish() closes the queue, held back before the reader has it, is
/// dropped, holds nothing, and the status says so.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_MessageInFlightAtAnEarlyFinish_IsDroppedAndGivesCancelled) {
  test_service_.Send(3);
  HookedReactor::Reset(true);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return FinishWhileTheSecondIsInFlight(&executor_, stub_.get(), s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(seen.status.error_message(), HookedReader::kDroppedMessage);
}

/// @test A frame destroyed with the stream held cancels the RPC, releases the hold, destroys the reactor once
/// OnDone() has run, then frees the queue, posting nothing. AddressSanitizer reports a reactor freed under gRPC,
/// or a queue freed before a callback that still reaches it.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_FrameDestroyedWithTheStreamHeld_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(10);
  DestroyAfterOneMessage(
      [&](std::promise<void>* read_one) { return ReadOneUntilHeldThenStall(&executor_, stub_.get(), 2, read_one); });
}

/// @test A frame destroyed while its Read() waits on an empty queue, on a stream the server holds open, does the
/// same, without posting the destroyed coroutine.
TEST_F(ReactorCoroBoundedReaderTest, ListFeatures_FrameDestroyedWhileReadWaits_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(1);
  DestroyAfterOneMessage(
      [&](std::promise<void>* read_one) { return ReadOneThenWait(&executor_, stub_.get(), read_one); });
}

}  // namespace
