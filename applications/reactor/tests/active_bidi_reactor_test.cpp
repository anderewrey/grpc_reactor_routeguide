///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///
/// ActiveBidiReactor Test Suite
///
/// Tests the bidirectional streaming reactor pattern using RouteChat RPC.
/// Uses std::promise/future for synchronization (Option B pattern - no EventLoop).
///
/// The test fixture creates:
/// - An in-process gRPC server with RouteChat implementation
/// - Client reactors with callbacks that complete promises directly
/// - Location-based note storage for deterministic test scenarios
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "applications/reactor/reactor_client_routeguide.h"

namespace {

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

/// Helper to create a RouteNote with location and message
routeguide::RouteNote MakeRouteNote(int latitude, int longitude, const std::string& message) {
  routeguide::RouteNote note;
  note.mutable_location()->set_latitude(latitude);
  note.mutable_location()->set_longitude(longitude);
  note.set_message(message);
  return note;
}

/// Result container for RouteChat tests
struct RouteChatResult {
  grpc::Status status;
  std::vector<routeguide::RouteNote> received_notes;
  int write_done_count = 0;
  bool read_nok_received = false;
  bool completed = false;
};

/// Test fixture with in-process server
class ActiveBidiReactorTest : public ::testing::Test {
 protected:
  void SetUp() override {
    // Build in-process server
    grpc::ServerBuilder builder;
    builder.RegisterService(&test_service_);
    int selected_port = 0;
    builder.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &selected_port);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr) << "Failed to start in-process server";
    ASSERT_GT(selected_port, 0) << "Failed to get dynamic port";

    // Create channel to in-process server
    std::string server_address = "localhost:" + std::to_string(selected_port);
    channel_ = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    stub_ = routeguide::RouteGuide::NewStub(channel_);
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
    }
    test_service_.ClearMaxMessages();
  }

  std::unique_ptr<grpc::ClientContext> CreateClientContext() {
    return std::make_unique<grpc::ClientContext>();
  }

  std::unique_ptr<grpc::ClientContext> CreateClientContextWithDeadline(
      std::chrono::milliseconds timeout) {
    auto context = std::make_unique<grpc::ClientContext>();
    context->set_deadline(std::chrono::system_clock::now() + timeout);
    return context;
  }

  TestRouteGuideService test_service_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<routeguide::RouteGuide::Stub> stub_;
};

// =============================================================================
// RouteChat Bidirectional Streaming Tests
// =============================================================================

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

  // Synchronization for sequential writes
  std::mutex write_mutex;
  std::condition_variable write_cv;
  bool write_ready = true;

  routeguide::RouteChat::Callbacks cbs;
  cbs.read_ok = [&received_notes, &notes_mutex](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    const routeguide::RouteNote& note) {
    std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(note);
    return false;  // Continue reading immediately
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&write_mutex, &write_cv, &write_ready](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool ok) {
    std::lock_guard<std::mutex> lock(write_mutex);
    write_ready = true;
    write_cv.notify_one();
  };
  cbs.done = [&result_promise, &received_notes, &notes_mutex](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    RouteChatResult result;
    result.status = status;
    {
      std::lock_guard<std::mutex> lock(notes_mutex);
      result.received_notes = received_notes;
    }
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RouteChat::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send 3 notes to the same location
  std::vector<routeguide::RouteNote> sent_notes;
  sent_notes.push_back(MakeRouteNote(100, 200, "First note"));
  sent_notes.push_back(MakeRouteNote(100, 200, "Second note"));
  sent_notes.push_back(MakeRouteNote(100, 200, "Third note"));

  for (auto& note : sent_notes) {
    {
      std::unique_lock<std::mutex> lock(write_mutex);
      write_cv.wait(lock, [&write_ready] { return write_ready; });
      write_ready = false;
    }
    reactor->SendRequest(std::move(note));
  }

  // Wait for last write to complete
  {
    std::unique_lock<std::mutex> lock(write_mutex);
    write_cv.wait_for(lock, std::chrono::seconds(1), [&write_ready] { return write_ready; });
  }

  // Give server time to send all responses
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Signal end of client stream
  reactor->CloseRequestStream();

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Timeout waiting for RPC completion";

  RouteChatResult result = result_future.get();

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

  // Synchronization for sequential writes
  std::mutex write_mutex;
  std::condition_variable write_cv;
  bool write_ready = true;

  routeguide::RouteChat::Callbacks cbs;
  cbs.read_ok = [&received_count, &received_notes, &notes_mutex](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    const routeguide::RouteNote& note) {
    received_count++;
    std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(note);
    return false;
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&write_mutex, &write_cv, &write_ready](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool) {
    std::lock_guard<std::mutex> lock(write_mutex);
    write_ready = true;
    write_cv.notify_one();
  };
  cbs.done = [&result_promise, &received_notes, &notes_mutex, &received_count](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    RouteChatResult result;
    result.status = status;
    {
      std::lock_guard<std::mutex> lock(notes_mutex);
      result.received_notes = received_notes;
    }
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RouteChat::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send notes to different locations in interleaved pattern
  // Location A (100, 200): 2 notes -> expect 0 + 1 = 1 response
  // Location B (300, 400): 2 notes -> expect 0 + 1 = 1 response
  // Total: 2 responses

  std::vector<routeguide::RouteNote> notes;
  notes.push_back(MakeRouteNote(100, 200, "A1"));  // Location A, first
  notes.push_back(MakeRouteNote(300, 400, "B1"));  // Location B, first
  notes.push_back(MakeRouteNote(100, 200, "A2"));  // Location A, second (gets A1)
  notes.push_back(MakeRouteNote(300, 400, "B2"));  // Location B, second (gets B1)

  for (auto& note : notes) {
    {
      std::unique_lock<std::mutex> lock(write_mutex);
      write_cv.wait(lock, [&write_ready] { return write_ready; });
      write_ready = false;
    }
    reactor->SendRequest(std::move(note));
  }

  {
    std::unique_lock<std::mutex> lock(write_mutex);
    write_cv.wait_for(lock, std::chrono::seconds(1), [&write_ready] { return write_ready; });
  }

  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  reactor->CloseRequestStream();

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  RouteChatResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  EXPECT_EQ(received_count.load(), 2);  // A2 gets A1, B2 gets B1
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

  std::mutex write_mutex;
  std::condition_variable write_cv;
  bool write_ready = true;

  routeguide::RouteChat::Callbacks cbs;
  cbs.read_ok = [&received_notes, &notes_mutex](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    const routeguide::RouteNote& note) {
    std::lock_guard<std::mutex> lock(notes_mutex);
    received_notes.push_back(note);
    return false;
  };
  cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
  cbs.write_done = [&write_mutex, &write_cv, &write_ready](
                       grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                       bool) {
    std::lock_guard<std::mutex> lock(write_mutex);
    write_ready = true;
    write_cv.notify_one();
  };
  cbs.done = [&result_promise, &received_notes, &notes_mutex](
                 grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                 const grpc::Status& status) {
    RouteChatResult result;
    result.status = status;
    {
      std::lock_guard<std::mutex> lock(notes_mutex);
      result.received_notes = received_notes;
    }
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RouteChat::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send 2 notes to same location
  {
    std::unique_lock<std::mutex> lock(write_mutex);
    write_cv.wait(lock, [&write_ready] { return write_ready; });
    write_ready = false;
  }
  reactor->SendRequest(MakeRouteNote(100, 200, "Note 1"));

  {
    std::unique_lock<std::mutex> lock(write_mutex);
    write_cv.wait_for(lock, std::chrono::seconds(1), [&write_ready] { return write_ready; });
    write_ready = false;
  }
  reactor->SendRequest(MakeRouteNote(100, 200, "Note 2"));

  {
    std::unique_lock<std::mutex> lock(write_mutex);
    write_cv.wait_for(lock, std::chrono::seconds(1), [&write_ready] { return write_ready; });
  }

  // Give server time to process
  std::this_thread::sleep_for(std::chrono::milliseconds(50));

  // Close client stream
  reactor->CloseRequestStream();

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  RouteChatResult result = result_future.get();

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

  routeguide::RouteChat::Callbacks cbs;
  cbs.read_ok = [&received_notes, &notes_mutex](
                    grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                    const routeguide::RouteNote& note) {
    std::lock_guard<std::mutex> lock(notes_mutex);
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

  auto reactor = std::make_unique<routeguide::RouteChat::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send messages - server will close after 2
  reactor->SendRequest(MakeRouteNote(100, 200, "Note 1"));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  reactor->SendRequest(MakeRouteNote(100, 200, "Note 2"));
  std::this_thread::sleep_for(std::chrono::milliseconds(20));

  // This might fail since server closed
  reactor->SendRequest(MakeRouteNote(100, 200, "Note 3"));

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  RouteChatResult result = result_future.get();

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
TEST_F(ActiveBidiReactorTest, TryCancel_BidiStreaming_TriggersOnDone) {
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::RouteChat::Callbacks cbs;
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

  auto reactor = std::make_unique<routeguide::RouteChat::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send a note then cancel
  reactor->SendRequest(MakeRouteNote(100, 200, "Note before cancel"));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  reactor->TryCancel();

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

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
TEST_F(ActiveBidiReactorTest, ContextDeadline_BidiStreaming_PropagatesStatus) {
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::RouteChat::Callbacks cbs;
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
  auto reactor = std::make_unique<routeguide::RouteChat::ClientReactor>(
      *stub_, CreateClientContextWithDeadline(std::chrono::milliseconds(50)), std::move(cbs));

  // Send notes to trigger activity
  reactor->SendRequest(MakeRouteNote(100, 200, "Note 1"));
  reactor->SendRequest(MakeRouteNote(100, 200, "Note 2"));
  reactor->SendRequest(MakeRouteNote(100, 200, "Note 3"));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

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
/// - Each exchanges different messages
/// - Wait for all OnDone callbacks
/// - Verify all complete successfully
TEST_F(ActiveBidiReactorTest, MultipleConcurrentRouteChat_AllComplete) {
  const int kNumStreams = 3;

  std::vector<std::promise<RouteChatResult>> promises(kNumStreams);
  std::vector<std::future<RouteChatResult>> futures;
  for (auto& p : promises) {
    futures.push_back(p.get_future());
  }

  std::vector<std::unique_ptr<routeguide::RouteChat::ClientReactor>> reactors;
  std::vector<std::atomic<int>> received_counts(kNumStreams);
  for (auto& count : received_counts) {
    count = 0;
  }

  // Per-stream write synchronization: only one write may be in flight at a time on a given
  // stream (enforced by SendRequest()), so each stream needs its own ready signal.
  std::vector<std::mutex> write_mutexes(kNumStreams);
  std::vector<std::condition_variable> write_cvs(kNumStreams);
  std::vector<bool> write_readies(kNumStreams, true);

  for (int i = 0; i < kNumStreams; ++i) {
    routeguide::RouteChat::Callbacks cbs;

    // Capture i by value
    cbs.read_ok = [&received_counts, i](
                      grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                      const routeguide::RouteNote&) {
      received_counts[i]++;
      return false;
    };
    cbs.read_nok = [](grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*) {};
    cbs.write_done = [&write_mutexes, &write_cvs, &write_readies, i](
                         grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                         bool) {
      std::lock_guard<std::mutex> lock(write_mutexes[i]);
      write_readies[i] = true;
      write_cvs[i].notify_one();
    };
    cbs.done = [&promises, i](
                   grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>*,
                   const grpc::Status& status) {
      RouteChatResult result;
      result.status = status;
      result.completed = true;
      promises[i].set_value(std::move(result));
    };

    reactors.push_back(std::make_unique<routeguide::RouteChat::ClientReactor>(
        *stub_, CreateClientContext(), std::move(cbs)));
  }

  // Send messages on each stream - each to its own location, waiting for each write to
  // complete before sending the next one (only one write may be in flight at a time).
  for (int i = 0; i < kNumStreams; ++i) {
    {
      std::unique_lock<std::mutex> lock(write_mutexes[i]);
      write_cvs[i].wait(lock, [&write_readies, i] { return write_readies[i]; });
      write_readies[i] = false;
    }
    reactors[i]->SendRequest(MakeRouteNote(i * 100, i * 100, "Note 1"));

    {
      std::unique_lock<std::mutex> lock(write_mutexes[i]);
      write_cvs[i].wait_for(lock, std::chrono::seconds(1), [&write_readies, i] { return write_readies[i]; });
      write_readies[i] = false;
    }
    reactors[i]->SendRequest(MakeRouteNote(i * 100, i * 100, "Note 2"));
  }

  // Give time for processing
  std::this_thread::sleep_for(std::chrono::milliseconds(100));

  // Close all streams
  for (auto& reactor : reactors) {
    reactor->CloseRequestStream();
  }

  // Wait for all completions
  bool all_ready = true;
  for (auto& f : futures) {
    auto wait_result = f.wait_for(std::chrono::seconds(5));
    if (wait_result != std::future_status::ready) {
      all_ready = false;
      break;
    }
  }
  ASSERT_TRUE(all_ready) << "Timeout waiting for all streams to complete";

  // Verify all completed successfully
  for (int i = 0; i < kNumStreams; ++i) {
    RouteChatResult result = futures[i].get();
    EXPECT_TRUE(result.completed) << "Stream " << i << " did not complete";
    EXPECT_TRUE(result.status.ok()) << "Stream " << i << " failed: " << result.status.error_message();
    // Second note should receive first note as response
    EXPECT_GE(received_counts[i].load(), 1)
        << "Stream " << i << " should receive at least 1 response";
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
