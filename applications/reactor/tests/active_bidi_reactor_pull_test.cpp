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
#include <cstdio>
#include <cstdlib>
#include <functional>
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

/// Every reactor in this file is instantiated with Delivery::kPull; its push counterpart is
/// covered by active_bidi_reactor_push_test.cpp.
constexpr auto kDelivery = RpcReactor::Client::Delivery::kPull;
constexpr auto kSubmission = RpcReactor::Client::Submission::kApplicationPushes;

using routeguide::RouteChat::ReadPacing;

// =============================================================================
// Compile-time API checks
// =============================================================================

/// GetResponse() pulls a held message out, and kContinuous holds none, so it does not exist there.
static_assert(!pacing_test::HasGetResponse<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kContinuous>,
              routeguide::RouteNote>);
static_assert(pacing_test::HasGetResponse<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kTurnByTurn>,
              routeguide::RouteNote>);
static_assert(pacing_test::HasGetResponse<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kPerMessage>,
              routeguide::RouteNote>);
/// ResumeRead() is a Delivery::kPush method, so no pull reactor offers it, whatever its pacing.
static_assert(!pacing_test::HasResumeRead<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kTurnByTurn>>);
static_assert(!pacing_test::HasResumeRead<
              routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kPerMessage>>);
/// The read_ok slot is lent the message, and returns the hold decision only under kPerMessage.
static_assert(std::is_same_v<
              routeguide::RouteChat::Callbacks<kDelivery, kSubmission, ReadPacing::kContinuous>::OnReadDoneOkCallback,
              std::function<void(grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                                 const routeguide::RouteNote&)>>);
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
      grpc::CallbackServerContext* /*context*/) override {
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

  static std::unique_ptr<grpc::ClientContext> CreateClientContextWithDeadline(
      std::chrono::milliseconds timeout) {
    auto context = std::make_unique<grpc::ClientContext>();
    context->set_deadline(std::chrono::system_clock::now() + timeout);
    return context;
  }
};

/// A RouteChat reactor's base, as the callbacks receive it.
using BidiReactorBase = grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>;

/// The pacing of master's bool read_ok contract, which the ActiveBidiReactorTest cases written for it
/// keep: every message is held, unless read_ok returns false.
using PerMessageReactor = routeguide::RouteChat::ClientReactor<kDelivery, kSubmission, ReadPacing::kPerMessage>;
using PerMessageCallbacks = routeguide::RouteChat::Callbacks<kDelivery, kSubmission, ReadPacing::kPerMessage>;

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

  /// Builds a read_ok callback that gives a copy of every lent message to `keep`, then consumes it the
  /// way the pacing requires: nothing more under kContinuous, where the reactor re-arms by itself and
  /// the next read overwrites the lent message, and a GetResponse() under kPerMessage and kTurnByTurn,
  /// where the callback keeps the message held. The copy is handed to `keep` before the release,
  /// because GetResponse() arms the next read, and the next read_ok could then run before this one
  /// returns. The pulled message must match the lent one, which checks GetResponse() hands out the
  /// message it holds.
  template <class KeepFn>
  static Callbacks::OnReadDoneOkCallback ConsumeEach(KeepFn keep) {
    return [keep]([[maybe_unused]] BidiReactorBase* reactor, const routeguide::RouteNote& lent) {
      if constexpr (kPacing == ReadPacing::kContinuous) {
        keep(routeguide::RouteNote(lent));
      } else {
        const std::string lent_bytes = lent.SerializeAsString();
        keep(routeguide::RouteNote(lent));
        routeguide::RouteNote pulled;
        EXPECT_TRUE(static_cast<Reactor*>(reactor)->GetResponse(pulled)) << "GetResponse() rejected a held message";
        EXPECT_EQ(pulled.SerializeAsString(), lent_bytes) << "GetResponse() did not hand out the held message";
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
  using Reactor = TestFixture::Reactor;

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
  using Reactor = TestFixture::Reactor;

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
  using Reactor = TestFixture::Reactor;

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
  using Reactor = TestFixture::Reactor;

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
  using Reactor = TestFixture::Reactor;

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
  using Reactor = TestFixture::Reactor;

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
  using Reactor = TestFixture::Reactor;

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
  using Reactor = TestFixture::Reactor;

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
    const std::lock_guard<std::mutex> lock(write_mutex);
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


/// @test Validates the read hold is taken before the read_ok callback hands the response off.
///
/// Same forced order as the ActiveReadReactor test of the same name: the read_ok callback hands
/// the response to another thread, which runs GetResponse() (its StartRead() and RemoveHold()) to
/// completion before the callback returns, then the callback lingers.
/// - Send a note, then a second one to the same location with SendLastRequest()
/// - The server answers the second note with the first, then finishes the RPC
/// - That only response is consumed through GetResponse() while read_ok is still running
///
/// Verifies that:
/// - OnDone never fires while a read_ok callback is still running
/// - The response is received through GetResponse(), and the final status is OK
TYPED_TEST(ActiveBidiReactorHeldPacingTest, RouteChat_ResponseConsumedBeforeCallbackReturns_HoldsUntilReturn) {
  using Reactor = TestFixture::Reactor;
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
                    const routeguide::RouteNote&) {
    auto* client_reactor = static_cast<Reactor*>(reactor);
    std::thread consumer([client_reactor, &received_notes] {
      routeguide::RouteNote note;
      if (client_reactor->GetResponse(note)) received_notes.push_back(std::move(note));
    });
    consumer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (done_fired) done_inside_callback = true;
    return pacing_test::KeepHeld<kPacing>();  // Held: the response was already consumed through GetResponse()
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
/// - Every GetResponse() after the close hands its message out and re-arms a read
/// - All 3 echoes arrive, in order, and read_nok fires
/// - OnDone fires with OK, with no TryCancel() needed
TYPED_TEST(ActiveBidiReactorHeldPacingTest, RouteChat_ReleasedAfterCloseRequestStream_ReadsOn) {
  using Reactor = TestFixture::Reactor;
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
  cbs.read_ok = [&mutex, &cv, &messages](BidiReactorBase*, const routeguide::RouteNote& note) {
    {
      const std::lock_guard<std::mutex> lock(mutex);
      messages.push_back(note.message());
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
    routeguide::RouteNote note;
    EXPECT_TRUE(reactor->GetResponse(note)) << "GetResponse() " << released << " found no held message";
    EXPECT_FALSE(note.message().empty()) << "GetResponse() " << released << " handed out an empty message";
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
// ReadPacing::kPerMessage: master's bool read_ok contract, each message declined
// =============================================================================
//
// These cases predate the ReadPacing axis, when every pull reactor had a bool read_ok. kPerMessage is
// that contract unchanged, so they run on it as written: each read_ok copies the lent message and
// returns false, which makes the reactor claim its hold back and read on by itself.

/// @test Validates basic send/receive flow with multiple notes.
///
/// Tests the RouteChat echo behavior:
/// - Send 3 notes to the same location
/// - First note: expect 0 responses (no history)
/// - Second note: expect 1 response (first note)
/// - Third note: expect 2 responses (first + second)
/// - Total expected responses: 0 + 1 + 2 = 3
TEST_F(ActiveBidiReactorTest, RouteChat_SendReceive_MatchesNotes) {
  std::promise<RouteChatResult> result_promise;
  std::future<RouteChatResult> result_future = result_promise.get_future();

  std::vector<routeguide::RouteNote> received_notes;
  std::mutex notes_mutex;

  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time

  PerMessageCallbacks cbs;
  cbs.read_ok = [&received_notes, &notes_mutex](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    const routeguide::RouteNote& note) {
    const std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(note);
    return false;  // Continue reading immediately
  };
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

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send 3 notes to the same location
  std::vector<routeguide::RouteNote> sent_notes;
  sent_notes.push_back(rg_utils::MakeRouteNote("First note", 100, 200));
  sent_notes.push_back(rg_utils::MakeRouteNote("Second note", 100, 200));
  sent_notes.push_back(rg_utils::MakeRouteNote("Third note", 100, 200));

  for (auto& note : sent_notes) {
    EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Previous write never completed";
    EXPECT_TRUE(reactor->SendRequest(std::move(note)));
  }
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Last write never completed";

  // Give server time to send all responses
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Signal end of client stream
  reactor->CloseRequestStream();

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for RPC completion";

  const RouteChatResult result = result_future.get();

  EXPECT_TRUE(result.completed);
  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();

  // Expected responses: 0 (first) + 1 (second) + 2 (third) = 3
  EXPECT_EQ(result.received_notes.size(), 3);
}

/// @test Validates interleaved messages to different locations.
///
/// Tests that messages to different locations are handled independently:
/// - Send notes to location A, then location B
/// - Verify responses arrive correctly for each location
TEST_F(ActiveBidiReactorTest, RouteChat_InterleavedMessages_AllReceived) {
  std::promise<RouteChatResult> result_promise;
  std::future<RouteChatResult> result_future = result_promise.get_future();

  std::atomic<int> received_count{0};
  std::vector<routeguide::RouteNote> received_notes;
  std::mutex notes_mutex;

  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time

  PerMessageCallbacks cbs;
  cbs.read_ok = [&received_count, &received_notes, &notes_mutex](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    const routeguide::RouteNote& note) {
    received_count++;
    const std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(note);
    return false;
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&write_slot](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool) { write_slot.release(); };
  cbs.done = [&result_promise, &received_notes, &notes_mutex, &received_count](
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

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send notes to different locations in interleaved pattern
  // Location A (100, 200): 2 notes -> expect 0 + 1 = 1 response
  // Location B (300, 400): 2 notes -> expect 0 + 1 = 1 response
  // Total: 2 responses

  std::vector<routeguide::RouteNote> notes;
  notes.push_back(rg_utils::MakeRouteNote("A1", 100, 200));  // Location A, first
  notes.push_back(rg_utils::MakeRouteNote("B1", 300, 400));  // Location B, first
  notes.push_back(rg_utils::MakeRouteNote("A2", 100, 200));  // Location A, second (gets A1)
  notes.push_back(rg_utils::MakeRouteNote("B2", 300, 400));  // Location B, second (gets B1)

  for (auto& note : notes) {
    EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Previous write never completed";
    EXPECT_TRUE(reactor->SendRequest(std::move(note)));
  }
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Last write never completed";

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  reactor->CloseRequestStream();

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RouteChatResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  ASSERT_EQ(received_count.load(), 2);  // A2 gets A1, B2 gets B1

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
/// - Send notes to build up history
/// - Call CloseRequestStream()
/// - Continue receiving any queued responses
TEST_F(ActiveBidiReactorTest, RouteChat_ClientClosesFirst_ServerContinues) {
  std::promise<RouteChatResult> result_promise;
  std::future<RouteChatResult> result_future = result_promise.get_future();

  std::vector<routeguide::RouteNote> received_notes;
  std::mutex notes_mutex;

  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time

  PerMessageCallbacks cbs;
  cbs.read_ok = [&received_notes, &notes_mutex](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    const routeguide::RouteNote& note) {
    const std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(note);
    return false;
  };
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

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send 2 notes to same location
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5)));
  EXPECT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("Note 1", 100, 200)));
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Note 1 write never completed";
  EXPECT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("Note 2", 100, 200)));
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Note 2 write never completed";

  // Give server time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Close client stream
  reactor->CloseRequestStream();

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RouteChatResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  // Note 2 should receive Note 1 as response
  EXPECT_GE(result.received_notes.size(), 1);
}

/// @test Validates server closes first, client continues.
///
/// Tests that when server stops accepting messages:
/// - Server configured to close after N messages
/// - read_nok fires when server done reading
/// - OnDone fires with appropriate status
TEST_F(ActiveBidiReactorTest, RouteChat_ServerClosesFirst_ClientContinues) {
  // Configure server to close after 2 messages
  test_service_.SetMaxMessages(2);

  std::promise<RouteChatResult> result_promise;
  std::future<RouteChatResult> result_future = result_promise.get_future();

  std::atomic<bool> read_nok_fired{false};
  std::vector<routeguide::RouteNote> received_notes;
  std::mutex notes_mutex;

  PerMessageCallbacks cbs;
  cbs.read_ok = [&received_notes, &notes_mutex](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    const routeguide::RouteNote& note) {
    const std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(note);
    return false;
  };
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

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send messages - server will close after 2
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 1", 100, 200));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 2", 100, 200));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // This might fail since server closed
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 3", 100, 200));

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

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
TEST_F(ActiveBidiReactorTest, RouteChat_TryCancel_TriggersOnDone) {
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  PerMessageCallbacks cbs;
  cbs.read_ok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                   const routeguide::RouteNote&) {
    return false;
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                      bool) {};
  cbs.done = [&done_promise](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send a note then cancel
  reactor->SendRequest(rg_utils::MakeRouteNote("Note before cancel", 100, 200));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  reactor->TryCancel();

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

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
TEST_F(ActiveBidiReactorTest, RouteChat_DeadlineExceeded_PropagatesStatus) {
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  PerMessageCallbacks cbs;
  cbs.read_ok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                   const routeguide::RouteNote&) {
    // Slow down to help trigger deadline
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    return false;
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                      bool) {};
  cbs.done = [&done_promise](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    done_promise.set_value(status);
  };

  // Very short deadline
  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContextWithDeadline(std::chrono::milliseconds(50)), std::move(cbs));

  // Send notes to trigger activity
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 1", 100, 200));
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 2", 100, 200));
  reactor->SendRequest(rg_utils::MakeRouteNote("Note 3", 100, 200));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

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
/// - The last write_done closes its stream
/// - Wait for all OnDone callbacks
/// - Verify all complete successfully
TEST_F(ActiveBidiReactorTest, RouteChat_MultipleConcurrent_AllComplete) {
  constexpr int kNumStreams = 3;
  constexpr int kNotesPerStream = 2;

  struct Stream {
    std::unique_ptr<PerMessageReactor> reactor;
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
    PerMessageCallbacks cbs;
    cbs.read_ok = [&stream](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                            const routeguide::RouteNote&) {
      stream.received++;
      return false;
    };
    cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
    // Issued from inside the write_done reaction, where OnWriteDone() has already cleared the
    // pending write, so no write gate is shared with the test thread.
    cbs.write_done = [&stream, &next_note, i](
                         grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                         bool ok) {
      EXPECT_TRUE(ok) << "Stream " << i << " write failed";
      const int note = stream.sent++;
      if (ok && note < kNotesPerStream) {
        EXPECT_TRUE(stream.reactor->SendRequest(next_note(i, note)));
      } else {
        stream.reactor->CloseRequestStream();
      }
    };
    cbs.done = [&stream](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                         const grpc::Status& status) {
      RouteChatResult result;
      result.status = status;
      result.completed = true;
      stream.done.set_value(std::move(result));
    };

    stream.reactor = std::make_unique<PerMessageReactor>(
        *stub_, CreateClientContext(), std::move(cbs));
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
    EXPECT_GE(streams[i].received.load(), 1) << "Stream " << i << " should receive at least 1 response";
  }
}

// =============================================================================
// ReadPacing::kPerMessage specific tests: the callback's runtime hold decision
// =============================================================================

/// @test Validates a late decline cannot release the read hold of the message that follows it.
///
/// The bidirectional counterpart of the ActiveReadReactor regression test of the same name. Under kPull
/// the stolen claim would also re-arm a read into the storage still lent to message 1's callback. Three notes
/// at one location draw three echoes: one after the second note, and two after the third. The callbacks
/// of the first two echoes are ordered explicitly:
///
/// 1. Message 0's consumer thread calls GetResponse(), which arms the read of message 1
/// 2. Message 0's callback waits until message 1's callback has started, then returns false
/// 3. Message 1's callback waits until message 0's has returned, then keeps its message by returning true
/// 4. The test thread checks the read side is held, then pulls message 1 with GetResponse()
///
/// Verifies that:
/// - Message 1 stays held after message 0's decline: OnDone stays pending and no further message arrives
/// - The application's GetResponse() for message 1 is accepted and hands out message 1
/// - All 3 echoes arrive and the RPC completes with OK
TEST_F(ActiveBidiReactorTest, RouteChat_PerMessage_DeclinedAfterGetResponse_LeavesNextHoldAlone) {
  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  bool first_returning = false;
  bool second_kept = false;
  std::atomic<bool> first_pulled{false};
  std::binary_semaphore write_slot{1};  // Only one write may be in flight at a time
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  PerMessageCallbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received, &first_returning, &second_kept, &first_pulled](
                    BidiReactorBase* reactor, const routeguide::RouteNote&) {
    auto* client_reactor = static_cast<PerMessageReactor*>(reactor);
    std::unique_lock<std::mutex> lock(mutex);
    const int index = received++;
    cv.notify_all();
    if (index == 0) {
      lock.unlock();
      std::thread consumer([client_reactor, &first_pulled] {
        routeguide::RouteNote note;
        first_pulled = client_reactor->GetResponse(note) && note.message() == "First note";
      });
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

  auto reactor = std::make_unique<PerMessageReactor>(*stub_, CreateClientContext(), std::move(cbs));

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
    const std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(received, 2) << "Message 0's decline released message 1's hold, and the read side read on";
  }
  EXPECT_TRUE(first_pulled) << "Message 0's own GetResponse() was rejected or handed out the wrong message";
  routeguide::RouteNote second;
  EXPECT_TRUE(reactor->GetResponse(second)) << "Message 0's decline released message 1's hold";
  EXPECT_EQ(second.message(), "First note");

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, 3);
}

}  // namespace
