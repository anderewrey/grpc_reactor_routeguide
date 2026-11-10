///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests coro::Reader, the reader without a buffer of reactor_coro.h, against an in-process server, with
/// coroutines resumed on the AppLoop task queue that stands in for the application's event loop: every message
/// owned and in order under both deliveries, the end of the stream, a server error, kPull's read-ahead against
/// kPush's reading only when asked, a cancel while a read waits, a frame destroyed mid-stream, Finish() given
/// the status whether OnDone() runs before or after it is awaited, and Finish() awaited before the end. The
/// re-armed hand-over inside the reader is tested on its own, with no RPC and no timing, in coro_task_test.cpp.

#include <gtest/gtest.h>

#include <chrono>
#include <coroutine>
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
using RpcReactor::coro::Executor;
using RpcReactor::coro::Reader;
using RpcReactor::coro::RootTasks;
using RpcReactor::coro::Task;
template <Delivery kDelivery>
using ListFeaturesReader = Reader<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kTurnByTurn>>;

using coro_reader_test::Forever;
using coro_reader_test::kTimeout;

// What Read() gives, the same under both deliveries.
static_assert(std::is_same_v<ListFeaturesReader<Delivery::kPull>::MessageT, std::optional<routeguide::Feature>>);
static_assert(std::is_same_v<ListFeaturesReader<Delivery::kPush>::MessageT, std::optional<routeguide::Feature>>);

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

class ReactorCoroReaderTest : public coro_reader_test::CoroReaderTestBase {
 protected:
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
};

/// The names of the three features the service sends after Send(3).
std::vector<std::string> ThreeNames() { return {"feature 1", "feature 2", "feature 3"}; }

/// @test Under kPull, Read() gives every message, swapped out of the reactor, in order and on the loop, then
/// nothing at the end of the stream, and nothing again after, and Finish() gives OK.
TEST_F(ReactorCoroReaderTest, ListFeatures_PullReadsEveryMessage_ThenNothing_ThenFinishOk) {
  test_service_.Send(3);
  const Seen seen = ReadWholeStream<Delivery::kPull>();

  EXPECT_EQ(seen.names, ThreeNames());
  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(3, LoopThread()));
  EXPECT_TRUE(seen.read_after_end_gave_nothing);
  EXPECT_TRUE(seen.status.ok());
}

/// @test Under kPush, Read() gives every message the read callback handed over, and the same end.
TEST_F(ReactorCoroReaderTest, ListFeatures_PushReadsEveryMessage_ThenNothing_ThenFinishOk) {
  test_service_.Send(3);
  const Seen seen = ReadWholeStream<Delivery::kPush>();

  EXPECT_EQ(seen.names, ThreeNames());
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

/// A ListFeatures reactor whose OnDone() waits before it runs, so the coroutine awaits Finish() first.
template <Delivery kDelivery>
class SlowDoneReactor final
    : public RpcReactor::Client::ActiveReadReactor<routeguide::Feature, kDelivery, ReadPacing::kTurnByTurn> {
  using Base = RpcReactor::Client::ActiveReadReactor<routeguide::Feature, kDelivery, ReadPacing::kTurnByTurn>;

 public:
  using Callbacks = RpcReactor::Client::ActiveReadCallbacks<routeguide::Feature, kDelivery, ReadPacing::kTurnByTurn>;

  SlowDoneReactor(routeguide::RouteGuide::Stub& stub, std::unique_ptr<grpc::ClientContext> context,
                  const routeguide::Rectangle& request, Callbacks&& cbs)
      : Base(std::move(context), std::move(cbs)) {
    stub.async()->ListFeatures(this->context_.get(), &request, this);
    this->StartRead(&this->response_);
    this->StartCall();
  }

 protected:
  void OnDone(const grpc::Status& status) override {
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    Base::OnDone(status);
  }
};

/// Reads the whole stream, then awaits Finish(), after pausing the loop thread when `pause` is set, so that
/// OnDone() has run by then.
template <class ReaderT>
Task<> ReadAllThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, bool pause, Seen* seen,
                         std::promise<void>* done) {
  ReaderT reader{*executor, *stub, routeguide::Rectangle{}};
  while (auto feature = co_await reader.Read()) seen->names.push_back(feature->name());
  if (pause) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// The end of the read and OnDone() are two hand-overs: whichever order they come in, Finish() gives the status.
class ReactorCoroReaderOrderTest : public ReactorCoroReaderTest {
 protected:
  template <class ReaderT>
  Seen RunReadAllThenFinish(bool pause) {
    test_service_.Send(3);
    test_service_.FailAfter(2, grpc::StatusCode::NOT_FOUND);
    Seen seen;
    RunToEnd([&](std::promise<void>* done) {
      return ReadAllThenFinish<ReaderT>(&executor_, stub_.get(), pause, &seen, done);
    });
    return seen;
  }
  /// Reads every message, then pauses, so OnDone() has run when Finish() is awaited.
  template <Delivery kDelivery>
  Seen OnDoneFirst() {
    return RunReadAllThenFinish<ListFeaturesReader<kDelivery>>(true);
  }
  /// Reads every message, then awaits Finish() while OnDone() is delayed.
  template <Delivery kDelivery>
  Seen FinishFirst() {
    return RunReadAllThenFinish<Reader<SlowDoneReactor<kDelivery>>>(false);
  }
};

/// @test Under kPull, OnDone() runs before Finish() is awaited: Finish() goes on at once with the status.
TEST_F(ReactorCoroReaderOrderTest, ListFeatures_PullOnDoneBeforeFinish_FinishGivesTheStatus) {
  const Seen seen = OnDoneFirst<Delivery::kPull>();

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1", "feature 2"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test The same under kPush.
TEST_F(ReactorCoroReaderOrderTest, ListFeatures_PushOnDoneBeforeFinish_FinishGivesTheStatus) {
  const Seen seen = OnDoneFirst<Delivery::kPush>();

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1", "feature 2"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test Under kPull, Finish() is awaited before OnDone() runs: it waits for OnDone(), then gives its status.
TEST_F(ReactorCoroReaderOrderTest, ListFeatures_PullFinishBeforeOnDone_FinishWaitsForTheStatus) {
  const Seen seen = FinishFirst<Delivery::kPull>();

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1", "feature 2"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test The same under kPush.
TEST_F(ReactorCoroReaderOrderTest, ListFeatures_PushFinishBeforeOnDone_FinishWaitsForTheStatus) {
  const Seen seen = FinishFirst<Delivery::kPush>();

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1", "feature 2"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// Reads `reads` messages, then awaits Finish() without reading to the end of the stream, after pausing the loop
/// thread when `pause` is set, so that a server which finishes on its own has done so by then.
template <Delivery kDelivery>
Task<> ReadSomeThenFinish(Executor* executor, routeguide::RouteGuide::Stub* stub, int reads, bool pause, Seen* seen,
                          std::promise<void>* done) {
  ListFeaturesReader<kDelivery> reader{*executor, *stub, routeguide::Rectangle{}};
  for (int i = 0; i < reads; ++i) {
    if (auto feature = co_await reader.Read()) seen->names.push_back(feature->name());
  }
  if (pause) std::this_thread::sleep_for(std::chrono::milliseconds(200));
  seen->status = co_await reader.Finish();
  done->set_value();
}

/// A Finish() awaited before Read() has given nothing ends the RPC.
class ReactorCoroReaderEarlyFinishTest : public ReactorCoroReaderTest {
 protected:
  /// On a stream the server holds open after three messages, which only the cancel Finish() sends can end.
  template <Delivery kDelivery>
  Seen FinishAfter(int reads) {
    test_service_.HoldAfter(3);
    return Run<kDelivery>(reads, false);
  }
  /// On a stream the server finishes OK after three messages, and has finished by the time Finish() is awaited.
  template <Delivery kDelivery>
  Seen FinishOnAFinishedStreamAfter(int reads) {
    test_service_.Send(3);
    return Run<kDelivery>(reads, true);
  }

 private:
  template <Delivery kDelivery>
  Seen Run(int reads, bool pause) {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) {
      return ReadSomeThenFinish<kDelivery>(&executor_, stub_.get(), reads, pause, &seen, done);
    });
    return seen;
  }
};

/// @test Under kPull, Finish() awaited mid-stream, with the next message read ahead, cancels the RPC, releases
/// that message, and gives CANCELLED.
TEST_F(ReactorCoroReaderEarlyFinishTest, ListFeatures_PullFinishMidStream_EndsTheRpcCancelled) {
  const Seen seen = FinishAfter<Delivery::kPull>(1);

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test Under kPush, Finish() awaited mid-stream, with the given message's read not armed, releases its hold,
/// and gives CANCELLED.
TEST_F(ReactorCoroReaderEarlyFinishTest, ListFeatures_PushFinishMidStream_EndsTheRpcCancelled) {
  const Seen seen = FinishAfter<Delivery::kPush>(1);

  EXPECT_EQ(seen.names, (std::vector<std::string>{"feature 1"}));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test Under kPull, Finish() awaited after the last message, while the read armed for the next one waits, gives
/// CANCELLED.
TEST_F(ReactorCoroReaderEarlyFinishTest, ListFeatures_PullFinishAfterTheLastMessage_EndsTheRpcCancelled) {
  const Seen seen = FinishAfter<Delivery::kPull>(3);

  EXPECT_EQ(seen.names, ThreeNames());
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test Under kPush, Finish() awaited after the last message, before the Read() that would give nothing, gives
/// CANCELLED.
TEST_F(ReactorCoroReaderEarlyFinishTest, ListFeatures_PushFinishAfterTheLastMessage_EndsTheRpcCancelled) {
  const Seen seen = FinishAfter<Delivery::kPush>(3);

  EXPECT_EQ(seen.names, ThreeNames());
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}
/// @test Under kPull, Finish() awaited once every message is read, on a stream the server has finished, gives the
/// server's OK rather than CANCELLED: the status had arrived before the cancel.
TEST_F(ReactorCoroReaderEarlyFinishTest, ListFeatures_PullFinishAfterTheLastMessageOfAFinishedStream_GivesOk) {
  const Seen seen = FinishOnAFinishedStreamAfter<Delivery::kPull>(3);

  EXPECT_EQ(seen.names, ThreeNames());
  EXPECT_TRUE(seen.status.ok());
}

/// @test The same under kPush, with the last message's read never armed.
TEST_F(ReactorCoroReaderEarlyFinishTest, ListFeatures_PushFinishAfterTheLastMessageOfAFinishedStream_GivesOk) {
  const Seen seen = FinishOnAFinishedStreamAfter<Delivery::kPush>(3);

  EXPECT_EQ(seen.names, ThreeNames());
  EXPECT_TRUE(seen.status.ok());
}
}  // namespace
