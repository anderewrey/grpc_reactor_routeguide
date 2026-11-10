///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests coro::Reader with its latest-only buffer, the reader of reactor_coro.h over a reactor under Delivery::kPull
/// and ReadPacing::kContinuous, against an in-process server, with coroutines resumed on the AppLoop task queue
/// that stands in for the application's event loop: the newest message kept and every other one counted as
/// overwritten, the last message given once the stream has ended, a waiting Read() woken by a message and by the
/// end of the stream, an early Finish() that drops the message in the slot, and a frame destroyed with a full
/// slot. Every coroutine records the thread it resumes on after each co_await, and every test checks it is the
/// loop's.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
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

using LatestReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>;
using LatestReader = Reader<LatestReactor>;

static_assert(BufferFor(Delivery::kPull, ReadPacing::kContinuous) == Buffer::kLatest);
static_assert(BufferOf(static_cast<LatestReactor*>(nullptr)) == Buffer::kLatest);
static_assert(std::is_same_v<LatestReader::MessageT, std::optional<routeguide::Feature>>);

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
  std::vector<std::string> names;          // of every message Read() gave, in order
  std::vector<std::uint64_t> overwritten;  // Overwritten() after each Read() that gave a message
  std::uint64_t overwritten_at_end = 99;   // Overwritten() after the Read() that gave nothing
  std::vector<std::thread::id> resumed;    // the thread after each co_await
  bool read_after_end_gave_nothing = false;
  grpc::Status status;

  /// Called after each co_await: records the thread the coroutine resumed on.
  void Resumed() { resumed.push_back(std::this_thread::get_id()); }
  /// @return the messages taken plus those overwritten: every message the stream sent, once read to the end
  [[nodiscard]] std::uint64_t Received() const {
    std::uint64_t received = names.size();
    for (const auto n : overwritten) received += n;
    return received;
  }
};

/// A kPull, kContinuous ListFeatures reactor whose reactions the test can observe and hold: it holds the second
/// message back until the test releases it, so the first Read() is known to give the first message, then counts
/// the messages that have reached the reader, and says when its OnDone() has begun. One test at a time uses it,
/// hence its static state.
class HookedReactor final
    : public RpcReactor::Client::ActiveReadReactor<routeguide::Feature, Delivery::kPull, ReadPacing::kContinuous> {
  using Base = RpcReactor::Client::ActiveReadReactor<routeguide::Feature, Delivery::kPull, ReadPacing::kContinuous>;

 public:
  using Callbacks =
      RpcReactor::Client::ActiveReadCallbacks<routeguide::Feature, Delivery::kPull, ReadPacing::kContinuous>;

  HookedReactor(routeguide::RouteGuide::Stub& stub, std::unique_ptr<grpc::ClientContext> context,
                const routeguide::Rectangle& request, Callbacks&& cbs)
      : Base(std::move(context), std::move(cbs)) {
    stub.async()->ListFeatures(this->context_.get(), &request, this);
    this->StartRead(&this->response_);
    this->StartCall();
  }

  /// Starts a test: nothing has arrived, and the second message is held back.
  static void Reset() {
    hold_second_ = true;
    arrived_ = 0;
    done_began_ = false;
  }
  /// Lets the second message reach the reader.
  static void ReleaseSecond() { hold_second_ = false; }
  /// @return once this many messages have reached the reader, or false after kTimeout
  static bool WaitForArrived(int count) { return PollUntil([count] { return arrived_.load() >= count; }); }
  /// @return once OnDone() has begun, with the RPC's status, or false after kTimeout
  static bool WaitForOnDone() { return PollUntil([] { return done_began_.load(); }); }

 protected:
  void OnReadDone(bool ok) override {
    if (ok && ++reads_ == 2) PollUntil([] { return !hold_second_.load(); });
    Base::OnReadDone(ok);
    if (ok) ++arrived_;  // after the read callback put the message in the slot
  }
  void OnDone(const grpc::Status& status) override {
    done_began_ = true;
    Base::OnDone(status);
  }

 private:
  int reads_ = 0;  // only touched by OnReadDone(), whose runs the read between them orders
  static inline std::atomic<bool> hold_second_{true};
  static inline std::atomic<int> arrived_{0};
  static inline std::atomic<bool> done_began_{false};
};
using HookedReader = Reader<HookedReactor>;

/// Reads the whole stream, sleeping `pause` after each message, so a stream faster than that overwrites some,
/// reads once more after its end, then finishes.
template <class ReaderT>
Task<> ReadAll(Executor* executor, routeguide::RouteGuide::Stub* stub, std::chrono::milliseconds pause, Seen* seen,
               std::promise<void>* done) {
  ReaderT reader{*executor, *stub, routeguide::Rectangle{}};
  while (auto feature = co_await reader.Read()) {
    seen->Resumed();
    seen->names.push_back(feature->name());
    seen->overwritten.push_back(reader.Overwritten());
    std::this_thread::sleep_for(pause);
  }
  seen->Resumed();
  seen->overwritten_at_end = reader.Overwritten();
  seen->read_after_end_gave_nothing = !co_await reader.Read();
  seen->Resumed();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Reads one message, lets the second one through, blocks the loop thread until the stream has ended, so every other
/// message reached the slot while no code of the coroutine ran, then reads to the end and finishes.
Task<> ReadOneUntilOnDoneThenReadOn(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                                    std::promise<void>* done) {
  HookedReader reader{*executor, *stub, routeguide::Rectangle{}};
  if (auto first = co_await reader.Read()) {
    seen->names.push_back(first->name());
    seen->overwritten.push_back(reader.Overwritten());
  }
  seen->Resumed();
  HookedReactor::ReleaseSecond();
  EXPECT_TRUE(HookedReactor::WaitForOnDone());
  while (auto feature = co_await reader.Read()) {
    seen->Resumed();
    seen->names.push_back(feature->name());
    seen->overwritten.push_back(reader.Overwritten());
  }
  seen->Resumed();
  seen->overwritten_at_end = reader.Overwritten();
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// What ReadOneThenFinish() waits for before Finish(): OnDone() to begin, so the server's status has arrived.
inline constexpr int kUntilOnDone = 0;

/// Reads one message, lets the second one through, blocks the loop thread until `arrived` messages have reached the
/// slot, or until OnDone() has begun for kUntilOnDone, then awaits Finish() without reading on.
Task<> ReadOneThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, int arrived, Seen* seen,
                         std::promise<void>* done) {
  HookedReader reader{*executor, *stub, routeguide::Rectangle{}};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  seen->Resumed();
  HookedReactor::ReleaseSecond();
  EXPECT_TRUE(arrived == kUntilOnDone ? HookedReactor::WaitForOnDone() : HookedReactor::WaitForArrived(arrived));
  seen->status = co_await reader.Finish();
  seen->Resumed();
  done->set_value();
}

/// Hands its reader out, then reads the whole stream, signalling `first` once it has the first message.
Task<> ReadAllSignallingTheFirst(Executor* executor, routeguide::RouteGuide::Stub* stub, LatestReader** out,
                                 std::promise<void>* first, Seen* seen, std::promise<void>* done) {
  LatestReader reader{*executor, *stub, routeguide::Rectangle{}};
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

/// Reads one message, lets the second one through, blocks the loop thread until two more reached the slot, then never
/// reads again, nor resumes.
Task<> ReadOneUntilTheSlotIsFullThenStall(Executor* executor, routeguide::RouteGuide::Stub* stub,
                                          std::promise<void>* read_one) {
  HookedReader reader{*executor, *stub, routeguide::Rectangle{}};
  static_cast<void>(co_await reader.Read());
  HookedReactor::ReleaseSecond();
  EXPECT_TRUE(HookedReactor::WaitForArrived(3));
  read_one->set_value();
  co_await Forever{};
  ADD_FAILURE() << "a destroyed frame never resumes";
}

/// Reads one message, then waits in Read() for a held stream, until its frame is destroyed.
Task<> ReadOneThenWait(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* read_one) {
  LatestReader reader{*executor, *stub, routeguide::Rectangle{}};
  static_cast<void>(co_await reader.Read());
  read_one->set_value();
  static_cast<void>(co_await reader.Read());
  ADD_FAILURE() << "a destroyed frame never resumes";
}

class ReactorCoroLatestReaderTest : public coro_reader_test::CoroReaderTestBase {
 protected:
  void SetUp() override {
    CoroReaderTestBase::SetUp();
    HookedReactor::Reset();
  }

  /// Runs `make_task` to its end, checks it resumed on the loop each time, and returns what it saw.
  template <class MakeTask>
  Seen Run(MakeTask make_task) {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) { return make_task(&seen, done); });
    ExpectResumedOnTheLoop(seen);
    return seen;
  }

  /// On a gated stream, so the coroutine's first Read() is known to wait: opens the gate once it waits, then
  /// waits up to five seconds for the first message, then cancels, and returns what the coroutine saw.
  Seen OpenTheGateOnAWaitingRead(bool* woken) {
    test_service_.GateFirstMessage();
    RootTasks roots;
    Seen seen;
    LatestReader* reader = nullptr;
    std::promise<void> first;
    std::promise<void> done;
    OnLoop([&] {
      roots.Spawn(ReadAllSignallingTheFirst(&executor_, stub_.get(), &reader, &first, &seen, &done));
      EXPECT_EQ(roots.Size(), 1U);  // Spawn() returned at the first Read(), which waits for the gate
    });
    EXPECT_EQ(executor_.Posts(), 0);
    test_service_.OpenGate();
    *woken = first.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
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
      roots.Spawn(ReadAll<LatestReader>(&executor_, stub_.get(), std::chrono::milliseconds(0), &seen, &done));
      EXPECT_EQ(roots.Size(), 1U);
    });
    test_service_.OpenGate();
    EXPECT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);
    EXPECT_EQ(RootsLeft(roots), 0U);
    ExpectResumedOnTheLoop(seen);
    return seen;
  }

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

/// @return the number in a feature's name, "feature n"
int Number(const std::string& name) { return std::stoi(name.substr(name.find(' ') + 1)); }

/// @return whether every message of a stream of `sent` was either taken or overwritten, in order: each one taken is
///         the one after those Overwritten() reported since the previous one, and the last one taken is the last sent
::testing::AssertionResult TakenOrOverwrittenToTheEnd(const Seen& seen, int sent) {
  int previous = 0;
  for (std::size_t i = 0; i < seen.names.size(); ++i) {
    const int expected = previous + 1 + static_cast<int>(seen.overwritten[i]);
    if (Number(seen.names[i]) != expected) {
      return ::testing::AssertionFailure() << seen.names[i] << " taken, feature " << expected << " expected";
    }
    previous = expected;
  }
  if (previous != sent) return ::testing::AssertionFailure() << "feature " << previous << " taken last, of " << sent;
  return ::testing::AssertionSuccess();
}

/// @test A coroutine slower than the stream takes only the newest messages: every message is either taken or
/// counted as overwritten, the messages taken come in order, the last one is the stream's last, and Finish() gives
/// OK. Two thousand messages, read with a pause, so the read callback overwrites the slot while the coroutine
/// takes from it, which is where ThreadSanitizer finds a slot touched without the lock.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_SlowCoroutine_TakesTheNewest_AndCountsTheOverwritten) {
  constexpr int kMessages = 2000;
  test_service_.Send(kMessages);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadAll<LatestReader>(&executor_, stub_.get(), std::chrono::milliseconds(1), s, done);
  });

  EXPECT_TRUE(TakenOrOverwrittenToTheEnd(seen, kMessages));
  EXPECT_EQ(seen.Received(), static_cast<std::uint64_t>(kMessages));
  EXPECT_LT(seen.names.size(), static_cast<std::size_t>(kMessages));  // the pause let the stream overwrite some
  EXPECT_EQ(seen.overwritten_at_end, 0U);
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test A message the coroutine takes before the next one arrives is not counted as overwritten.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_OneMessage_NoneOverwritten) {
  test_service_.Send(1);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadAll<LatestReader>(&executor_, stub_.get(), std::chrono::milliseconds(0), s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.overwritten, std::vector<std::uint64_t>{0});
  EXPECT_TRUE(seen.status.ok());
}

/// @test An empty stream: the first Read() gives nothing, and Finish() gives OK.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_EmptyStream_FirstReadGivesNothing) {
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadAll<LatestReader>(&executor_, stub_.get(), std::chrono::milliseconds(0), s, done);
  });

  EXPECT_TRUE(seen.names.empty());
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test The last message is never lost: while the coroutine blocks after the first message, the stream sends
/// the others and ends, so the slot ends on the last one, which Read() gives after the end of the stream, with the
/// one before counted as overwritten.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_StreamEndsWhileTheCoroutineBlocks_LastMessageIsStillGiven) {
  test_service_.Send(3);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadOneUntilOnDoneThenReadOn(&executor_, stub_.get(), s, done);
  });

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1", "feature 3"}));
  EXPECT_EQ(seen.overwritten, (std::vector<std::uint64_t>{0, 1}));
  EXPECT_EQ(seen.overwritten_at_end, 0U);
  EXPECT_TRUE(seen.status.ok());
}

/// @test A failed RPC is a value: Read() gives what came before the error, then nothing, and Finish() gives it.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_ServerErrorMidStream_FinishGivesTheError) {
  test_service_.Send(3);
  test_service_.FailAfter(2, grpc::StatusCode::NOT_FOUND);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadAll<LatestReader>(&executor_, stub_.get(), std::chrono::milliseconds(0), s, done);
  });

  EXPECT_EQ(seen.Received(), 2U);
  EXPECT_EQ(seen.names.back(), "feature 2");
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test A Read() waiting on an empty slot resumes on the next message, while the stream is still open: the server
/// sends one message once the Read() waits, then holds the stream.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_WaitingRead_ResumesOnTheNextMessage) {
  test_service_.HoldAfter(1);
  bool woken = false;
  const Seen seen = OpenTheGateOnAWaitingRead(&woken);

  EXPECT_TRUE(woken);
  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test A Read() waiting on an empty slot resumes with nothing at the end of the stream.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_WaitingRead_ResumesWithNothingAtTheEndOfTheStream) {
  const Seen seen = EndTheStreamOnAWaitingRead();

  EXPECT_TRUE(seen.names.empty());
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test An early Finish() with a message in the slot, on a stream the server holds open, drops it, cancels the
/// RPC, which only that cancel can end, and gives CANCELLED, saying a message was dropped.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_FinishWithAFullSlot_DropsItAndGivesCancelled) {
  test_service_.HoldAfter(3);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadOneThenFinish(&executor_, stub_.get(), 3, s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(seen.status.error_message(), HookedReader::kDroppedMessage);
}

/// @test An early Finish() with a message in the slot, on a stream whose OK status has arrived, gives CANCELLED
/// rather than the server's OK: the coroutine did not read the last message.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_FinishWithAFullSlotOfAFinishedStream_GivesCancelled) {
  test_service_.Send(3);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadOneThenFinish(&executor_, stub_.get(), kUntilOnDone, s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(seen.status.error_message(), HookedReader::kDroppedMessage);
}

/// @test An early Finish() with an empty slot, once every message has arrived and the OK status too, gives that
/// OK: nothing was dropped.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_FinishWithAnEmptySlotOfAFinishedStream_GivesOk) {
  test_service_.Send(1);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadOneThenFinish(&executor_, stub_.get(), kUntilOnDone, s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_TRUE(seen.status.ok());
}

/// @test A frame destroyed with a message in the slot, on a stream the server holds open, cancels the RPC,
/// destroys the reactor once OnDone() has run, then frees the slot, posting nothing. AddressSanitizer reports a
/// reactor freed under gRPC, or a slot freed before a callback that still reaches it.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_FrameDestroyedWithAFullSlot_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(3);
  DestroyAfterOneMessage([&](std::promise<void>* read_one) {
    return ReadOneUntilTheSlotIsFullThenStall(&executor_, stub_.get(), read_one);
  });
}

/// @test A frame destroyed while its Read() waits on an empty slot, on a stream the server holds open, does the
/// same, without posting the destroyed coroutine.
TEST_F(ReactorCoroLatestReaderTest, ListFeatures_FrameDestroyedWhileReadWaits_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(1);
  DestroyAfterOneMessage(
      [&](std::promise<void>* read_one) { return ReadOneThenWait(&executor_, stub_.get(), read_one); });
}

}  // namespace
