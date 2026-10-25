///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///
/// LatestSlot Test Suite
///
/// Tests the latest-only pattern: a realtime reading, where only the newest message matters, read with
/// ActiveReadReactor under Delivery::kPull and ReadPacing::kContinuous, and an application that keeps
/// the newest message in a LatestSlot for a consumer slower than the stream.
///
/// The slot's own contract comes first: when Put() asks for a notification, what Take() returns, and
/// what is counted as replaced. A stress test then runs a producer against a consumer emulating an event
/// loop. The reactor tests last show what the pattern promises an application: the stream never waits
/// for the consumer, at most one notification waits in the queue, the consumer only ever moves forward,
/// every reading is either handled or replaced, and the last one is never lost.
///
/// @see /docs/testing.md for comprehensive test documentation

#include "applications/reactor/latest_slot.h"

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <functional>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/continuous_use_support.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace {

using continuous_use_test::AppLoop;
using continuous_use_test::kReadings;
using continuous_use_test::ReadingService;
using RpcReactor::LatestSlot;
using Delivery = routeguide::ListFeatures::Delivery;
using ReadPacing = routeguide::ListFeatures::ReadPacing;
using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>;
using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull, ReadPacing::kContinuous>;

constexpr auto kTimeout = std::chrono::seconds(5);

/// One reading: a message copied out of the reactor, with its number in the stream, counted from 1.
struct Reading {
  int seq;
  routeguide::Feature feature;
};

// =============================================================================
// The slot's contract
// =============================================================================

/// @test The first value asks for a notification, since nobody is to be told about it yet.
TEST(LatestSlotTest, FirstPut_AsksForANotification) {
  LatestSlot<int> slot;
  EXPECT_TRUE(slot.Put(1));
  EXPECT_EQ(slot.Replaced(), 0u);
}

/// @test A value put before the previous one was taken replaces it, is counted as a replacement, and asks
/// for no notification: the one still pending covers it. Take() returns the newest.
TEST(LatestSlotTest, PutBeforeTake_ReplacesTheValue_AndAsksForNoNotification) {
  LatestSlot<int> slot;
  ASSERT_TRUE(slot.Put(1));
  EXPECT_FALSE(slot.Put(2));
  EXPECT_FALSE(slot.Put(3));
  EXPECT_EQ(slot.Replaced(), 2u);
  EXPECT_EQ(slot.Take(), std::optional<int>(3));
}

/// @test Take() empties the slot: a second Take() finds nothing, and the next Put() asks for a
/// notification again.
TEST(LatestSlotTest, Take_EmptiesTheSlot_SoTheNextPutAsksAgain) {
  LatestSlot<int> slot;
  EXPECT_EQ(slot.Take(), std::nullopt);
  ASSERT_TRUE(slot.Put(1));
  EXPECT_EQ(slot.Take(), std::optional<int>(1));
  EXPECT_EQ(slot.Take(), std::nullopt);
  EXPECT_TRUE(slot.Put(2));
  EXPECT_EQ(slot.Replaced(), 0u);
}

/// @test The slot keeps a move-only value, so it can carry a message owned under Delivery::kPush too.
TEST(LatestSlotTest, MoveOnlyValue_IsMovedThrough) {
  LatestSlot<std::unique_ptr<int>> slot;
  ASSERT_TRUE(slot.Put(std::make_unique<int>(7)));
  auto value = slot.Take();
  ASSERT_TRUE(value.has_value());
  ASSERT_NE(*value, nullptr);
  EXPECT_EQ(**value, 7);
}

/// @test A producer puts values as fast as it can while a consumer, notified through an event loop of its
/// own, takes them. At most one notification is ever queued, the consumer only moves forward, every value
/// is either taken or replaced, and the consumer ends on the last value.
TEST(LatestSlotTest, ProducerAgainstNotifiedConsumer_KeepsEveryPromise) {
  constexpr int kValues = 100000;
  LatestSlot<int> slot;
  std::atomic<int> queued_notifications{0};
  std::atomic<int> max_queued_notifications{0};
  std::vector<int> taken;  // Consumer thread only, until the loop is joined.
  {
    AppLoop loop;
    const auto on_latest = [&] {
      --queued_notifications;
      if (auto value = slot.Take()) taken.push_back(*value);
    };
    for (int i = 1; i <= kValues; ++i) {
      if (slot.Put(i)) {
        const int queued = ++queued_notifications;
        int max = max_queued_notifications.load();
        while (queued > max && !max_queued_notifications.compare_exchange_weak(max, queued)) {}
        loop.Post(on_latest);
      }
    }
    // The producer's end, queued behind every notification, as a reactor's OnDone is behind its reads.
    loop.Post([&] {
      if (auto value = slot.Take()) taken.push_back(*value);
    });
  }

  EXPECT_LE(max_queued_notifications.load(), 1) << "More than one notification waited in the queue";
  ASSERT_FALSE(taken.empty());
  EXPECT_TRUE(std::is_sorted(taken.begin(), taken.end()) &&
              std::adjacent_find(taken.begin(), taken.end()) == taken.end())
      << "The consumer saw a value twice, or an older value after a newer one";
  EXPECT_EQ(taken.back(), kValues) << "The last value was lost";
  EXPECT_EQ(taken.size() + slot.Replaced(), static_cast<size_t>(kValues))
      << "A value was neither taken nor counted as replaced";
}

// =============================================================================
// With the reactor: a realtime reading under Delivery::kPull and ReadPacing::kContinuous
// =============================================================================

class LatestOnlyReadTest : public RouteGuideTestFixtureBase<ReadingService> {
 protected:
  /// What the consumer saw, reported once the RPC is done and the slot drained.
  struct Outcome {
    grpc::Status status;
    std::vector<int> handled;  // Sequence numbers, in the order they were handled.
    std::uint64_t replaced = 0;
  };

  /// Copies each lent message into the slot, numbered, and notifies through `notify` only when the slot
  /// was empty, exactly as an application's read_ok would.
  Callbacks MakeCallbacks(LatestSlot<Reading>& slot, std::atomic<int>& read_seq,
                          std::function<void()> notify) {
    Callbacks cbs;
    cbs.read_ok = [&slot, &read_seq, notify](auto*, const routeguide::Feature& response) {
      const int seq = ++read_seq;
      EXPECT_EQ(response.location().longitude(), seq) << "The stream arrived out of order";
      if (slot.Put(Reading{seq, response})) notify();
    };
    return cbs;
  }
};

/// @test A consumer that handles nothing while the stream runs does not slow it down: the RPC completes,
/// one notification was asked for, and the slot holds the last reading, every other one replaced.
///
/// The consumer stands blocked, as an application thread busy elsewhere would. A pacing that held messages
/// for the application would stall the stream on its first one; kContinuous is what lets it run on.
TEST_F(LatestOnlyReadTest, BlockedConsumer_StreamRunsOn_AndOnlyTheLastReadingIsLeft) {
  LatestSlot<Reading> slot;
  std::atomic<int> read_seq{0};
  std::atomic<int> notifications{0};
  std::promise<grpc::Status> done_promise;
  auto done_future = done_promise.get_future();

  auto cbs = MakeCallbacks(slot, read_seq, [&notifications] { ++notifications; });
  cbs.done = [&done_promise](auto*, const grpc::Status& status) { done_promise.set_value(status); };
  routeguide::Rectangle request;
  auto reactor = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }))
      << "The stream waited for a consumer that never took anything";
  EXPECT_TRUE(done_future.get().ok());
  // Returns once OnDone() has run to its end, after which the reactor may be destroyed.
  ASSERT_TRUE(reactor->CancelAndWait(kTimeout));
  EXPECT_EQ(read_seq.load(), kReadings);
  EXPECT_EQ(notifications.load(), 1) << "A burst of readings must ask for one notification only";
  EXPECT_EQ(slot.Replaced(), static_cast<std::uint64_t>(kReadings - 1));
  const auto last = slot.Take();
  ASSERT_TRUE(last.has_value());
  EXPECT_EQ(last->seq, kReadings);
  EXPECT_EQ(last->feature.name(), "reading " + std::to_string(kReadings));
}

/// @test A consumer slower than the stream, on an event loop of its own, as in the demo: each notification
/// is a task that takes the newest reading and spends a while on it, and the done callback queues a last
/// task that drains the slot. The consumer only moves forward, at most one notification waits at a time,
/// every reading is either handled or replaced, and the last one handled is the last one sent.
TEST_F(LatestOnlyReadTest, SlowConsumer_HandlesOnlyNewerReadings_AndEndsOnTheLast) {
  LatestSlot<Reading> slot;
  std::atomic<int> read_seq{0};
  std::atomic<int> queued_notifications{0};
  std::atomic<int> max_queued_notifications{0};
  std::promise<Outcome> outcome_promise;
  auto outcome_future = outcome_promise.get_future();
  Outcome outcome;  // Written by the loop's tasks only, then handed over through the promise.
  std::unique_ptr<ClientReactor> reactor;
  // Built before the loop's thread starts, which is what orders it before the tasks using it.
  const auto handle = [&](const Reading& reading) {
    outcome.handled.push_back(reading.seq);
    std::this_thread::sleep_for(std::chrono::microseconds(500));  // A display, slower than the stream.
  };
  {
    AppLoop loop;
    auto cbs = MakeCallbacks(slot, read_seq, [&] {
      const int queued = ++queued_notifications;
      int max = max_queued_notifications.load();
      while (queued > max && !max_queued_notifications.compare_exchange_weak(max, queued)) {}
      loop.Post([&] {
        --queued_notifications;
        if (auto reading = slot.Take()) handle(*reading);
      });
    });
    cbs.done = [&](auto*, const grpc::Status& status) {
      loop.Post([&, status] {
        if (auto reading = slot.Take()) handle(*reading);
        outcome.status = status;
        outcome.replaced = slot.Replaced();
        outcome_promise.set_value(std::move(outcome));
      });
    };
    routeguide::Rectangle request;
    reactor = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), request, std::move(cbs));
    ASSERT_TRUE(AwaitRpc(outcome_future, [&] { reactor->TryCancel(); }));
    // Every callback has run before the outcome is ready, but OnDone() may still be returning.
    ASSERT_TRUE(reactor->CancelAndWait(kTimeout));
  }

  const Outcome result = outcome_future.get();
  EXPECT_TRUE(result.status.ok()) << result.status.error_message();
  EXPECT_EQ(read_seq.load(), kReadings);
  EXPECT_LE(max_queued_notifications.load(), 1) << "More than one notification waited in the queue";
  ASSERT_FALSE(result.handled.empty());
  EXPECT_TRUE(std::is_sorted(result.handled.begin(), result.handled.end()) &&
              std::adjacent_find(result.handled.begin(), result.handled.end()) == result.handled.end())
      << "A reading was handled twice, or an older one after a newer one";
  EXPECT_EQ(result.handled.back(), kReadings) << "The last reading was lost";
  EXPECT_EQ(result.handled.size() + result.replaced, static_cast<size_t>(kReadings))
      << "A reading was neither handled nor counted as replaced";
}

}  // namespace
