///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///
/// Journal Test Suite
///
/// Tests the journal pattern: a stream where every message matters but the stream must never wait for the
/// application, such as telemetry ingested into a journal, read with ActiveReadReactor under
/// Delivery::kPush and ReadPacing::kContinuous. Each message is handed out owned and queued as its own
/// event, and the reactor reads on at once, so a consumer slower than the stream builds a backlog in the
/// event queue instead of slowing the stream down.
///
/// The tests show what the pattern promises an application, and what it costs: the stream ends at its own
/// pace while the consumer has written nothing, every message then reaches the consumer, once and in order,
/// and the backlog is bounded by nothing but the length of the stream.
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/continuous_use_support.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace {

using continuous_use_test::AppLoop;
using continuous_use_test::kReadings;
using continuous_use_test::ReadingService;
using Delivery = routeguide::ListFeatures::Delivery;
using ReadPacing = routeguide::ListFeatures::ReadPacing;
using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>;
using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush, ReadPacing::kContinuous>;

constexpr auto kTimeout = std::chrono::seconds(5);

/// Keeps an application loop's first task blocked until opened, standing for an application thread busy
/// elsewhere. Opens itself on destruction, so a failing test never leaves the loop blocked.
class Gate {
 public:
  /// The task to post first: it blocks the loop until Open().
  void Block(AppLoop& loop) {
    loop.Post([future = promise_.get_future().share()] { future.wait(); });
  }
  void Open() {
    if (!opened_.exchange(true)) promise_.set_value();
  }
  ~Gate() { Open(); }

 private:
  std::promise<void> promise_;
  std::atomic<bool> opened_{false};
};

class JournalReadTest : public RouteGuideTestFixtureBase<ReadingService> {
 protected:
  /// What the journal holds once it has caught up, reported by a task queued behind every record.
  struct Outcome {
    std::vector<int> journal;  // Each record's number, in the order it was written.
  };

  /// Queues each owned message on `loop` as its own task, as a read_ok handing it to EventLoop would, and
  /// counts it. The task reclaims the message and writes its number to `journal`, after `write_time`.
  Callbacks MakeCallbacks(AppLoop& loop, std::atomic<int>& read, std::atomic<int>& written,
                          std::vector<int>& journal, std::chrono::microseconds write_time) {
    Callbacks cbs;
    cbs.read_ok = [&, write_time](auto*, std::unique_ptr<routeguide::Feature> response) {
      ++read;
      // Released into the task as the event payload is released into EventLoop, and reclaimed there.
      loop.Post([&, write_time, message = response.release()] {
        const std::unique_ptr<routeguide::Feature> record{message};
        journal.push_back(static_cast<int>(record->location().longitude()));
        std::this_thread::sleep_for(write_time);
        ++written;
      });
    };
    return cbs;
  }
};

/// @test A journal that writes nothing while the stream runs does not slow it down: the RPC ends with
/// every reading queued and none written. Once the journal resumes, it writes every reading, once and in
/// order. The backlog reached the whole stream: nothing bounds it.
///
/// The journal stands blocked, as an application thread busy elsewhere would. A pacing that held messages
/// for the application would stall the stream on its first one; kContinuous is what lets it run on.
TEST_F(JournalReadTest, BlockedJournal_StreamEndsWithEveryRecordQueued_ThenAllAreWrittenInOrder) {
  std::atomic<int> read{0};
  std::atomic<int> written{0};
  std::vector<int> journal;  // Loop thread only, until the outcome is handed over.
  std::promise<grpc::Status> done_promise;
  auto done_future = done_promise.get_future();
  std::optional<int> written_at_done;
  std::promise<Outcome> outcome_promise;
  auto outcome_future = outcome_promise.get_future();
  std::unique_ptr<ClientReactor> reactor;
  {
    AppLoop loop;
    Gate gate;  // Destroyed before the loop, so the loop is never left blocked.
    gate.Block(loop);

    auto cbs = MakeCallbacks(loop, read, written, journal, std::chrono::microseconds(0));
    cbs.done = [&](auto*, const grpc::Status& status) {
      written_at_done = written.load();
      done_promise.set_value(status);
    };
    routeguide::Rectangle request;
    reactor = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), request, std::move(cbs));

    ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }))
        << "The stream waited for a journal that wrote nothing";
    EXPECT_TRUE(done_future.get().ok());
    // Returns once OnDone() has run to its end, after which the reactor may be destroyed.
    ASSERT_TRUE(reactor->CancelAndWait(kTimeout));
    EXPECT_EQ(read.load(), kReadings) << "The stream ended before every reading was read";
    EXPECT_EQ(written_at_done, std::optional<int>(0)) << "The journal wrote while it was blocked";

    gate.Open();
    loop.Post([&] { outcome_promise.set_value(Outcome{std::move(journal)}); });
    ASSERT_EQ(outcome_future.wait_for(kTimeout), std::future_status::ready) << "The journal never caught up";
  }

  const Outcome outcome = outcome_future.get();
  ASSERT_EQ(outcome.journal.size(), static_cast<size_t>(kReadings)) << "A reading was lost or written twice";
  for (int i = 0; i < kReadings; ++i) {
    EXPECT_EQ(outcome.journal[i], i + 1) << "Record " << i << " was written out of order";
  }
}

/// @test A journal slower than the stream, on an event loop of its own, as in the demo: the RPC ends while
/// records are still queued, every record read is either written or still queued at that point, and the
/// journal then writes every reading, once and in order.
TEST_F(JournalReadTest, SlowJournal_WritesEveryRecordInOrder_AfterTheRpcEnded) {
  std::atomic<int> read{0};
  std::atomic<int> written{0};
  std::vector<int> journal;  // Loop thread only, until the outcome is handed over.
  std::promise<grpc::Status> done_promise;
  auto done_future = done_promise.get_future();
  int read_at_done = 0;
  int written_at_done = 0;
  std::promise<Outcome> outcome_promise;
  auto outcome_future = outcome_promise.get_future();
  std::unique_ptr<ClientReactor> reactor;
  {
    AppLoop loop;
    auto cbs = MakeCallbacks(loop, read, written, journal, std::chrono::microseconds(500));
    cbs.done = [&](auto*, const grpc::Status& status) {
      read_at_done = read.load();
      written_at_done = written.load();
      // Queued behind every record, as an OnDone event is behind every read event.
      loop.Post([&] { outcome_promise.set_value(Outcome{std::move(journal)}); });
      done_promise.set_value(status);
    };
    routeguide::Rectangle request;
    reactor = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), request, std::move(cbs));

    ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
    EXPECT_TRUE(done_future.get().ok());
    ASSERT_TRUE(reactor->CancelAndWait(kTimeout));
    ASSERT_EQ(outcome_future.wait_for(kTimeout), std::future_status::ready) << "The journal never caught up";
  }

  EXPECT_EQ(read_at_done, kReadings) << "The RPC ended before every reading was read";
  EXPECT_LE(written_at_done, read_at_done);
  const Outcome outcome = outcome_future.get();
  ASSERT_EQ(outcome.journal.size(), static_cast<size_t>(kReadings)) << "A reading was lost or written twice";
  for (int i = 0; i < kReadings; ++i) {
    EXPECT_EQ(outcome.journal[i], i + 1) << "Record " << i << " was written out of order";
  }
}

}  // namespace
