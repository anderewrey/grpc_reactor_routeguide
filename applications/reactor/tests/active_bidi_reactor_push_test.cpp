///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///
/// ActiveBidiReactor Test Suite
///
/// Tests the bidirectional streaming reactor pattern using RouteChat RPC.
/// Uses std::promise/future for synchronization.
///
/// The test fixture creates:
/// - An in-process gRPC server with RouteChat implementation
/// - Client reactors with callbacks that complete promises directly
/// - Location-based note storage for deterministic test scenarios
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>
#include <grpcpp/client_context.h>

#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <cstdio>
#include <cstdlib>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <semaphore>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/read_pacing_test_support.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"


namespace {

/// Every reactor in this file is instantiated with Delivery::kPush; its pull counterpart is
/// covered by active_bidi_reactor_pull_test.cpp.
constexpr auto kDelivery = RpcReactor::Client::Delivery::kPush;
constexpr auto kSubmission = RpcReactor::Client::Submission::kApplicationPushes;

using routeguide::RouteChat::ReadPacing;

// =============================================================================
// Compile-time API checks
// =============================================================================

/// GetResponse() is a Delivery::kPull method, so no push reactor offers it, whatever its pacing.
static_assert(!pacing_test::HasGetResponse<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kTurnByTurn>,
              routeguide::RouteNote>);
static_assert(!pacing_test::HasGetResponse<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kPerMessage>,
              routeguide::RouteNote>);
/// ResumeRead() releases a read hold, and kContinuous never takes one, so it does not exist there.
static_assert(!pacing_test::HasResumeRead<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kContinuous>>);
static_assert(pacing_test::HasResumeRead<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kTurnByTurn>>);
static_assert(pacing_test::HasResumeRead<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kPerMessage>>);
/// The read_ok slot takes the message as owned, and returns the hold decision only under kPerMessage.
static_assert(std::is_same_v<
              routeguide::RouteChat::Callbacks<kDelivery, kSubmission, ReadPacing::kPerMessage>::OnReadDoneOkCallback,
              std::function<bool(grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                                 std::unique_ptr<routeguide::RouteNote>)>>);
/// kPerMessage is the default ReadPacing, in the generic layer and in the RouteGuide one alike: naming only
/// the Delivery selects it.
static_assert(
    std::is_same_v<
        RpcReactor::Client::ActiveBidiReactor<routeguide::RouteNote, routeguide::RouteNote, kDelivery, kSubmission>,
        RpcReactor::Client::ActiveBidiReactor<routeguide::RouteNote, routeguide::RouteNote, kDelivery, kSubmission,
                                              ReadPacing::kPerMessage>>);
static_assert(std::is_same_v<routeguide::RouteChat::ClientReactor<kDelivery, kSubmission>,
                             routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kPerMessage>>);

/// Test service implementing RouteChat for bidirectional streaming tests.
/// Stores received notes by location and sends back all previously stored notes
/// at the same location for each received note.
class TestRouteGuideService final : public routeguide::RouteGuide::CallbackService {
 public:
  /// Configure server to close after N messages (for ServerClosesFirst test)
  void SetMaxMessages(int max) {
    max_messages_ = max;
  }

  void ClearMaxMessages() {
    max_messages_ = -1;
  }

  /// RouteChat: Bidirectional streaming - receive notes, send back notes at same location
  grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote>* RouteChat(
      grpc::CallbackServerContext* context) override {
    class RouteChatReactor : public grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote> {
     public:
      explicit RouteChatReactor(int max_messages)
          : max_messages_(max_messages) {
        StartRead(&incoming_note_);
      }

      void OnReadDone(bool ok) override {
        if (!ok) {
          // Client finished sending - complete the RPC
          Finish(grpc::Status::OK);
          return;
        }

        message_count_++;

        // Check if we should close after max messages
        if (max_messages_ > 0 && message_count_ >= max_messages_) {
          Finish(grpc::Status::OK);
          return;
        }

        // Get location key
        auto location = std::make_pair(incoming_note_.location().latitude(),
                                       incoming_note_.location().longitude());

        // Get all previously stored notes at this location
        auto& notes_at_location = notes_by_location_[location];

        // Queue responses: send all stored notes at this location
        for (const auto& note : notes_at_location) {
          pending_responses_.push_back(note);
        }

        // Store the incoming note for future lookups
        notes_at_location.push_back(incoming_note_);

        // Start sending responses or continue reading
        if (!pending_responses_.empty() && !writing_) {
          SendNextResponse();
        } else if (pending_responses_.empty()) {
          // No responses to send, continue reading
          StartRead(&incoming_note_);
        }
      }

      void OnWriteDone(bool ok) override {
        writing_ = false;
        if (!ok) {
          Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Write failed"));
          return;
        }

        pending_responses_.erase(pending_responses_.begin());

        if (!pending_responses_.empty()) {
          SendNextResponse();
        } else {
          // All responses sent, continue reading
          StartRead(&incoming_note_);
        }
      }

      void OnDone() override {
        delete this;
      }

     private:
      void SendNextResponse() {
        writing_ = true;
        outgoing_note_ = pending_responses_.front();
        StartWrite(&outgoing_note_);
      }

      routeguide::RouteNote incoming_note_;
      routeguide::RouteNote outgoing_note_;
      std::map<std::pair<int, int>, std::vector<routeguide::RouteNote>> notes_by_location_;
      std::vector<routeguide::RouteNote> pending_responses_;
      bool writing_ = false;
      int max_messages_;
      int message_count_ = 0;
    };

    return new RouteChatReactor(max_messages_);
  }

 private:
  int max_messages_ = -1;  // -1 means no limit
};

/// Result container for RouteChat tests
struct RouteChatResult {
  grpc::Status status;
  std::vector<routeguide::RouteNote> received_notes;
  int write_done_count = 0;
  bool read_nok_received = false;
  bool completed = false;
};

/// Test fixture with in-process server
class ActiveBidiReactorTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {
 protected:
  void TearDown() override {
    RouteGuideTestFixtureBase::TearDown();
    test_service_.ClearMaxMessages();
  }

  std::unique_ptr<grpc::ClientContext> CreateClientContextWithDeadline(
      std::chrono::milliseconds timeout) {
    auto context = std::make_unique<grpc::ClientContext>();
    context->set_deadline(std::chrono::system_clock::now() + timeout);
    return context;
  }
};

/// A RouteChat reactor's base, as the callbacks receive it.
using BidiReactorBase = grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>;

/// Fixture for the behaviours that must hold identically under every read pacing. The pacing changes
/// only who arms the next read, so everything an application observes on either direction is expected
/// to be the same under all of them.
/// @tparam PacingTagT a pacing_test::PacingTag, selecting the reactor's ReadPacing
template <class PacingTagT>
class ActiveBidiReactorPacingTest : public ActiveBidiReactorTest {
 protected:
  static constexpr ReadPacing kPacing = PacingTagT::value;
  using Reactor = routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, kPacing>;
  using Callbacks = routeguide::RouteChat::Callbacks<kDelivery, kSubmission, kPacing>;

  /// Builds a read_ok callback that gives every owned message to `keep`, then consumes it the way the
  /// pacing requires: nothing more under kContinuous, where the reactor re-arms by itself, and a
  /// ResumeRead() under kPerMessage and kTurnByTurn, where the callback keeps the message held. The
  /// message is handed to `keep` before the release, because ResumeRead() arms the next read, and the
  /// next read_ok could then run before this one returns. ResumeRead()'s result is not checked: unlike
  /// a read-only stream, a failed write or a cancel can end the RPC while the hold stands, and the
  /// release then rightly arms nothing.
  template <class KeepFn>
  static typename Callbacks::OnReadDoneOkCallback ConsumeEach(KeepFn keep) {
    return [keep]([[maybe_unused]] BidiReactorBase* reactor, std::unique_ptr<routeguide::RouteNote> message) {
      keep(std::move(*message));
      if constexpr (kPacing != ReadPacing::kContinuous) {
        static_cast<Reactor*>(reactor)->ResumeRead();
        return pacing_test::KeepHeld<kPacing>();
      }
    };
  }
};

TYPED_TEST_SUITE(ActiveBidiReactorPacingTest, pacing_test::AllPacings);

/// Fixture for the behaviours of a held message, under the two pacings that hold one: kTurnByTurn
/// always, and kPerMessage when read_ok returns true.
/// @tparam PacingTagT a pacing_test::PacingTag, selecting kPerMessage or kTurnByTurn
template <class PacingTagT>
class ActiveBidiReactorHeldPacingTest : public ActiveBidiReactorTest {
 protected:
  static constexpr ReadPacing kPacing = PacingTagT::value;
  using Reactor = routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, kPacing>;
  using Callbacks = routeguide::RouteChat::Callbacks<kDelivery, kSubmission, kPacing>;
};

TYPED_TEST_SUITE(ActiveBidiReactorHeldPacingTest, pacing_test::HeldPacings);

/// The reactor and callbacks of the tests pinned to ReadPacing::kContinuous.
using ContinuousReactor = routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kContinuous>;
using ContinuousCallbacks = routeguide::RouteChat::Callbacks<kDelivery, kSubmission, ReadPacing::kContinuous>;

// =============================================================================
// Every pacing: RouteChat bidirectional streaming behaviours
// =============================================================================
//
// These cases close the write side with SendLastRequest() rather than CloseRequestStream(). The fake
// server echoes every queued note before it reads the client's half-close, so the RPC then ends only
// once every echo was read, and the read side reports its own end through read_nok, under every pacing.

/// @test Validates basic send/receive flow with multiple notes.
///
/// Tests the RouteChat echo behavior:
/// - Send 3 notes to the same location, the last one with SendLastRequest()
/// - First note: expect 0 responses (no history)
/// - Second note: expect 1 response (first note)
/// - Third note: expect 2 responses (first + second)
/// - Total expected responses: 0 + 1 + 2 = 3, in the order the server sent them
/// - The read stream ends with read_nok, then OnDone fires with OK
TYPED_TEST(ActiveBidiReactorPacingTest, RouteChat_SendReceive_MatchesNotes) {
  using Reactor = typename TestFixture::Reactor;

  std::promise<RouteChatResult> result_promise;
  std::future<RouteChatResult> result_future = result_promise.get_future();

  std::vector<routeguide::RouteNote> received_notes;
  std::mutex notes_mutex;
  std::atomic<bool> read_nok_fired{false};

  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&received_notes, &notes_mutex](routeguide::RouteNote&& note) {
    const std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(std::move(note));
  });
  cbs.read_nok = [&read_nok_fired](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {
    read_nok_fired = true;
  };
  cbs.write_done = [&write_slot](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool) { write_slot.release(); };
  cbs.done = [&result_promise, &received_notes, &notes_mutex, &read_nok_fired](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    RouteChatResult result;
    result.status = status;
    {
      const std::lock_guard<std::mutex> lock(notes_mutex);
      result.received_notes = received_notes;
    }
    result.read_nok_received = read_nok_fired.load();
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  // Send 3 notes to the same location, the last one closing the write side
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5)));
  EXPECT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("First note", 100, 200)));
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "First write never completed";
  EXPECT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("Second note", 100, 200)));
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Second write never completed";
  EXPECT_TRUE(reactor->SendLastRequest(rg_utils::MakeRouteNote("Third note", 100, 200)));

  ASSERT_TRUE(this->AwaitRpc(result_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for RPC completion";

  const RouteChatResult result = result_future.get();

  EXPECT_TRUE(result.completed);
  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  EXPECT_TRUE(result.read_nok_received) << "read_nok should fire when the read stream ends";

  // Expected responses: 0 (first) + 1 (second) + 2 (third) = 3
  ASSERT_EQ(result.received_notes.size(), 3);
  EXPECT_EQ(result.received_notes[0].message(), "First note");
  EXPECT_EQ(result.received_notes[1].message(), "First note");
  EXPECT_EQ(result.received_notes[2].message(), "Second note");
}

/// @test Validates interleaved messages to different locations.
///
/// Tests that messages to different locations are handled independently:
/// - Send notes to location A, then location B, the last one with SendLastRequest()
/// - Verify responses arrive correctly for each location
TYPED_TEST(ActiveBidiReactorPacingTest, RouteChat_InterleavedMessages_AllReceived) {
  using Reactor = typename TestFixture::Reactor;

  std::promise<RouteChatResult> result_promise;
  std::future<RouteChatResult> result_future = result_promise.get_future();

  std::vector<routeguide::RouteNote> received_notes;
  std::mutex notes_mutex;

  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&received_notes, &notes_mutex](routeguide::RouteNote&& note) {
    const std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(std::move(note));
  });
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&write_slot](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool) { write_slot.release(); };
  cbs.done = [&result_promise, &received_notes, &notes_mutex](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    RouteChatResult result;
    result.status = status;
    {
      const std::lock_guard<std::mutex> lock(notes_mutex);
      result.received_notes = received_notes;
    }
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  // Send notes to different locations in interleaved pattern
  // Location A (100, 200): 2 notes -> expect 0 + 1 = 1 response
  // Location B (300, 400): 2 notes -> expect 0 + 1 = 1 response
  // Total: 2 responses
  std::vector<routeguide::RouteNote> notes;
  notes.push_back(rg_utils::MakeRouteNote("A1", 100, 200));  // Location A, first
  notes.push_back(rg_utils::MakeRouteNote("B1", 300, 400));  // Location B, first
  notes.push_back(rg_utils::MakeRouteNote("A2", 100, 200));  // Location A, second (gets A1)
  notes.push_back(rg_utils::MakeRouteNote("B2", 300, 400));  // Location B, second (gets B1), closes the stream

  for (size_t i = 0; i < notes.size(); ++i) {
    EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Previous write never completed";
    if (i + 1 < notes.size()) {
      EXPECT_TRUE(reactor->SendRequest(std::move(notes[i])));
    } else {
      EXPECT_TRUE(reactor->SendLastRequest(std::move(notes[i])));
    }
  }

  ASSERT_TRUE(this->AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RouteChatResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  ASSERT_EQ(result.received_notes.size(), 2);  // A2 gets A1, B2 gets B1

  // The stream delivers messages in send order (A1, B1, A2, B2), so the server's two echoed
  // responses arrive in a fixed, non-racy order: A1's note first (echoed once A2 arrives at
  // location A), then B1's note (echoed once B2 arrives at location B). Checking their content
  // catches a server that got the per-location bucketing wrong but still echoed the right count.
  EXPECT_EQ(result.received_notes[0].message(), "A1");
  EXPECT_EQ(result.received_notes[0].location().latitude(), 100);
  EXPECT_EQ(result.received_notes[0].location().longitude(), 200);
  EXPECT_EQ(result.received_notes[1].message(), "B1");
  EXPECT_EQ(result.received_notes[1].location().latitude(), 300);
  EXPECT_EQ(result.received_notes[1].location().longitude(), 400);
}

/// @test Validates client closes first, server continues sending.
///
/// Tests that after CloseRequestStream(), remaining server messages are received:
/// - Send 3 notes to one location, which draws 3 echoes
/// - Call CloseRequestStream() as soon as the last write completes, without waiting for the echoes
/// - Every echo the server still sends after the close is received, under every pacing: a message
///   released after the close completes must still re-arm the read
TYPED_TEST(ActiveBidiReactorPacingTest, RouteChat_ClientClosesFirst_ServerContinues) {
  using Reactor = typename TestFixture::Reactor;

  std::promise<RouteChatResult> result_promise;
  std::future<RouteChatResult> result_future = result_promise.get_future();

  std::vector<routeguide::RouteNote> received_notes;
  std::mutex notes_mutex;
  std::atomic<bool> read_nok_fired{false};

  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&received_notes, &notes_mutex](routeguide::RouteNote&& note) {
    const std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(std::move(note));
  });
  cbs.read_nok = [&read_nok_fired](BidiReactorBase*) { read_nok_fired = true; };
  cbs.write_done = [&write_slot](BidiReactorBase*, bool) { write_slot.release(); };
  cbs.done = [&result_promise, &received_notes, &notes_mutex, &read_nok_fired](BidiReactorBase*,
                                                                               const grpc::Status& status) {
    RouteChatResult result;
    result.status = status;
    {
      const std::lock_guard<std::mutex> lock(notes_mutex);
      result.received_notes = received_notes;
    }
    result.read_nok_received = read_nok_fired.load();
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  for (const char* text : {"Note 1", "Note 2", "Note 3"}) {
    EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Previous write never completed";
    EXPECT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote(text, 100, 200)));
  }
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Last write never completed";

  // Close client stream, while the server may still be sending its echoes
  EXPECT_TRUE(reactor->CloseRequestStream());

  ASSERT_TRUE(this->AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RouteChatResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  EXPECT_TRUE(result.read_nok_received) << "read_nok should fire when the read stream ends";
  // Note 2 draws Note 1, and Note 3 draws Note 1 and Note 2
  ASSERT_EQ(result.received_notes.size(), 3);
  EXPECT_EQ(result.received_notes[0].message(), "Note 1");
  EXPECT_EQ(result.received_notes[1].message(), "Note 1");
  EXPECT_EQ(result.received_notes[2].message(), "Note 2");
}

/// @test Validates server closes first, client continues.
///
/// Tests that when server stops accepting messages:
/// - Server configured to close after N messages
/// - read_nok fires when server done reading
/// - OnDone fires with appropriate status
TYPED_TEST(ActiveBidiReactorPacingTest, RouteChat_ServerClosesFirst_ClientContinues) {
  using Reactor = typename TestFixture::Reactor;

  // Configure server to close after 2 messages
  this->test_service_.SetMaxMessages(2);

  std::promise<RouteChatResult> result_promise;
  std::future<RouteChatResult> result_future = result_promise.get_future();

  std::atomic<bool> read_nok_fired{false};
  std::vector<routeguide::RouteNote> received_notes;
  std::mutex notes_mutex;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&received_notes, &notes_mutex](routeguide::RouteNote&& note) {
    const std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(std::move(note));
  });
  cbs.read_nok = [&read_nok_fired](
                     grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {
    read_nok_fired = true;
  };
  cbs.write_done = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                      bool) {};
  cbs.done = [&result_promise, &read_nok_fired](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    RouteChatResult result;
    result.status = status;
    result.read_nok_received = read_nok_fired.load();
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  // Send messages - server will close after 2
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 1", 100, 200));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 2", 100, 200));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // This might fail since server closed
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 3", 100, 200));

  ASSERT_TRUE(this->AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RouteChatResult result = result_future.get();

  // Server should have closed after 2 messages
  EXPECT_TRUE(result.completed);
  // read_nok should fire when server closes read stream
  EXPECT_TRUE(result.read_nok_received);
}

/// @test Validates TryCancel terminates bidirectional stream.
///
/// Tests that TryCancel correctly terminates an active bidi stream:
/// - Start stream and send some notes
/// - Call TryCancel() mid-stream
/// - Verify OnDone fires with CANCELLED status
TYPED_TEST(ActiveBidiReactorPacingTest, RouteChat_TryCancel_TriggersOnDone) {
  using Reactor = typename TestFixture::Reactor;

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([](routeguide::RouteNote&&) {});
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                      bool) {};
  cbs.done = [&done_promise](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  // Send a note then cancel
  reactor->SendRequest(rg_utils::MakeRouteNote("Note before cancel", 100, 200));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  reactor->TryCancel();

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

  // Cancel may result in CANCELLED or OK (if processed before cancel)
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::CANCELLED ||
              status.error_code() == grpc::StatusCode::OK)
      << "Expected CANCELLED or OK, got: " << status.error_code();
}

/// @test Validates context deadline propagates to bidirectional stream.
///
/// Tests that deadline exceeded is correctly reported:
/// - Set short deadline on context
/// - Server takes time to respond
/// - OnDone fires with DEADLINE_EXCEEDED status
TYPED_TEST(ActiveBidiReactorPacingTest, RouteChat_DeadlineExceeded_PropagatesStatus) {
  using Reactor = typename TestFixture::Reactor;

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([](routeguide::RouteNote&&) {
    // Slow down to help trigger deadline
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  });
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                      bool) {};
  cbs.done = [&done_promise](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    done_promise.set_value(status);
  };

  // Very short deadline
  auto reactor = std::make_unique<Reactor>(
      *this->stub_, this->CreateClientContextWithDeadline(std::chrono::milliseconds(50)), std::move(cbs));

  // Send notes to trigger activity
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 1", 100, 200));
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 2", 100, 200));
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 3", 100, 200));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

  // Should get DEADLINE_EXCEEDED, but may also get CANCELLED or OK depending on timing
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::DEADLINE_EXCEEDED ||
              status.error_code() == grpc::StatusCode::CANCELLED ||
              status.error_code() == grpc::StatusCode::OK)
      << "Expected DEADLINE_EXCEEDED, CANCELLED, or OK, got: " << status.error_code();
}

/// @test Validates multiple concurrent RouteChat streams complete independently.
///
/// Tests that multiple bidi streams can run concurrently:
/// - Create 3 concurrent RouteChat reactors
/// - Each sends its next note from its own write_done, so all streams write at the same time
/// - Each stream's last note goes out with SendLastRequest(), which closes its write side
/// - Wait for all OnDone callbacks
/// - Verify all complete successfully
TYPED_TEST(ActiveBidiReactorPacingTest, RouteChat_MultipleConcurrent_AllComplete) {
  using Reactor = typename TestFixture::Reactor;

  constexpr int kNumStreams = 3;
  constexpr int kNotesPerStream = 2;

  struct Stream {
    std::unique_ptr<Reactor> reactor;
    std::atomic<int> sent{0};
    std::atomic<int> received{0};
    std::promise<RouteChatResult> done;
  };
  std::array<Stream, kNumStreams> streams;

  // Each stream writes to its own location; the server echoes a note back once a second note
  // arrives at the same location.
  auto next_note = [](int stream, int note) {
    return rg_utils::MakeRouteNote("Note " + std::to_string(note + 1), stream * 100, stream * 100);
  };

  for (int i = 0; i < kNumStreams; ++i) {
    Stream& stream = streams[i];
    typename TestFixture::Callbacks cbs;
    cbs.read_ok = TestFixture::ConsumeEach([&stream](routeguide::RouteNote&&) { stream.received++; });
    cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
    // Issued from inside the write_done reaction, where OnWriteDone() has already re-armed the
    // idle-window hold, so no write gate is shared with the test thread.
    cbs.write_done = [&stream, &next_note, i](
                         grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                         bool ok) {
      EXPECT_TRUE(ok) << "Stream " << i << " write failed";
      const int note = stream.sent++;
      if (!ok || note >= kNotesPerStream) return;  // The last note already closed the write side
      if (note + 1 < kNotesPerStream) {
        EXPECT_TRUE(stream.reactor->SendRequest(next_note(i, note)));
      } else {
        EXPECT_TRUE(stream.reactor->SendLastRequest(next_note(i, note)));
      }
    };
    cbs.done = [&stream](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                         const grpc::Status& status) {
      RouteChatResult result;
      result.status = status;
      result.completed = true;
      stream.done.set_value(std::move(result));
    };

    stream.reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));
  }

  // Only each stream's first write comes from the test thread; its write_done sends the rest.
  for (int i = 0; i < kNumStreams; ++i) {
    streams[i].sent = 1;
    EXPECT_TRUE(streams[i].reactor->SendRequest(next_note(i, 0)));
  }

  std::array<std::future<RouteChatResult>, kNumStreams> futures;
  for (int i = 0; i < kNumStreams; ++i) {
    futures[i] = streams[i].done.get_future();
  }

  // The callbacks reference streams and next_note, so this test must not return while any stream is
  // still running: on a timeout, cancel every stream and wait for all of them before failing.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  bool timed_out = false;
  for (auto& future : futures) {
    timed_out |= future.wait_until(deadline) != std::future_status::ready;
  }
  if (timed_out) {
    for (auto& stream : streams) {
      stream.reactor->TryCancel();
    }
    for (auto& future : futures) {
      if (future.wait_for(std::chrono::seconds(5)) != std::future_status::ready) {
        std::fprintf(stderr, "A cancelled RouteChat stream never completed, aborting\n");
        std::abort();
      }
    }
    FAIL() << "Timeout waiting for the streams to complete";
  }

  for (int i = 0; i < kNumStreams; ++i) {
    const RouteChatResult result = futures[i].get();
    EXPECT_TRUE(result.completed) << "Stream " << i << " did not complete";
    EXPECT_TRUE(result.status.ok()) << "Stream " << i << " failed: " << result.status.error_message();
    // Second note should receive first note as response
    EXPECT_EQ(streams[i].received.load(), 1) << "Stream " << i << " should receive exactly 1 response";
  }
}

/// @test Validates an unbound read_ok slot does not stall the stream.
///
/// With no read_ok bound the reactor drops each message and re-arms its read anyway, so the RPC
/// still runs to completion and the read stream still reports its end.
/// The held pacings matter most here: with no callback bound there is nobody to release a hold, so the
/// reactor must arm the read itself rather than take a hold that nothing would ever release.
TYPED_TEST(ActiveBidiReactorPacingTest, RouteChat_NoReadOkCallbackBound_StreamStillDrains) {
  using Reactor = typename TestFixture::Reactor;

  std::atomic<bool> read_nok_fired{false};
  std::mutex write_mutex;
  std::condition_variable write_cv;
  bool write_ready = true;

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  typename TestFixture::Callbacks cbs;
  // read_ok deliberately left unbound: the reactor must drop each message and keep reading.
  cbs.read_nok = [&read_nok_fired](
                     grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {
    read_nok_fired = true;
  };
  cbs.write_done = [&write_mutex, &write_cv, &write_ready](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool) {
    std::lock_guard<std::mutex> lock(write_mutex);
    write_ready = true;
    write_cv.notify_one();
  };
  cbs.done = [&done_promise](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                             const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  // Three notes at one location give the server two echoes to send, both dropped by the reactor.
  for (int i = 0; i < 3; ++i) {
    {
      std::unique_lock<std::mutex> lock(write_mutex);
      ASSERT_TRUE(write_cv.wait_for(lock, std::chrono::seconds(5), [&write_ready] { return write_ready; }))
          << "Write " << i << " never completed";
      write_ready = false;
    }
    reactor->SendRequest(rg_utils::MakeRouteNote("dropped-" + std::to_string(i), 700, 800));
  }
  // A pending write rejects the close, so wait for the last one first.
  {
    std::unique_lock<std::mutex> lock(write_mutex);
    EXPECT_TRUE(write_cv.wait_for(lock, std::chrono::seconds(5), [&write_ready] { return write_ready; }))
        << "Last write never completed";
  }

  EXPECT_TRUE(reactor->CloseRequestStream());

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }))
      << "Stream stalled with no read_ok slot bound";
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_TRUE(read_nok_fired.load()) << "read_nok should fire when the read stream ends";
}

// =============================================================================
// Held pacings: kTurnByTurn, and kPerMessage with read_ok returning true
// =============================================================================

/// @test Validates the read hold is taken before the read_ok callback hands the message off.
///
/// Same forced order as the ActiveReadReactor test of the same name: the read_ok
/// callback hands the message to another thread, which runs ResumeRead() (its StartRead() and
/// RemoveHold()) to completion before the callback returns, then the callback lingers. Under kPerMessage
/// it then returns true, which leaves the release it already made as the only one.
/// - Send a note, then a second one to the same location with SendLastRequest()
/// - The server answers the second note with the first, then finishes the RPC
/// - That only message is consumed and resumed while read_ok is still running
///
/// Verifies that:
/// - OnDone never fires while a read_ok callback is still running
/// - The message is received, and the final status is OK
TYPED_TEST(ActiveBidiReactorHeldPacingTest, RouteChat_ResumedBeforeCallbackReturns_HoldsUntilReturn) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  std::vector<routeguide::RouteNote> received_notes;
  std::atomic<bool> done_fired{false};
  std::atomic<bool> done_inside_callback{false};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  std::binary_semaphore write_slot{0};  // Released once the first write completes

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&received_notes, &done_fired, &done_inside_callback](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>* reactor,
                    std::unique_ptr<routeguide::RouteNote> note) {
    auto* client_reactor = static_cast<Reactor*>(reactor);
    std::thread consumer([client_reactor, &note, &received_notes] {
      received_notes.push_back(std::move(*note));
      client_reactor->ResumeRead();
    });
    consumer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (done_fired) done_inside_callback = true;
    return pacing_test::KeepHeld<kPacing>();
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&write_slot](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool) { write_slot.release(); };
  cbs.done = [&done_promise, &done_fired](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    done_fired = true;
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  EXPECT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("First note", 100, 200)));
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "First write never completed";
  EXPECT_TRUE(reactor->SendLastRequest(rg_utils::MakeRouteNote("Second note", 100, 200)));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for RPC completion";

  const grpc::Status status = done_future.get();

  EXPECT_FALSE(done_inside_callback) << "OnDone fired while the read_ok callback was still running";
  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  ASSERT_EQ(received_notes.size(), 1);
  EXPECT_EQ(received_notes[0].message(), "First note");
}

/// @test Validates a read hold stalls only the read direction, not the write direction.
///
/// This is what makes a held pacing usable on a bidirectional RPC: gRPC counts holds and outstanding
/// operations on one per-RPC counter, but a hold only defers OnDone, it does not gate writes. An
/// application can therefore keep sending while it is behind on responses.
///
/// 1. Two notes at one location make the server echo the first one back
/// 2. That response arrives and the consumer does not resume, so a hold stands
/// 3. A third note is sent and its OnWriteDone fires while that hold is still outstanding
/// 4. Only then does the test resume, draining the two echoes the third note produced
///
/// Verifies that:
/// - The write completes with the read side held
/// - No further response arrives until ResumeRead() is called
/// - All 3 echoes arrive and the RPC completes with OK
TYPED_TEST(ActiveBidiReactorHeldPacingTest, RouteChat_ReadHoldDoesNotBlockWrites) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  bool write_ready = true;

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  // Waits for the next echo to be delivered, without resuming: the test thread owns the re-arm.
  auto wait_for_received = [&mutex, &cv, &received](int expected) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(5), [&received, expected] { return received == expected; });
  };
  auto wait_for_write = [&mutex, &cv, &write_ready]() {
    std::unique_lock<std::mutex> lock(mutex);
    const bool ready = cv.wait_for(lock, std::chrono::seconds(5), [&write_ready] { return write_ready; });
    write_ready = false;
    return ready;
  };

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    std::unique_ptr<routeguide::RouteNote>) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++received;
    }
    cv.notify_all();
    return pacing_test::KeepHeld<kPacing>();
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&mutex, &cv, &write_ready](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*, bool) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      write_ready = true;
    }
    cv.notify_all();
  };
  cbs.done = [&done_promise](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                             const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  // First note records history and draws no echo. Second note draws exactly one.
  ASSERT_TRUE(wait_for_write());
  ASSERT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("first", 500, 600)));
  ASSERT_TRUE(wait_for_write());
  ASSERT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("second", 500, 600)));
  ASSERT_TRUE(wait_for_write());
  ASSERT_TRUE(wait_for_received(1)) << "First echo never arrived";

  // The read side is now held. The write side must keep working through it.
  ASSERT_TRUE(reactor->SendLastRequest(rg_utils::MakeRouteNote("third", 500, 600)))
      << "SendLastRequest() rejected while the read side was held";
  EXPECT_TRUE(wait_for_write()) << "Write never completed while a read hold was outstanding";
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(received, 1) << "Read side delivered while it was supposed to be held";
  }

  // Drain: the third note echoes both earlier notes back, one resume at a time.
  for (int expected = 2; expected <= 3; ++expected) {
    EXPECT_TRUE(reactor->ResumeRead()) << "ResumeRead() rejected while a hold was outstanding";
    ASSERT_TRUE(wait_for_received(expected)) << "Echo " << expected << " never arrived after resuming";
  }
  EXPECT_TRUE(reactor->ResumeRead()) << "Final ResumeRead() must arm the read that ends the stream";

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }))
      << "Held bidi stream never completed after the last resume";
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, 3);
}

/// @test Validates a message held across CloseRequestStream() still lets the read side run to its end.
///
/// Closing the write side ends only the client's requests: the server may go on sending. Regression test
/// for a successful close that marked the whole stream as over, so the release of a message held across
/// it re-armed no read, and the RPC could never end. Three notes at one location draw three echoes:
///
/// 1. All three notes are sent, and echo 1 arrives and is kept held
/// 2. CloseRequestStream() closes the write side, and the test waits for that close to complete
/// 3. Each release must re-arm the read, until all three echoes are in and the read side ends
///
/// Verifies that:
/// - Every ResumeRead() after the close re-arms a read
/// - All 3 echoes arrive, in order, and read_nok fires
/// - OnDone fires with OK, with no TryCancel() needed
TYPED_TEST(ActiveBidiReactorHeldPacingTest, RouteChat_ReleasedAfterCloseRequestStream_ReadsOn) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  std::mutex mutex;
  std::condition_variable cv;
  std::vector<std::string> messages;
  std::atomic<bool> read_nok_fired{false};
  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  auto wait_for_received = [&mutex, &cv, &messages](size_t expected) {
    std::unique_lock<std::mutex> lock(mutex);
    return cv.wait_for(lock, std::chrono::seconds(5), [&messages, expected] { return messages.size() == expected; });
  };

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&mutex, &cv, &messages](BidiReactorBase*, std::unique_ptr<routeguide::RouteNote> note) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      messages.push_back(note->message());
    }
    cv.notify_all();
    return pacing_test::KeepHeld<kPacing>();  // The test thread releases each one
  };
  cbs.read_nok = [&read_nok_fired](BidiReactorBase*) { read_nok_fired = true; };
  cbs.write_done = [&write_slot](BidiReactorBase*, bool) { write_slot.release(); };
  cbs.done = [&done_promise](BidiReactorBase*, const grpc::Status& status) { done_promise.set_value(status); };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), std::move(cbs));

  for (const char* text : {"First note", "Second note", "Third note"}) {
    ASSERT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Previous write never completed";
    ASSERT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote(text, 100, 200)));
  }
  ASSERT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Last write never completed";
  ASSERT_TRUE(wait_for_received(1)) << "Echo 1 never arrived";

  ASSERT_TRUE(reactor->CloseRequestStream()) << "Close rejected on an idle write stream";
  // No callback reports the close completing, so give it time to: the defect showed only once it had.
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  for (size_t released = 1; released <= 3; ++released) {
    EXPECT_TRUE(reactor->ResumeRead()) << "Release " << released << " after the close re-armed no read";
    // Not an ASSERT: returning now would leave the RPC running against this test's locals.
    if (released < 3 && !wait_for_received(released + 1)) {
      ADD_FAILURE() << "Echo " << released + 1 << " never arrived after the close";
      break;
    }
  }

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "RPC never ended after the close";
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_TRUE(read_nok_fired) << "read_nok should fire when the read stream ends";
  const std::lock_guard<std::mutex> lock(mutex);
  ASSERT_EQ(messages.size(), 3);
  EXPECT_EQ(messages[0], "First note");
  EXPECT_EQ(messages[1], "First note");
  EXPECT_EQ(messages[2], "Second note");
}

// =============================================================================
// ReadPacing::kContinuous specific tests, and the write-side idle hold
// =============================================================================
//
// The write side is the same under every pacing, so its tests run on kContinuous, which takes no read
// hold to interfere with the idle-window hold they observe.

/// @test Validates the read side keeps flowing while the consumer only parks the messages.
///
/// The read_ok slot stores each owned message and never calls back into the reactor, which the
/// previous pull contract could not sustain: keeping a message meant leaving a hold outstanding
/// until GetResponse() released it, so a consumer that deferred stalled the RPC. Sending kNoteCount
/// notes to one location makes the server echo every note it stored earlier, so note i comes back
/// once per later note and the total is kNoteCount*(kNoteCount-1)/2.
TEST_F(ActiveBidiReactorTest, RouteChat_DeferredConsumer_StreamCompletesWithoutConsumerAction) {
  constexpr int kNoteCount = 8;
  constexpr size_t kExpectedResponses = kNoteCount * (kNoteCount - 1) / 2;

  // Parked messages: kept owned and untouched until the RPC is over.
  std::vector<std::unique_ptr<routeguide::RouteNote>> parked;
  std::mutex parked_mutex;
  std::condition_variable parked_cv;

  std::mutex write_mutex;
  std::condition_variable write_cv;
  bool write_ready = true;

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  ContinuousCallbacks cbs;
  cbs.read_ok = [&parked, &parked_mutex, &parked_cv](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    std::unique_ptr<routeguide::RouteNote> note) {
    std::lock_guard<std::mutex> lock(parked_mutex);
    parked.push_back(std::move(note));  // Defer everything: no processing, no reactor call
    parked_cv.notify_one();
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&write_mutex, &write_cv, &write_ready](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool) {
    std::lock_guard<std::mutex> lock(write_mutex);
    write_ready = true;
    write_cv.notify_one();
  };
  cbs.done = [&done_promise](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                             const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<ContinuousReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  for (int i = 0; i < kNoteCount; ++i) {
    {
      std::unique_lock<std::mutex> lock(write_mutex);
      ASSERT_TRUE(write_cv.wait_for(lock, std::chrono::seconds(5), [&write_ready] { return write_ready; }))
          << "Write " << i << " never completed";
      write_ready = false;
    }
    reactor->SendRequest(rg_utils::MakeRouteNote("note-" + std::to_string(i), 500, 600));
  }

  // Wait for every echo before closing: the fake server drops queued responses once it finishes.
  {
    std::unique_lock<std::mutex> lock(parked_mutex);
    ASSERT_TRUE(parked_cv.wait_for(lock, std::chrono::seconds(5),
                                   [&parked] { return parked.size() >= kExpectedResponses; }))
        << "Stream stalled while the consumer deferred processing, parked " << parked.size();
  }
  // And for the last write: its completion can trail the echoes, and a pending write rejects the close.
  {
    std::unique_lock<std::mutex> lock(write_mutex);
    EXPECT_TRUE(write_cv.wait_for(lock, std::chrono::seconds(5), [&write_ready] { return write_ready; }))
        << "Last write never completed";
  }

  EXPECT_TRUE(reactor->CloseRequestStream());

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for RPC completion";
  EXPECT_TRUE(done_future.get().ok());

  // Each parked message must still hold its own content, not a later message's.
  std::map<std::string, int> counts;
  {
    std::lock_guard<std::mutex> lock(parked_mutex);
    ASSERT_EQ(parked.size(), kExpectedResponses);
    for (const auto& note : parked) counts[note->message()]++;
  }
  for (int i = 0; i < kNoteCount - 1; ++i) {
    EXPECT_EQ(counts["note-" + std::to_string(i)], kNoteCount - 1 - i)
        << "Parked copies of note-" << i << " were overwritten";
  }
}


/// @test Validates the idle-window hold gates the write path.
///
/// The hold is the permission to write, so a second call while the first write is still in flight
/// finds it already claimed and must be rejected without reaching gRPC. Rejection is also what
/// enforces the one-write-in-flight rule, since no separate write-pending flag exists any more.
TEST_F(ActiveBidiReactorTest, RouteChat_OverlappingWrite_RejectedWhileHoldIsClaimed) {
  std::atomic<int> completed{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  ContinuousCallbacks cbs;
  cbs.read_ok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                   std::unique_ptr<routeguide::RouteNote>) {};
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&completed](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                                bool) { ++completed; };
  cbs.done = [&done_promise](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                            const grpc::Status& status) { done_promise.set_value(status); };

  auto reactor = std::make_unique<ContinuousReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Spams writes from this thread and checks the invariant the hold enforces, rather than betting on
  // a write not completing in time. An accepted write is only legal if every earlier one has
  // completed, so accepted can never exceed completed by more than the single one in flight. The loop
  // runs until it has seen both an acceptance and a rejection, since a fast transport can complete
  // many writes before the next call. Each note has its own location, so the server echoes none back.
  constexpr int kMaxAttempts = 1000;
  int accepted = 0;
  int rejected = 0;
  for (int i = 0; i < kMaxAttempts && (accepted == 0 || rejected == 0); ++i) {
    if (reactor->SendRequest(rg_utils::MakeRouteNote("note-" + std::to_string(i), 900 + i, 900))) {
      ++accepted;
    } else {
      ++rejected;
    }
    // MUTATION PROBE: a non-atomic claim lets two threads through; a missing claim lets this thread
    // through repeatedly. Both break the inequality below.
    ASSERT_LE(accepted, completed.load() + 1)
        << "Two writes were in flight at once, so the hold did not gate the write path";
  }
  EXPECT_GT(accepted, 0) << "Every write was rejected, so the hold was never armed";
  EXPECT_GT(rejected, 0) << "Every write was accepted, so the hold gated nothing";

  reactor->TryCancel();
  ASSERT_EQ(done_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
}

/// @test Validates the claim survives concurrent claimants.
///
/// The claim matters when two threads reach it at once. Two writer threads hammer the write path, and the
/// server finishes the RPC after a few dozen notes, so the gRPC thread's OnReadDone(false) also claims the
/// idle-window hold, to release it, while the writers still try to claim it. Each note goes to its own
/// location, so the server echoes nothing and the read stream ends only with the RPC.
/// The test asserts the end state: writes were accepted, and OnDone() follows. A claim that lets two
/// claimants through either issues two writes at once, which gRPC does not allow, or releases the hold
/// twice, which lets OnDone() overlap the reaction still running: either one makes the process crash, or
/// the RPC never end. Probabilistic by nature. Of three mutants run 200 times each, an unclaimed release in
/// OnReadDone(false) and a non-atomic claim in SendRequest() were caught in every batch of ten runs, while a
/// non-atomic claim in the release itself never was: its window is too narrow for this test to hit.
TEST_F(ActiveBidiReactorTest, RouteChat_ConcurrentClaimants_KeepTheHoldAccountingIntact) {
  test_service_.SetMaxMessages(40);
  std::atomic<int> accepted{0};
  std::atomic<int> location{0};
  std::atomic<bool> read_ended{false};

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  ContinuousCallbacks cbs;
  cbs.read_ok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                   std::unique_ptr<routeguide::RouteNote>) {};
  cbs.read_nok = [&read_ended](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {
    read_ended = true;
  };
  cbs.write_done = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*, bool) {};
  cbs.done = [&done_promise](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                            const grpc::Status& status) { done_promise.set_value(status); };

  auto reactor = std::make_unique<ContinuousReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Each writer keeps writing until the server has ended the read stream, and a little after, so the
  // writers are still claiming while OnReadDone(false) claims to release.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  auto writer = [&reactor, &accepted, &location, &read_ended, deadline] {
    int after_end = 0;
    while (after_end < 100 && std::chrono::steady_clock::now() < deadline) {
      if (reactor->SendRequest(rg_utils::MakeRouteNote("concurrent", 920, ++location))) ++accepted;
      if (read_ended) ++after_end;
    }
  };
  std::thread a(writer);
  std::thread b(writer);
  a.join();
  b.join();

  EXPECT_GT(accepted.load(), 0) << "Every concurrent write was rejected, so nothing was exercised";
  // The server finished the RPC meanwhile, so OnDone() follows with no close: the hold accounting survived
  // the writers and OnReadDone(false) claiming at once.
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }))
      << "RPC never completed, so the hold accounting drifted";
  EXPECT_TRUE(done_future.get().ok());
}

/// @test Validates the write path is closed deterministically once the read stream ends.
///
/// The pre-hold code could only narrow this window: it tested stream_no_more_ and a gRPC thread
/// could set the flag right afterwards. The release in OnReadDone(false) removes the hold instead,
/// so every later write finds nothing to claim and is rejected rather than racing into gRPC.
TEST_F(ActiveBidiReactorTest, RouteChat_WriteAfterReadStreamClosed_RejectedDeterministically) {
  test_service_.SetMaxMessages(1);  // Server finishes right after the first note

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();
  std::promise<void> nok_promise;
  std::future<void> nok_future = nok_promise.get_future();

  ContinuousCallbacks cbs;
  cbs.read_ok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                   std::unique_ptr<routeguide::RouteNote>) {};
  cbs.read_nok = [&nok_promise](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {
    nok_promise.set_value();
  };
  cbs.write_done = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*, bool) {};
  cbs.done = [&done_promise](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                            const grpc::Status& status) { done_promise.set_value(status); };

  auto reactor = std::make_unique<ContinuousReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  ASSERT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("only", 910, 910)))
      << "The setup write was rejected, so the rest of this test would pass vacuously";
  ASSERT_EQ(nok_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "Server never closed the read stream";

  // The read stream has ended, so the hold is gone for good and no re-arm can resurrect it.
  EXPECT_FALSE(reactor->SendRequest(rg_utils::MakeRouteNote("after close", 910, 910)))
      << "A write was accepted after the read stream ended";
  EXPECT_FALSE(reactor->CloseRequestStream()) << "A close was accepted after the read stream ended";

  ASSERT_EQ(done_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "RPC never completed, so a hold was left outstanding";
}

/// @test Validates the release claims its hold rather than assuming one is outstanding.
///
/// CloseRequestStream() consumes the hold, so by the time the server ends the read stream there is
/// nothing left to release. A release that skipped its claim would decrement gRPC's
/// outstanding-callback count a second time here, dropping it to zero while OnReadDone(false) is
/// still running and letting OnDone() overlap the reaction that gRPC promises it follows.
/// The suite cannot observe the underflow itself, since this gRPC is release-built and its own
/// check is compiled out, so the observable consequence is what gets asserted.
TEST_F(ActiveBidiReactorTest, RouteChat_ReleaseWithNoHold_DoesNotLetOnDoneOverlapTheReaction) {
  std::atomic<bool> nok_returned{false};
  std::atomic<bool> done_saw_nok_returned{false};

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  ContinuousCallbacks cbs;
  cbs.read_ok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                   std::unique_ptr<routeguide::RouteNote>) {};
  cbs.read_nok = [&nok_returned](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {
    // Widens the window so an OnDone() that fires from inside this reaction is observable rather
    // than merely possible.
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    nok_returned = true;
  };
  cbs.write_done = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*, bool) {};
  cbs.done = [&done_promise, &nok_returned, &done_saw_nok_returned](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    done_saw_nok_returned = nok_returned.load();
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<ContinuousReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Closing straight away consumes the idle-window hold while the write stream is still empty.
  ASSERT_TRUE(reactor->CloseRequestStream()) << "Close rejected on an idle write stream";

  ASSERT_EQ(done_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_TRUE(done_saw_nok_returned.load())
      << "OnDone() ran before OnReadDone(false) returned, so a hold was released without being held";
}


// =============================================================================
// ReadPacing::kPerMessage specific tests: the callback's runtime hold decision
// =============================================================================

/// @test Validates a late decline cannot release the read hold of the message that follows it.
///
/// The bidirectional counterpart of the ActiveReadReactor regression test of the same name. Three notes
/// at one location draw three echoes: one after the second note, and two after the third. The callbacks
/// of the first two echoes are ordered explicitly:
///
/// 1. Message 0's consumer thread calls ResumeRead(), which arms the read of message 1
/// 2. Message 0's callback waits until message 1's callback has started, then returns false
/// 3. Message 1's callback waits until message 0's has returned, then keeps its message by returning true
/// 4. The test thread checks the read side is held, then releases message 1 with ResumeRead()
///
/// Verifies that:
/// - Message 1 stays held after message 0's decline: OnDone stays pending and no further message arrives
/// - The application's ResumeRead() for message 1 finds the hold and is accepted
/// - All 3 echoes arrive and the RPC completes with OK
TEST_F(ActiveBidiReactorTest, RouteChat_PerMessage_DeclinedAfterResumeRead_LeavesNextHoldAlone) {
  using Reactor = routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kPerMessage>;

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  bool first_returning = false;
  bool second_kept = false;
  std::atomic<bool> first_resumed{false};
  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::RouteChat::Callbacks<kDelivery, kSubmission, ReadPacing::kPerMessage> cbs;
  cbs.read_ok = [&mutex, &cv, &received, &first_returning, &second_kept, &first_resumed](
                    BidiReactorBase* reactor, std::unique_ptr<routeguide::RouteNote>) {
    auto* client_reactor = static_cast<Reactor*>(reactor);
    std::unique_lock<std::mutex> lock(mutex);
    const int index = received++;
    cv.notify_all();
    if (index == 0) {
      lock.unlock();
      std::thread consumer([client_reactor, &first_resumed] { first_resumed = client_reactor->ResumeRead(); });
      consumer.join();
      lock.lock();
      // Bounded, so a gRPC that ran the two reactions one after the other only weakens the test.
      cv.wait_for(lock, std::chrono::seconds(2), [&received] { return received > 1; });
      first_returning = true;
      cv.notify_all();
      return false;
    }
    if (index == 1) {
      cv.wait_for(lock, std::chrono::seconds(2), [&first_returning] { return first_returning; });
      lock.unlock();
      // Leaves message 0's reaction time to run its claim after returning.
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
      lock.lock();
      second_kept = true;
      cv.notify_all();
      return true;
    }
    return false;
  };
  cbs.read_nok = [](BidiReactorBase*) {};
  cbs.write_done = [&write_slot](BidiReactorBase*, bool) { write_slot.release(); };
  cbs.done = [&done_promise](BidiReactorBase*, const grpc::Status& status) { done_promise.set_value(status); };

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));

  // The third note goes out while message 0's callback still waits for message 1, which it draws.
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5)));
  EXPECT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("First note", 100, 200)));
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "First write never completed";
  EXPECT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("Second note", 100, 200)));
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Second write never completed";
  EXPECT_TRUE(reactor->SendLastRequest(rg_utils::MakeRouteNote("Third note", 100, 200)));

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&second_kept] { return second_kept; }))
        << "Message 1 never reached its callback";
  }
  EXPECT_EQ(done_future.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout)
      << "RPC terminated while message 1 was kept";
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(received, 2) << "Message 0's decline released message 1's hold, and the read side read on";
  }
  EXPECT_TRUE(first_resumed) << "Message 0's own ResumeRead() was rejected";
  EXPECT_TRUE(reactor->ResumeRead()) << "Message 0's decline released message 1's hold";

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, 3);
}

}  // namespace
