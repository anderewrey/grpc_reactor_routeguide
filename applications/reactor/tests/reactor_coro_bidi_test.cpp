///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

/// Tests coro::Bidi of reactor_coro.h against an in-process server, with coroutines resumed on the AppLoop task
/// queue that stands in for the application's event loop. A reading coroutine and a writing coroutine share one
/// Bidi, joined by coro::WhenAll() before Finish(): every note sent and every reply received, the read side
/// ending while the writes go on, the writes ending while the replies go on, a message queued before Read() is
/// awaited, a cancel, Finish() awaited early, and a frame destroyed with both sides in flight.

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
using RpcReactor::Client::ReadPacing;
using RpcReactor::Client::Submission;
using RpcReactor::coro::Bidi;
using RpcReactor::coro::Executor;
using RpcReactor::coro::RootTasks;
using RpcReactor::coro::Task;
using RpcReactor::coro::WhenAll;
using ChatReactor =
    routeguide::RouteChat::ClientReactor<Delivery::kPush, Submission::kApplicationPushes, ReadPacing::kContinuous>;
using Chat = Bidi<ChatReactor>;

constexpr auto kTimeout = std::chrono::seconds(10);
// How long the loop thread pauses for something to happen on gRPC's threads meanwhile: a reply to arrive, or the
// server's status. No event tells the client either, so the pause has head-room for loaded or sanitizer runs.
constexpr auto kPause = std::chrono::milliseconds(1000);

/// The note numbered `i`, recognised by the server by its message.
routeguide::RouteNote NoteNumber(int i) { return rg_utils::MakeRouteNote("note " + std::to_string(i), i, 0); }

/// The messages of the notes numbered 1 to `count`, in order.
std::vector<std::string> Messages(int count) {
  std::vector<std::string> messages;
  for (int i = 1; i <= count; ++i) messages.push_back("note " + std::to_string(i));
  return messages;
}

/// Records every note it reads, and echoes it back, unless told not to. Finishes once the client closes its
/// stream, after sending every echo, and a burst of replies when asked. Or finishes once a given number of notes
/// is read, with a given status.
class ChatService final : public routeguide::RouteGuide::CallbackService {
 public:
  /// Echoes nothing: the client's reads wait.
  void Silent() { echo_ = false; }

  /// Once the client closes its stream, sends this many replies, numbered from 1001, before finishing.
  void BurstAfterClose(int replies) { burst_ = replies; }

  /// Finishes with this status once this many notes are read, after echoing them.
  void FinishAfter(int received, grpc::StatusCode code) {
    finish_after_ = received;
    code_ = code;
  }

  /// The messages of the notes read, in order.
  std::vector<std::string> Received() {
    const std::lock_guard lock(mutex_);
    return received_;
  }

  grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote>* RouteChat(
      grpc::CallbackServerContext* /*context*/) override {
    class Chatter : public grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote> {
     public:
      explicit Chatter(ChatService* service) : service_(service) { StartRead(&in_); }

      void OnReadDone(bool ok) override {
        if (!ok) {
          // The client closed its stream, or the RPC ended.
          for (int i = 1; i <= service_->burst_; ++i) Send(NoteNumber(1000 + i));
          EndWhenSent(grpc::Status::OK);
          return;
        }
        service_->Record(in_.message());
        if (service_->echo_) Send(in_);
        if (++read_ == service_->finish_after_) {
          EndWhenSent(grpc::Status(service_->code_, "finished on purpose"));
          return;
        }
        StartRead(&in_);
      }

      void OnWriteDone(bool /*ok*/) override {
        const routeguide::RouteNote* next = nullptr;
        {
          const std::lock_guard lock(mutex_);
          out_.pop_front();
          writing_ = !out_.empty();
          if (writing_) next = &out_.front();
        }
        if (next != nullptr) {
          StartWrite(next);
          return;
        }
        MaybeFinish();
      }

      void OnDone() override { delete this; }

     private:
      /// Queues a reply, and writes it at once when no write is in flight. The deque is only touched under the
      /// lock, since OnReadDone() and OnWriteDone() run on different threads; the note written keeps its address
      /// while others are pushed behind it, so StartWrite() gets its pointer outside the lock.
      void Send(const routeguide::RouteNote& note) {
        const routeguide::RouteNote* next = nullptr;
        {
          const std::lock_guard lock(mutex_);
          out_.push_back(note);
          if (!std::exchange(writing_, true)) next = &out_.back();
        }
        if (next != nullptr) StartWrite(next);
      }

      /// Finishes with `status` once every queued reply is written.
      void EndWhenSent(grpc::Status status) {
        {
          const std::lock_guard lock(mutex_);
          status_ = std::move(status);
          ending_ = true;
        }
        MaybeFinish();
      }

      void MaybeFinish() {
        {
          const std::lock_guard lock(mutex_);
          if (!ending_ || writing_ || finished_) return;
          finished_ = true;
        }
        Finish(status_);
      }

      ChatService* service_;
      routeguide::RouteNote in_;
      int read_ = 0;
      std::mutex mutex_;
      std::deque<routeguide::RouteNote> out_;
      bool writing_ = false;
      bool ending_ = false;
      bool finished_ = false;
      grpc::Status status_;
    };
    return new Chatter(this);
  }

 private:
  void Record(const std::string& message) {
    const std::lock_guard lock(mutex_);
    received_.push_back(message);
  }

  std::atomic<bool> echo_{true};
  std::atomic<int> burst_{0};
  std::atomic<int> finish_after_{-1};
  std::atomic<grpc::StatusCode> code_{grpc::StatusCode::OK};
  std::mutex mutex_;
  std::vector<std::string> received_;
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

/// What the test coroutines saw, written on the loop, read by the test once `done` is ready.
struct Seen {
  std::vector<std::string> replies;      // every message Read() gave, in order
  std::vector<bool> writes;              // what each Write() gave, in order
  std::vector<std::thread::id> threads;  // each resumption ran on
  std::optional<bool> closed;            // what Close() gave, if called
  int reads_after_close = -1;            // how many messages Read() gave after Close()
  grpc::Status status;
};

/// Reads until Read() gives nothing.
Task<int> ReadAll(Chat* chat, Seen* seen) {
  int count = 0;
  while (auto reply = co_await chat->Read()) {
    seen->threads.push_back(std::this_thread::get_id());
    seen->replies.push_back(reply->message());
    if (seen->closed) ++seen->reads_after_close;
    ++count;
  }
  seen->threads.push_back(std::this_thread::get_id());
  co_return count;
}

/// Writes the notes numbered 1 to `count`, pausing `between` after each, then closes, stopping early when a
/// Write() gives false.
Task<int> WriteAll(Chat* chat, int count, std::chrono::milliseconds between, Seen* seen) {
  int written = 0;
  for (int i = 1; i <= count; ++i) {
    const bool ok = co_await chat->Write(NoteNumber(i));
    seen->threads.push_back(std::this_thread::get_id());
    seen->writes.push_back(ok);
    if (!ok) break;
    ++written;
    if (between.count() > 0) std::this_thread::sleep_for(between);
  }
  seen->closed = co_await chat->Close();
  seen->reads_after_close = 0;
  co_return written;
}

/// One reading and one writing coroutine on one Bidi, joined, then Finish().
Task<> ReadAndWrite(Executor* executor, routeguide::RouteGuide::Stub* stub, int count,
                    std::chrono::milliseconds between, Seen* seen, std::promise<void>* done) {
  Chat chat{*executor, *stub};
  const auto [read, written] = co_await WhenAll(ReadAll(&chat, seen), WriteAll(&chat, count, between, seen));
  seen->status = co_await chat.Finish();
  seen->threads.push_back(std::this_thread::get_id());
  EXPECT_EQ(read, static_cast<int>(seen->replies.size()));
  EXPECT_LE(written, count);
  done->set_value();
}

/// Writes one note, pauses the loop thread so its echo is queued before Read() is awaited, then reads it, closes,
/// reads the end, and finishes. `posts` receives how many resumptions the executor was given by the read.
Task<> ReadQueuedBeforeTheAwait(Executor* executor, routeguide::RouteGuide::Stub* stub,
                                const CountingExecutor* counter, int* posts, Seen* seen, std::promise<void>* done) {
  Chat chat{*executor, *stub};
  seen->writes.push_back(co_await chat.Write(NoteNumber(1)));
  std::this_thread::sleep_for(kPause);
  const int before = counter->Posts();
  auto reply = co_await chat.Read();
  *posts = counter->Posts() - before;
  if (reply) seen->replies.push_back(reply->message());
  seen->closed = co_await chat.Close();
  EXPECT_FALSE(co_await chat.Read());
  EXPECT_FALSE(co_await chat.Read());  // after the end, at once
  seen->status = co_await chat.Finish();
  done->set_value();
}

/// Writes one note, reads its echo, then awaits Finish() without closing, after pausing the loop thread when
/// `pause` is set, so that a server which finishes on its own has done so by then.
Task<> FinishBeforeClose(Executor* executor, routeguide::RouteGuide::Stub* stub, bool read_to_end, bool pause,
                         Seen* seen, std::promise<void>* done) {
  Chat chat{*executor, *stub};
  seen->writes.push_back(co_await chat.Write(NoteNumber(1)));
  if (auto reply = co_await chat.Read()) seen->replies.push_back(reply->message());
  if (read_to_end) {
    while (auto reply = co_await chat.Read()) seen->replies.push_back(reply->message());
  }
  if (pause) std::this_thread::sleep_for(kPause);
  seen->status = co_await chat.Finish();
  seen->threads.push_back(std::this_thread::get_id());
  done->set_value();
}

/// Writes, closes, then awaits Finish() before reading anything, once every reply has been queued.
Task<> FinishBeforeReading(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                           std::promise<void>* done) {
  Chat chat{*executor, *stub};
  seen->writes.push_back(co_await chat.Write(NoteNumber(1)));
  seen->closed = co_await chat.Close();
  std::this_thread::sleep_for(kPause);
  seen->status = co_await chat.Finish();
  done->set_value();
}

/// A reading coroutine and a writing one, the writer cancelling after its first write.
Task<int> WriteThenCancel(Chat* chat, Seen* seen) {
  seen->writes.push_back(co_await chat->Write(NoteNumber(1)));
  chat->TryCancel();
  seen->writes.push_back(co_await chat->Write(NoteNumber(2)));
  seen->closed = co_await chat->Close();
  co_return 0;
}

Task<> ReadAndCancel(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen, std::promise<void>* done) {
  Chat chat{*executor, *stub};
  static_cast<void>(co_await WhenAll(ReadAll(&chat, seen), WriteThenCancel(&chat, seen)));
  seen->status = co_await chat.Finish();
  done->set_value();
}

/// Reads until Read() gives nothing, then reads once more, which gives nothing at once.
Task<int> ReadAllThenOnceMore(Chat* chat, Seen* seen) {
  const int read = co_await ReadAll(chat, seen);
  if (co_await chat->Read()) seen->replies.emplace_back("after the end");
  co_return read;
}

/// Writes one note, then awaits Finish() early, while the sibling reader still waits for a reply.
Task<int> WriteThenFinish(Chat* chat, Seen* seen) {
  seen->writes.push_back(co_await chat->Write(NoteNumber(1)));
  seen->status = co_await chat->Finish();
  co_return 0;
}

/// A reading coroutine, and a writing one that ends the RPC with Finish() while the reader waits.
Task<> FinishWhileTheReaderWaits(Executor* executor, routeguide::RouteGuide::Stub* stub, Seen* seen,
                                 std::promise<void>* done) {
  Chat chat{*executor, *stub};
  static_cast<void>(co_await WhenAll(ReadAllThenOnceMore(&chat, seen), WriteThenFinish(&chat, seen)));
  done->set_value();
}

/// A RouteChat reactor whose OnWriteDone() waits before it runs, so a write is still in flight while the test
/// destroys the frame.
class SlowWriteReactor final
    : public RpcReactor::Client::ActiveBidiReactor<routeguide::RouteNote, routeguide::RouteNote, Delivery::kPush,
                                                   Submission::kApplicationPushes, ReadPacing::kContinuous> {
  using Base = RpcReactor::Client::ActiveBidiReactor<routeguide::RouteNote, routeguide::RouteNote, Delivery::kPush,
                                                     Submission::kApplicationPushes, ReadPacing::kContinuous>;

 public:
  using Callbacks = RpcReactor::Client::ActiveBidiCallbacks<routeguide::RouteNote, routeguide::RouteNote,
                                                            Delivery::kPush, Submission::kApplicationPushes,
                                                            ReadPacing::kContinuous>;

  SlowWriteReactor(routeguide::RouteGuide::Stub& stub, std::unique_ptr<grpc::ClientContext> context, Callbacks&& cbs)
      : Base(std::move(context), std::move(cbs)) {
    stub.async()->RouteChat(this->context_.get(), this);
    this->StartRead(&this->response_);
    this->StartCall();
  }

 protected:
  void OnWriteDone(bool ok) override {
    std::this_thread::sleep_for(kPause);
    Base::OnWriteDone(ok);
  }
};

Task<> WaitInRead(Bidi<SlowWriteReactor>* chat) {
  static_cast<void>(co_await chat->Read());
  ADD_FAILURE() << "a destroyed frame never resumes";
}

Task<> WaitInWrite(Bidi<SlowWriteReactor>* chat, std::promise<void>* writing) {
  writing->set_value();  // the test queues the destruction behind this resumption, so behind the suspension
  static_cast<void>(co_await chat->Write(NoteNumber(1)));
  ADD_FAILURE() << "a destroyed frame never resumes";
}

/// Reads and writes, both suspended until the frame is destroyed: the server echoes nothing, and the write stays
/// in flight.
Task<> BothSidesInFlight(Executor* executor, routeguide::RouteGuide::Stub* stub, std::promise<void>* writing) {
  Bidi<SlowWriteReactor> chat{*executor, *stub};
  static_cast<void>(co_await WhenAll(WaitInRead(&chat), WaitInWrite(&chat, writing)));
  ADD_FAILURE() << "a destroyed frame never resumes";
}

class ReactorCoroBidiTest : public RouteGuideTestFixtureBase<ChatService> {
 protected:
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

  /// Runs ReadAndWrite() to its end.
  Seen Chatter(int count, std::chrono::milliseconds between = std::chrono::milliseconds(0)) {
    Seen seen;
    RunToEnd([&](std::promise<void>* done) {
      return ReadAndWrite(&executor_, stub_.get(), count, between, &seen, done);
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

/// @test Every note written is read by the server, in order, every echo is read back, in order, and Finish()
/// gives OK. Every resumption, of both coroutines and of the joined task, runs on the loop.
TEST_F(ReactorCoroBidiTest, RouteChat_EveryNoteSent_EveryReplyReceived) {
  constexpr int kNotes = 20;
  const Seen seen = Chatter(kNotes);

  EXPECT_EQ(test_service_.Received(), Messages(kNotes));
  EXPECT_EQ(seen.replies, Messages(kNotes));
  EXPECT_EQ(seen.writes, std::vector<bool>(kNotes, true));
  EXPECT_EQ(seen.closed, true);
  EXPECT_TRUE(seen.status.ok()) << seen.status.error_message();
  // kNotes writes, kNotes replies, the end of the stream, and Finish().
  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(2 * kNotes + 2, LoopThread()));
}

/// @test No note at all: the writer closes at once, and the reader gets the end of the stream.
TEST_F(ReactorCoroBidiTest, RouteChat_NoNote_CloseEndsBothSides) {
  const Seen seen = Chatter(0);

  EXPECT_TRUE(seen.replies.empty());
  EXPECT_EQ(seen.closed, true);
  EXPECT_TRUE(seen.status.ok()) << seen.status.error_message();
}

/// @test The server ends the RPC after 3 notes, while the writer is still writing, slowly: the reader gets the
/// three echoes then the end of the stream without the writer closing, a later Write() gives false, Close() false,
/// and Finish() gives the server's status.
TEST_F(ReactorCoroBidiTest, RouteChat_ReadSideEndsWhileWritesGoOn_WriteGivesFalse) {
  test_service_.FinishAfter(3, grpc::StatusCode::OK);
  const Seen seen = Chatter(1000, std::chrono::milliseconds(5));

  EXPECT_EQ(seen.replies, Messages(3));
  ASSERT_GE(seen.writes.size(), 4U);
  EXPECT_EQ(seen.writes.back(), false);
  EXPECT_EQ(seen.closed, false);
  EXPECT_TRUE(seen.status.ok()) << seen.status.error_message();
}

/// @test The server ends the RPC with an error after 2 notes: the reader ends, a Write() gives false, and
/// Finish() gives the error.
TEST_F(ReactorCoroBidiTest, RouteChat_ServerError_ReadEnds_FinishGivesTheError) {
  test_service_.FinishAfter(2, grpc::StatusCode::NOT_FOUND);
  const Seen seen = Chatter(1000, std::chrono::milliseconds(5));

  EXPECT_EQ(seen.replies, Messages(2));
  ASSERT_FALSE(seen.writes.empty());
  EXPECT_EQ(seen.writes.back(), false);
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test The writer closes after 2 notes, and the server answers the close with 50 more replies: the reader gets
/// every one of them after the close, then the end of the stream.
TEST_F(ReactorCoroBidiTest, RouteChat_WritesEndWhileReadsGoOn_EveryLaterReplyReceived) {
  constexpr int kBurst = 50;
  test_service_.BurstAfterClose(kBurst);
  const Seen seen = Chatter(2);

  std::vector<std::string> expected = Messages(2);
  for (int i = 1; i <= kBurst; ++i) expected.push_back("note " + std::to_string(1000 + i));
  EXPECT_EQ(seen.replies, expected);
  EXPECT_EQ(seen.closed, true);
  EXPECT_GE(seen.reads_after_close, kBurst);
  EXPECT_TRUE(seen.status.ok()) << seen.status.error_message();
}

/// @test A reply queued before Read() is awaited is taken without suspending, so without a resumption, and the
/// next Read() still waits for the end of the stream.
TEST_F(ReactorCoroBidiTest, RouteChat_ReplyQueuedBeforeTheAwait_GoesOnWithoutSuspending) {
  Seen seen;
  int posts = -1;
  RunToEnd([&](std::promise<void>* done) {
    return ReadQueuedBeforeTheAwait(&executor_, stub_.get(), &executor_, &posts, &seen, done);
  });

  EXPECT_EQ(posts, 0);
  EXPECT_EQ(seen.replies, Messages(1));
  EXPECT_TRUE(seen.status.ok()) << seen.status.error_message();
}

/// @test A cancel from the writer ends both sides: its next Write() gives false, Close() false, the reader gets
/// the end of the stream, and Finish() gives CANCELLED.
TEST_F(ReactorCoroBidiTest, RouteChat_TryCancel_EndsBothSides_FinishGivesCancelled) {
  test_service_.Silent();
  Seen seen;
  RunToEnd([&](std::promise<void>* done) { return ReadAndCancel(&executor_, stub_.get(), &seen, done); });

  EXPECT_EQ(seen.writes, (std::vector<bool>{true, false}));
  EXPECT_EQ(seen.closed, false);
  EXPECT_TRUE(seen.replies.empty());
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test Finish() awaited before Close(), the read side still open, ends the RPC: it cancels it, which the open
/// write flow would otherwise keep open, and gives CANCELLED, on the loop.
TEST_F(ReactorCoroBidiTest, RouteChat_FinishBeforeClose_EndsTheRpcCancelled) {
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return FinishBeforeClose(&executor_, stub_.get(), false, false, &seen, done);
  });

  EXPECT_EQ(seen.replies, Messages(1));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(seen.threads, std::vector<std::thread::id>(1, LoopThread()));
}

/// @test Finish() awaited after the client closed, but before reading the replies already queued, drops them
/// and ends the RPC: the server waits for nothing more, so its status has arrived, and it is the one given.
TEST_F(ReactorCoroBidiTest, RouteChat_FinishBeforeReadingTheQueuedReplies_GivesTheServersStatus) {
  test_service_.BurstAfterClose(5);
  Seen seen;
  RunToEnd([&](std::promise<void>* done) { return FinishBeforeReading(&executor_, stub_.get(), &seen, done); });

  EXPECT_TRUE(seen.replies.empty());
  EXPECT_TRUE(seen.status.ok()) << seen.status.error_message();
}

/// @test Finish() awaited before Close(), once every reply was read and the server's error had arrived, gives
/// that error: the cancel it sends changes nothing.
TEST_F(ReactorCoroBidiTest, RouteChat_FinishBeforeCloseAfterTheServerError_GivesTheError) {
  test_service_.FinishAfter(1, grpc::StatusCode::NOT_FOUND);
  Seen seen;
  RunToEnd([&](std::promise<void>* done) {
    return FinishBeforeClose(&executor_, stub_.get(), true, true, &seen, done);
  });

  EXPECT_EQ(seen.replies, Messages(1));
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::NOT_FOUND);
}

/// @test Finish() awaited by the writing coroutine, while the reading one still waits for a reply, ends the RPC
/// cancelled: the reader gets the end of the stream, and a Read() after it gives nothing at once, even once
/// Finish() has destroyed the reactor.
TEST_F(ReactorCoroBidiTest, RouteChat_FinishWhileTheReaderWaits_TheReaderGetsTheEnd) {
  test_service_.Silent();
  Seen seen;
  RunToEnd([&](std::promise<void>* done) { return FinishWhileTheReaderWaits(&executor_, stub_.get(), &seen, done); });

  EXPECT_EQ(seen.writes, std::vector<bool>{true});
  EXPECT_TRUE(seen.replies.empty());
  EXPECT_EQ(seen.status.error_code(), grpc::StatusCode::CANCELLED);
}

/// @test A frame destroyed with the reader waiting for a reply and a write in flight, both under the join,
/// cancels the RPC and destroys the reactor once its OnDone() has run, in time, and nothing is posted after: the
/// destroyed coroutines never resume. ASan checks that nothing is freed under gRPC.
TEST_F(ReactorCoroBidiTest, RouteChat_FrameDestroyedWithBothSidesInFlight_CancelsAndWaitsForOnDone) {
  test_service_.Silent();
  auto roots = std::make_unique<RootTasks>();
  std::promise<void> writing;
  OnLoop([&] { roots->Spawn(BothSidesInFlight(&executor_, stub_.get(), &writing)); });
  ASSERT_EQ(writing.get_future().wait_for(kTimeout), std::future_status::ready);
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

}  // namespace
