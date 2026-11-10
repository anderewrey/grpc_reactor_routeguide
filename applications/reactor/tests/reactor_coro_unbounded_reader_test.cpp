///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests coro::Reader with its unbounded buffer, the reader of reactor_coro.h over a reactor under Delivery::kPush
/// and ReadPacing::kContinuous, against an in-process server, with coroutines resumed on the AppLoop task queue
/// that stands in for the application's event loop: every message owned and in order, the stream running ahead
/// of a slow coroutine, a waiting Read() woken by a message and by the end of the stream, an early Finish() that
/// drops the messages queued, and those still to come, and a frame destroyed with messages queued. Every
/// coroutine records the thread it resumes on after each co_await, and every test checks it is the loop's.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
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

template <Delivery kDelivery, ReadPacing kPacing>
using ListFeaturesReactor = routeguide::ListFeatures::ClientReactor<kDelivery, kPacing>;
using UnboundedReactor = ListFeaturesReactor<Delivery::kPush, ReadPacing::kContinuous>;
using UnboundedReader = Reader<UnboundedReactor>;

// The reactor's type chooses the buffer, and the combinations with no reader map to none. A Reader over one of
// them does not compile, as its primary template's static_assert says: a test cannot show that, but the buffer
// it rests on is checked here.
static_assert(BufferFor(Delivery::kPull, ReadPacing::kTurnByTurn) == Buffer::kNone);
static_assert(BufferFor(Delivery::kPush, ReadPacing::kTurnByTurn) == Buffer::kNone);
static_assert(BufferFor(Delivery::kPush, ReadPacing::kContinuous) == Buffer::kUnbounded);
static_assert(BufferFor(Delivery::kPull, ReadPacing::kContinuous) == Buffer::kLatest);
static_assert(BufferFor(Delivery::kPush, ReadPacing::kPerMessage) == Buffer::kBounded);
static_assert(!BufferFor(Delivery::kPull, ReadPacing::kPerMessage).has_value());
static_assert(BufferOf(static_cast<UnboundedReactor*>(nullptr)) == Buffer::kUnbounded);
static_assert(!BufferOf(static_cast<ListFeaturesReactor<Delivery::kPull, ReadPacing::kPerMessage>*>(nullptr)));
static_assert(!BufferOf(static_cast<routeguide::Feature*>(nullptr)));
// What Read() gives, as for the reader without a buffer.
static_assert(std::is_same_v<UnboundedReader::MessageT, std::optional<routeguide::Feature>>);

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
  bool read_after_end_gave_nothing = false;
  std::size_t queued_after_pause = 0;  // Queued() once the coroutine paused
  int posts_after_pause = -1;          // suspensions while reading the messages queued during the pause
  grpc::Status status;

  /// Called after each co_await: records the thread the coroutine resumed on.
  void Resumed() { resumed.push_back(std::this_thread::get_id()); }
};

/// A kPush, kContinuous ListFeatures reactor whose reactions the test can observe and hold: it says when its
/// OnDone() has begun, so the server's status has arrived, and when its second message has arrived, which it can
/// hold back from the reader until the test releases it. One test at a time uses it, hence its static state.
class HookedReactor final
    : public RpcReactor::Client::ActiveReadReactor<routeguide::Feature, Delivery::kPush, ReadPacing::kContinuous> {
  using Base = RpcReactor::Client::ActiveReadReactor<routeguide::Feature, Delivery::kPush, ReadPacing::kContinuous>;

 public:
  using Callbacks =
      RpcReactor::Client::ActiveReadCallbacks<routeguide::Feature, Delivery::kPush, ReadPacing::kContinuous>;

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
    done_began_ = false;
  }
  /// Lets the second message reach the reader.
  static void ReleaseSecond() { hold_second_ = false; }
  /// @return once the second message has arrived, or false after kTimeout
  static bool WaitForTheSecond() { return PollUntil([] { return second_arrived_.load(); }); }
  /// @return once OnDone() has begun, with the RPC's status, or false after kTimeout
  static bool WaitForOnDone() { return PollUntil([] { return done_began_.load(); }); }

 protected:
  void OnReadDone(bool ok) override {
    if (ok && ++reads_ == 2) {
      second_arrived_ = true;
      PollUntil([] { return !hold_second_.load(); });
    }
    Base::OnReadDone(ok);
  }
  void OnDone(const grpc::Status& status) override {
    done_began_ = true;
    Base::OnDone(status);
  }

 private:
  int reads_ = 0;  // only touched by OnReadDone(), whose runs the read between them orders
  static inline std::atomic<bool> hold_second_{false};
  static inline std::atomic<bool> second_arrived_{false};
  static inline std::atomic<bool> done_began_{false};
};
using HookedReader = Reader<HookedReactor>;

/// Reads the whole stream, reads once more after its end, then finishes.
Task<> ReadAll(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen, std::promise<void>* done) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  while (auto feature = co_await reader.Read()) {
    seen->Resumed();
    seen->names.push_back(feature->name());
  }
  seen->Resumed();
  seen->read_after_end_gave_nothing = !co_await reader.Read();
  seen->Resumed();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Reads one message of a three-message stream, then blocks the loop thread, so no code of the coroutine runs
/// while the stream goes on, until the other two are queued, then reads them, counting the suspensions that
/// takes, then reads to the end and finishes.
Task<> ReadOnePauseThenReadOn(coro_reader_test::CountingExecutor* executor, routeguide::RouteGuide::Stub* stub,
                              Seen* seen, std::promise<void>* done) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  seen->Resumed();
  PollUntil([&] { return reader.Queued() == 2; });
  seen->queued_after_pause = reader.Queued();
  const int posts = executor->Posts();
  for (int i = 0; i < 2; ++i) {
    if (auto feature = co_await reader.Read()) seen->names.push_back(feature->name());
    seen->Resumed();
  }
  seen->posts_after_pause = executor->Posts() - posts;
  while (auto feature = co_await reader.Read()) {
    seen->Resumed();
    seen->names.push_back(feature->name());
  }
  seen->Resumed();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Reads one message of a stream the server holds open after three, blocks the loop thread until the other two
/// are queued, then awaits Finish().
Task<> ReadOneUntilQueuedThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                                    std::promise<void>* done) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  seen->Resumed();
  PollUntil([&] { return reader.Queued() == 2; });
  seen->queued_after_pause = reader.Queued();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Reads `reads` messages of a three-message stream, blocks the loop thread until OnDone() has begun, so the
/// server's status has arrived and the other messages are queued, then awaits Finish() without reading on.
Task<> ReadSomeUntilOnDoneThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, int reads, Seen* seen,
                                     std::promise<void>* done) {
  HookedReader reader{*executor, *stub, routeguide::Rectangle{}};
  for (int i = 0; i < reads; ++i) {
    if (auto feature = co_await reader.Read()) seen->names.push_back(feature->name());
    seen->Resumed();
  }
  EXPECT_TRUE(HookedReactor::WaitForOnDone());
  seen->queued_after_pause = reader.Queued();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Reads one message, waits until the second one has arrived and is held back from the reader, calls Finish(),
/// which closes the queue, then lets the second one through, and awaits the end.
Task<> FinishWhileTheSecondIsHeld(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                                  std::promise<void>* done) {
  HookedReader reader{*executor, *stub, routeguide::Rectangle{}};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  seen->Resumed();
  EXPECT_TRUE(HookedReactor::WaitForTheSecond());
  seen->queued_after_pause = reader.Queued();
  auto finish = reader.Finish();
  HookedReactor::ReleaseSecond();
  seen->status = co_await finish;
  seen->Resumed();
  done->set_value();
}

/// Hands its reader out, then reads the whole stream, signalling `first` once it has the first message.
Task<> ReadAllSignallingTheFirst(Executor* executor, routeguide::RouteGuide::Stub* stub, UnboundedReader** out,
                                 std::promise<void>* first, Seen* seen, std::promise<void>* done) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  *out = &reader;
  while (auto feature = co_await reader.Read()) {
    seen->Resumed();
    seen->names.push_back(feature->name());
    if (seen->names.size() == 1) first->set_value();
  }
  seen->Resumed();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Reads one message, blocks the loop thread until the next two are queued, then never reads again, nor resumes.
Task<> ReadOneUntilQueuedThenStall(Executor* executor, routeguide::RouteGuide::Stub* stub,
                                   std::promise<void>* read_one) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  static_cast<void>(co_await reader.Read());
  EXPECT_TRUE(PollUntil([&] { return reader.Queued() == 2; }));
  read_one->set_value();
  co_await Forever{};
  ADD_FAILURE() << "a destroyed frame never resumes";
}

/// Reads one message, then waits in Read() for a held stream, until its frame is destroyed.
Task<> ReadOneThenWait(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* read_one) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  static_cast<void>(co_await reader.Read());
  read_one->set_value();
  static_cast<void>(co_await reader.Read());
  ADD_FAILURE() << "a destroyed frame never resumes";
}

class ReactorCoroUnboundedReaderTest : public coro_reader_test::CoroReaderTestBase {
 protected:
  /// Runs `make_task` to its end, checks it resumed on the loop each time, and returns what it saw.
  template <class MakeTask>
  Seen Run(MakeTask make_task) {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) { return make_task(&seen, done); });
    ExpectResumedOnTheLoop(seen);
    return seen;
  }

  /// On a gated stream, so the coroutine's first Read() is known to wait: opens the gate once it waits, then
  /// waits up to `wake` for the first message, then cancels, and returns what the coroutine saw.
  Seen OpenTheGateOnAWaitingRead(std::chrono::seconds wake, bool* woken) {
    test_service_.GateFirstMessage();
    RootTasks roots;
    Seen seen;
    UnboundedReader* reader = nullptr;
    std::promise<void> first;
    std::promise<void> done;
    OnLoop([&] {
      roots.Spawn(ReadAllSignallingTheFirst(&executor_, stub_.get(), &reader, &first, &seen, &done));
      // Spawn() returns at the first suspension: the first Read(), since the gate holds every message.
      EXPECT_EQ(roots.Size(), 1U);
    });
    EXPECT_EQ(executor_.Posts(), 0);
    test_service_.OpenGate();
    *woken = first.get_future().wait_for(wake) == std::future_status::ready;
    OnLoop([&] { reader->TryCancel(); });
    EXPECT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_EQ(RootsLeft(roots), 0U);
    ExpectResumedOnTheLoop(seen);
    return seen;
  }

  /// On a gated stream with no message, so the coroutine's first Read() is known to wait: opens the gate once it
  /// waits, which ends the stream, and returns what the coroutine saw.
  Seen EndTheStreamOnAWaitingRead() {
    test_service_.GateFirstMessage();
    RootTasks roots;
    Seen seen;
    std::promise<void> done;
    OnLoop([&] {
      roots.Spawn(ReadAll(&executor_, stub_.get(), &seen, &done));
      EXPECT_EQ(roots.Size(), 1U);
    });
    test_service_.OpenGate();
    EXPECT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_EQ(RootsLeft(roots), 0U);
    ExpectResumedOnTheLoop(seen);
    return seen;
  }

 private:
  /// Every co_await of the coroutine resumed it on the loop's thread.
  void ExpectResumedOnTheLoop(const Seen& seen) {
    EXPECT_FALSE(seen.resumed.empty());
    EXPECT_EQ(seen.resumed, std::vector<std::thread::id>(seen.resumed.size(), LoopThread()));
  }
};

/// The names "feature 1" to "feature n".
std::vector<std::string> Names(int n) {
  std::vector<std::string> names;
  for (int i = 1; i <= n; ++i) names.push_back("feature " + std::to_string(i));
  return names;
}

/// @test Read() gives every message, owned, in order and on the loop, while the stream runs at the server's pace,
/// then nothing at the end of the stream, and nothing again after, and Finish() gives OK. Two thousand messages,
/// so the read callback fills the queue while the coroutine drains it, which is where ThreadSanitizer finds a
/// queue touched without the lock.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_ReadsEveryMessageInOrder_ThenNothing_ThenFinishOk) {
  constexpr int kMessages = 2000;
  test_service_.Send(kMessages);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) { return ReadAll(&executor_, stub_.get(), s, done); });

  EXPECT_EQ(seen.names, Names(kMessages));
  EXPECT_EQ(seen.resumed.size(), kMessages + 3U);
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test An empty stream: the first Read() gives nothing, and Finish() gives OK.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_EmptyStream_FirstReadGivesNothing) {
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) { return ReadAll(&executor_, stub_.get(), s, done); });

  EXPECT_TRUE(seen.names.empty());
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test A failed RPC is a value: Read() gives the messages sent before the error, then nothing, and Finish()
/// gives the error.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_ServerErrorMidStream_ReadsWhatCameThenFinishGivesTheError) {
  test_service_.Send(3);
  test_service_.FailAfter(2, grpc::StatusCode::NOT_FOUND);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) { return ReadAll(&executor_, stub_.get(), s, done); });

  EXPECT_EQ(seen.names, Names(2));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test The stream runs ahead of a slow coroutine: while it blocks after the first message, the rest of the
/// stream is queued, so it reads them on without suspending once, and Finish() gives OK.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_StreamRunsAheadOfASlowCoroutine_MessagesQueue) {
  test_service_.Send(3);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadOnePauseThenReadOn(&executor_, stub_.get(), s, done);
  });

  EXPECT_EQ(seen.names, Names(3));
  EXPECT_EQ(seen.queued_after_pause, 2U);
  EXPECT_EQ(seen.posts_after_pause, 0);
  EXPECT_TRUE(seen.status.ok());
}

/// @test A Read() waiting on an empty queue resumes on the next message, while the stream is still open: the
/// server sends one message once the Read() waits, then holds the stream. A wake-up left to the end of the stream
/// would come only with the cancel.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_WaitingRead_ResumesOnTheNextMessage) {
  test_service_.HoldAfter(1);
  bool woken = false;
  const Seen seen = OpenTheGateOnAWaitingRead(std::chrono::seconds(5), &woken);

  EXPECT_TRUE(woken);
  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test A Read() waiting on an empty queue resumes with nothing at the end of the stream: the server finishes
/// with no message once the Read() waits.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_WaitingRead_ResumesWithNothingAtTheEndOfTheStream) {
  const Seen seen = EndTheStreamOnAWaitingRead();

  EXPECT_TRUE(seen.names.empty());
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test An early Finish(), with messages queued on a stream whose OK status has arrived, drops them and gives
/// CANCELLED rather than the server's OK: the coroutine did not read every message.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FinishWithMessagesQueued_DropsThemAndGivesCancelled) {
  test_service_.Send(3);
  HookedReactor::Reset(false);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadSomeUntilOnDoneThenFinish(&executor_, stub_.get(), 1, s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.queued_after_pause, 2U);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(seen.status.error_message(), HookedReader::kDroppedMessage);
}

/// @test An early Finish(), with messages queued on a stream the server holds open, cancels the RPC, which only
/// that cancel can end, and gives CANCELLED.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FinishWithMessagesQueuedOnAHeldStream_EndsTheRpcCancelled) {
  test_service_.HoldAfter(3);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadOneUntilQueuedThenFinish(&executor_, stub_.get(), s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.queued_after_pause, 2U);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test An early Finish(), once every message is read, on a stream whose OK status has arrived, gives that OK:
/// nothing was dropped.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FinishAfterEveryMessageOfAFinishedStream_GivesOk) {
  test_service_.Send(3);
  HookedReactor::Reset(false);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadSomeUntilOnDoneThenFinish(&executor_, stub_.get(), 3, s, done);
  });

  EXPECT_EQ(seen.names, Names(3));
  EXPECT_EQ(seen.queued_after_pause, 0U);
  EXPECT_TRUE(seen.status.ok());
}

/// @test A message that arrives once an early Finish() has found the queue empty is dropped too, and the status
/// says so: the reactor holds the second message back until Finish() has closed the queue.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_MessageArrivingAfterAnEarlyFinish_IsDroppedAndGivesCancelled) {
  test_service_.Send(2);
  HookedReactor::Reset(true);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return FinishWhileTheSecondIsHeld(&executor_, stub_.get(), s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.queued_after_pause, 0U);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(seen.status.error_message(), HookedReader::kDroppedMessage);
}

/// @test A frame destroyed with messages queued, on a stream the server holds open, cancels the RPC, destroys the
/// reactor once OnDone() has run, then frees the queue, posting nothing. AddressSanitizer reports a reactor freed
/// under gRPC, or a queue freed before a callback that still reaches it.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FrameDestroyedWithMessagesQueued_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(3);
  DestroyAfterOneMessage(
      [&](std::promise<void>* read_one) { return ReadOneUntilQueuedThenStall(&executor_, stub_.get(), read_one); });
}

/// @test A frame destroyed while its Read() waits on an empty queue, on a stream the server holds open, does the
/// same, without posting the destroyed coroutine.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FrameDestroyedWhileReadWaits_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(1);
  DestroyAfterOneMessage(
      [&](std::promise<void>* read_one) { return ReadOneThenWait(&executor_, stub_.get(), read_one); });
}

}  // namespace
