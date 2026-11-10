///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests coro::Reader with its unbounded buffer, the reader of reactor_coro.h over a reactor under Delivery::kPush
/// and ReadPacing::kContinuous, against an in-process server, with coroutines resumed on the AppLoop task queue
/// that stands in for the application's event loop: every message owned and in order, the stream running ahead
/// of a slow coroutine, a waiting Read() woken by a message and by the end of the stream, an early Finish() that
/// drops the messages queued, and a frame destroyed with messages queued.

#include <gtest/gtest.h>

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

// The reactor's type chooses the buffer, and every combination with no reader is refused: a Reader over it
// does not compile, as its primary template's static_assert says.
static_assert(BufferFor(Delivery::kPull, ReadPacing::kTurnByTurn) == Buffer::kNone);
static_assert(BufferFor(Delivery::kPush, ReadPacing::kTurnByTurn) == Buffer::kNone);
static_assert(BufferFor(Delivery::kPush, ReadPacing::kContinuous) == Buffer::kUnbounded);
static_assert(!BufferFor(Delivery::kPull, ReadPacing::kContinuous).has_value());
static_assert(!BufferFor(Delivery::kPull, ReadPacing::kPerMessage).has_value());
static_assert(!BufferFor(Delivery::kPush, ReadPacing::kPerMessage).has_value());
static_assert(BufferOf(static_cast<UnboundedReactor*>(nullptr)) == Buffer::kUnbounded);
static_assert(!BufferOf(static_cast<ListFeaturesReactor<Delivery::kPull, ReadPacing::kContinuous>*>(nullptr)));
static_assert(!BufferOf(static_cast<routeguide::Feature*>(nullptr)));
// What Read() gives, as for the reader without a buffer.
static_assert(std::is_same_v<UnboundedReader::MessageT, std::optional<routeguide::Feature>>);

/// What a test coroutine saw, written on the loop, read by the test once `done` is ready.
struct Seen {
  std::vector<std::string> names;        // of every message Read() gave, in order
  std::vector<std::thread::id> threads;  // each message was given on
  bool read_after_end_gave_nothing = false;
  std::size_t queued_after_pause = 0;  // Queued() once the coroutine paused
  int posts_after_pause = -1;          // suspensions while reading on after the pause
  grpc::Status status;
};

/// Pauses the loop thread, so no code of the coroutine runs while the stream goes on: 200 milliseconds, then on
/// until `queued` messages wait, should a loaded machine need longer.
void PauseUntilQueued(const UnboundedReader& reader, std::size_t queued) {
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  const auto deadline = std::chrono::steady_clock::now() + kTimeout;
  while (reader.Queued() < queued && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

/// Reads the whole stream, reads once more after its end, then finishes.
Task<> ReadAll(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen, std::promise<void>* done) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  while (auto feature = co_await reader.Read()) {
    seen->names.push_back(feature->name());
    seen->threads.push_back(std::this_thread::get_id());
  }
  seen->read_after_end_gave_nothing = !co_await reader.Read();
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// Reads one message, pauses the loop thread until the other two are queued, then reads them, counting the
/// suspensions that takes, then reads to the end and finishes.
Task<> ReadOnePauseThenReadOn(coro_reader_test::CountingExecutor* executor, routeguide::RouteGuide::Stub* stub,
                              Seen* seen, std::promise<void>* done) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  PauseUntilQueued(reader, 2);
  seen->queued_after_pause = reader.Queued();
  const int posts = executor->Posts();
  for (int i = 0; i < 2; ++i) {
    if (auto feature = co_await reader.Read()) seen->names.push_back(feature->name());
  }
  seen->posts_after_pause = executor->Posts() - posts;
  while (auto feature = co_await reader.Read()) seen->names.push_back(feature->name());
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// Reads `reads` messages of a three-message stream, pauses the loop thread until the others wait, then awaits
/// Finish() without reading to the end of the stream.
Task<> ReadSomePauseThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, int reads, Seen* seen,
                               std::promise<void>* done) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  for (int i = 0; i < reads; ++i) {
    if (auto feature = co_await reader.Read()) seen->names.push_back(feature->name());
  }
  PauseUntilQueued(reader, static_cast<std::size_t>(3 - reads));
  seen->queued_after_pause = reader.Queued();
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// Hands its reader out, then reads the whole stream, signalling `first` once it has the first message.
Task<> ReadAllSignallingTheFirst(Executor* executor, routeguide::RouteGuide::Stub* stub, UnboundedReader** out,
                                 std::promise<void>* first, Seen* seen, std::promise<void>* done) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  *out = &reader;
  while (auto feature = co_await reader.Read()) {
    seen->names.push_back(feature->name());
    if (seen->names.size() == 1) first->set_value();
  }
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// Reads one message, pauses so the next ones queue, then never reads again, nor resumes.
Task<> ReadOnePauseThenStall(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* read_one) {
  UnboundedReader reader{*executor, *stub, routeguide::Rectangle{}};
  static_cast<void>(co_await reader.Read());
  PauseUntilQueued(reader, 2);
  EXPECT_EQ(reader.Queued(), 2U);
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
  /// Runs `make_task` to its end, and returns what it saw.
  template <class MakeTask>
  Seen Run(MakeTask make_task) {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) { return make_task(&seen, done); });
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
    return seen;
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
  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(kMessages, LoopThread()));
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

/// @test The stream runs ahead of a slow coroutine: while it pauses after the first message, the rest of the
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

/// @test An early Finish(), with messages queued on a stream the server has finished OK, drops them and gives
/// CANCELLED rather than the server's OK: the coroutine did not read every message.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FinishWithMessagesQueued_DropsThemAndGivesCancelled) {
  test_service_.Send(3);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadSomePauseThenFinish(&executor_, stub_.get(), 1, s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.queued_after_pause, 2U);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test An early Finish(), with messages queued on a stream the server holds open, cancels the RPC, which only
/// that cancel can end, and gives CANCELLED.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FinishWithMessagesQueuedOnAHeldStream_EndsTheRpcCancelled) {
  test_service_.HoldAfter(3);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadSomePauseThenFinish(&executor_, stub_.get(), 1, s, done);
  });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.queued_after_pause, 2U);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test An early Finish(), once every message is read, on a stream the server has finished, gives the server's
/// OK: nothing was dropped, and the status had arrived before the cancel.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FinishAfterEveryMessageOfAFinishedStream_GivesOk) {
  test_service_.Send(3);
  const Seen seen = Run([&](Seen* s, std::promise<void>* done) {
    return ReadSomePauseThenFinish(&executor_, stub_.get(), 3, s, done);
  });

  EXPECT_EQ(seen.names, Names(3));
  EXPECT_EQ(seen.queued_after_pause, 0U);
  EXPECT_TRUE(seen.status.ok());
}

/// A kPush, kContinuous ListFeatures reactor whose second read is handed over late, so a coroutine can await
/// Finish() while that message is on its way, after the server has finished.
class SlowSecondReadReactor final
    : public RpcReactor::Client::ActiveReadReactor<routeguide::Feature, Delivery::kPush, ReadPacing::kContinuous> {
  using Base = RpcReactor::Client::ActiveReadReactor<routeguide::Feature, Delivery::kPush, ReadPacing::kContinuous>;

 public:
  using Callbacks =
      RpcReactor::Client::ActiveReadCallbacks<routeguide::Feature, Delivery::kPush, ReadPacing::kContinuous>;

  SlowSecondReadReactor(routeguide::RouteGuide::Stub& stub, std::unique_ptr<grpc::ClientContext> context,
                        const routeguide::Rectangle& request, Callbacks&& cbs)
      : Base(std::move(context), std::move(cbs)) {
    stub.async()->ListFeatures(this->context_.get(), &request, this);
    this->StartRead(&this->response_);
    this->StartCall();
  }

 protected:
  void OnReadDone(bool ok) override {
    if (ok && ++reads_ == 2) std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Base::OnReadDone(ok);
  }

 private:
  int reads_ = 0;  // only touched by OnReadDone(), whose runs the read between them orders
};

/// Reads one message, then awaits Finish() while the second one is on its way.
Task<> ReadOneThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                         std::promise<void>* done) {
  Reader<SlowSecondReadReactor> reader{*executor, *stub, routeguide::Rectangle{}};
  if (auto first = co_await reader.Read()) seen->names.push_back(first->name());
  seen->queued_after_pause = reader.Queued();
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// @test A message that arrives after an early Finish() found the queue empty is dropped too, and the status is
/// CANCELLED, even though the server finished OK before the cancel.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_MessageArrivingAfterAnEarlyFinish_IsDroppedAndGivesCancelled) {
  test_service_.Send(2);
  const Seen seen =
      Run([&](Seen* s, std::promise<void>* done) { return ReadOneThenFinish(&executor_, stub_.get(), s, done); });

  EXPECT_EQ(seen.names, Names(1));
  EXPECT_EQ(seen.queued_after_pause, 0U);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test A frame destroyed with messages queued, on a stream the server holds open, cancels the RPC, destroys the
/// reactor once OnDone() has run, then frees the queue, posting nothing. AddressSanitizer reports a reactor freed
/// under gRPC, or a queue freed before a callback that still reaches it.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FrameDestroyedWithMessagesQueued_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(3);
  DestroyAfterOneMessage(
      [&](std::promise<void>* read_one) { return ReadOnePauseThenStall(&executor_, stub_.get(), read_one); });
}

/// @test A frame destroyed while its Read() waits on an empty queue, on a stream the server holds open, does the
/// same, without posting the destroyed coroutine.
TEST_F(ReactorCoroUnboundedReaderTest, ListFeatures_FrameDestroyedWhileReadWaits_CancelsAndWaitsForOnDone) {
  test_service_.HoldAfter(1);
  DestroyAfterOneMessage(
      [&](std::promise<void>* read_one) { return ReadOneThenWait(&executor_, stub_.get(), read_one); });
}

}  // namespace
