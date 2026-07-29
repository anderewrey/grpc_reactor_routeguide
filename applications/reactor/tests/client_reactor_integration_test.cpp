///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///
///
/// Client Reactor Integration Tests
///
/// Tests the full integration path: reactor → gRPC → EventLoop → application thread.
/// Validates the production usage pattern where gRPC callbacks trigger EventLoop events
/// that dispatch response processing to the application thread.
///
/// The test fixture creates:
/// - An in-process gRPC server with controllable responses (TestRouteGuideService)
/// - A real EventLoop running in NON_BLOCK mode (background thread)
/// - Client reactors that dispatch via EventLoop::TriggerEvent()
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <Event.h>
#include <EventLoop.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace {

/// Each case below names its Delivery explicitly: this fixture holds both approaches side by side.
using RpcReactor::Client::Delivery;

/// Global test environment to manage EventLoop lifecycle.
/// EventLoop doesn't support restart after Halt(), so we start it once for all tests.
class EventLoopEnvironment : public ::testing::Environment {
 public:
  void SetUp() override {
    EventLoop::SetMode(EventLoop::Mode::NON_BLOCK);
    EventLoop::Run();
  }

  void TearDown() override {
    EventLoop::Halt();
  }
};

/// Controllable test service - returns preconfigured responses
class TestRouteGuideService final : public routeguide::RouteGuide::CallbackService {
 public:
  void SetGetFeatureResponse(const routeguide::Feature& feature) {
    configured_feature_ = feature;
  }

  void SetListFeaturesResponse(const std::vector<routeguide::Feature>& features) {
    configured_features_ = features;
  }

  grpc::ServerUnaryReactor* GetFeature(grpc::CallbackServerContext* context,
                                       const routeguide::Point* /*point*/,
                                       routeguide::Feature* feature) override {
    *feature = configured_feature_;
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerWriteReactor<routeguide::Feature>* ListFeatures(
      grpc::CallbackServerContext* /*context*/,
      const routeguide::Rectangle* /*request*/) override {
    class ListFeaturesReactor : public grpc::ServerWriteReactor<routeguide::Feature> {
     public:
      explicit ListFeaturesReactor(std::vector<routeguide::Feature> features)
          : features_(std::move(features)), index_(0) {
        NextWrite();
      }

      void OnWriteDone(bool ok) override {
        if (ok) {
          NextWrite();
        } else {
          Finish(grpc::Status(grpc::StatusCode::UNKNOWN, "Write failed"));
        }
      }

      void OnDone() override { delete this; }

     private:
      void NextWrite() {
        if (index_ < features_.size()) {
          StartWrite(&features_[index_++]);
        } else {
          Finish(grpc::Status::OK);
        }
      }

      std::vector<routeguide::Feature> features_;
      size_t index_;
    };

    return new ListFeaturesReactor(configured_features_);
  }

 private:
  routeguide::Feature configured_feature_;
  std::vector<routeguide::Feature> configured_features_;
};

/// Test fixture with in-process server and EventLoop integration
class ClientReactorIntegrationTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {
 protected:
  void SetUp() override {
    RouteGuideTestFixtureBase::SetUp();
    // Store main thread ID for assertions
    main_thread_id_ = std::this_thread::get_id();
    // EventLoop is managed by EventLoopEnvironment (started once for all tests)
  }

  /// Waits for `done`, set by the test's final EventLoop handler. On a timeout, cancels the RPC and
  /// waits again before reporting the failure: the handlers reference the test's locals, so the test
  /// must not return while the RPC can still dispatch them. Aborts if the cancelled RPC never ends.
  template <class ReactorT>
  static ::testing::AssertionResult WaitForDone(const std::atomic<bool>& done, const ReactorT& reactor,
                                                const char* what) {
    if (WaitFor(done)) {
      return ::testing::AssertionSuccess();
    }
    reactor.TryCancel();
    if (!WaitFor(done)) {
      std::fprintf(stderr, "A cancelled RPC never completed, aborting\n");
      std::abort();
    }
    return ::testing::AssertionFailure() << "Timeout waiting for " << what;
  }

  std::thread::id main_thread_id_;

 private:
  static bool WaitFor(const std::atomic<bool>& done) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done) {
      if (std::chrono::steady_clock::now() > deadline) {
        return false;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return true;
  }
};

// =============================================================================
// Delivery::kPull: the event carries the reactor, and the handler pulls the response out of it
// =============================================================================

/// @test Delivery::kPull: validates unary RPC with EventLoop dispatch.
///
/// Verifies the full production flow:
/// 1. gRPC callback (OnDone) executes on gRPC thread pool
/// 2. Callback triggers EventLoop::TriggerEvent()
/// 3. EventLoop handler executes on EventLoop background thread
/// 4. Response data is correctly extracted via GetResponse()
///
/// Thread assertions confirm callbacks do NOT run on the main thread.
TEST_F(ClientReactorIntegrationTest, GetFeature_Pull_ValidPoint_DispatchesToEventLoop) {
  // Configure expected response
  routeguide::Feature expected_feature;
  expected_feature.set_name("Test Feature");
  expected_feature.mutable_location()->set_latitude(123456789);
  expected_feature.mutable_location()->set_longitude(-987654321);
  test_service_.SetGetFeatureResponse(expected_feature);

  // Test state
  std::atomic<bool> done{false};
  routeguide::Feature received_feature;
  grpc::Status received_status;
  routeguide::GetFeature::ClientReactor<Delivery::kPull>* dispatched_reactor = nullptr;
  std::unique_ptr<routeguide::GetFeature::ClientReactor<Delivery::kPull>> reactor;

  // Register event handler (Servant role in Active Object pattern)
  // In NON_BLOCK mode, EventLoop runs in a background thread
  static constexpr auto kTestOnDone = "TestPullGetFeatureOnDone";
  RpcReactor::EventConnection on_done_guard(kTestOnDone, [&](const EventLoop::Event* event) {
    // In NON_BLOCK mode, this runs on EventLoop's background thread (not main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    auto* r = static_cast<routeguide::GetFeature::ClientReactor<Delivery::kPull>*>(event->getData());
    // Compared on the main thread once done is set: the reactor's constructor already starts the
    // RPC, so this handler can run while the main thread is still assigning reactor.
    dispatched_reactor = r;

    received_status = r->Status();
    if (received_status.ok()) {
      r->GetResponse(received_feature);
    }
    done = true;
  });

  // Create request
  const routeguide::Point request = rg_utils::MakePoint(123456789, -987654321);

  // Create callbacks (triggered on gRPC thread)
  routeguide::GetFeature::Callbacks<Delivery::kPull> cbs;
  cbs.done = [&main_thread_id = main_thread_id_](grpc::ClientUnaryReactor* r, const grpc::Status&,
                                                  const routeguide::Feature&) {
    // Verify we're on gRPC thread (NOT main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    EventLoop::TriggerEvent(kTestOnDone, r);
  };

  // Create reactor (Method Request in Active Object pattern)
  reactor = std::make_unique<routeguide::GetFeature::ClientReactor<Delivery::kPull>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion - EventLoop runs in background thread (NON_BLOCK mode)
  ASSERT_TRUE(WaitForDone(done, *reactor, "RPC completion"));

  // Verify results
  EXPECT_EQ(dispatched_reactor, reactor.get());
  EXPECT_TRUE(received_status.ok()) << "Status: " << received_status.error_message();
  EXPECT_EQ(received_feature.name(), expected_feature.name());
  EXPECT_EQ(received_feature.location().latitude(), expected_feature.location().latitude());
  EXPECT_EQ(received_feature.location().longitude(), expected_feature.location().longitude());
}

// =============================================================================
// ListFeatures Streaming Tests with EventLoop
// =============================================================================

/// @test Delivery::kPull: validates server streaming RPC with EventLoop dispatch.
///
/// Tests the hold/resume pattern for streaming responses:
/// 1. Server sends multiple features via stream
/// 2. Each `OnReadDone(true)` callback fires on gRPC thread
/// 3. Callback triggers EventLoop event (holds reactor for deferred processing)
/// 4. EventLoop handler calls `GetResponse()` to extract feature data
/// 5. After stream ends, `OnDone` dispatches final status via EventLoop
///
/// The test validates:
/// - All streamed responses are received and dispatched correctly
/// - Thread assertions confirm gRPC → EventLoop thread transition
/// - Response data integrity across thread boundaries
TEST_F(ClientReactorIntegrationTest, ListFeatures_Pull_MultipleResponses_DispatchesToEventLoop) {
  // Configure server to return multiple features
  std::vector<routeguide::Feature> expected_features;
  expected_features.reserve(3);
  for (int i = 0; i < 3; ++i) {
    routeguide::Feature feature;
    feature.set_name("Feature " + std::to_string(i));
    feature.mutable_location()->set_latitude(i * 100);
    feature.mutable_location()->set_longitude(i * -100);
    expected_features.push_back(feature);
  }
  test_service_.SetListFeaturesResponse(expected_features);

  // Test state
  std::atomic<bool> done{false};
  std::vector<routeguide::Feature> received_features;
  grpc::Status received_status;
  std::unique_ptr<routeguide::ListFeatures::ClientReactor<Delivery::kPull>> reactor;

  // Register event handlers for streaming
  static constexpr auto kTestOnReadOk = "TestPullListFeaturesOnReadOk";
  static constexpr auto kTestOnDone = "TestPullListFeaturesOnDone";

  const RpcReactor::EventConnection on_read_ok_guard(kTestOnReadOk, [&](const EventLoop::Event* event) {
    // Verify we're on EventLoop thread (not main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    auto* r = static_cast<routeguide::ListFeatures::ClientReactor<Delivery::kPull>*>(event->getData());
    routeguide::Feature feature;
    r->GetResponse(feature);
    received_features.push_back(feature);
  });

  const RpcReactor::EventConnection on_done_guard(kTestOnDone, [&](const EventLoop::Event* event) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    auto* r = static_cast<routeguide::ListFeatures::ClientReactor<Delivery::kPull>*>(event->getData());
    received_status = r->Status();
    done = true;
  });

  // Create request
  const routeguide::Rectangle request;

  // Create callbacks
  routeguide::ListFeatures::Callbacks<Delivery::kPull> cbs;
  cbs.read_ok = [&main_thread_id = main_thread_id_](grpc::ClientReadReactor<routeguide::Feature>* r,
                                                const routeguide::Feature&) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    EventLoop::TriggerEvent(kTestOnReadOk, r);
    return true;  // Hold for GetResponse via EventLoop
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&main_thread_id = main_thread_id_](grpc::ClientReadReactor<routeguide::Feature>* r,
                                                  const grpc::Status&) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    EventLoop::TriggerEvent(kTestOnDone, r);
  };

  // Create reactor
  reactor = std::make_unique<routeguide::ListFeatures::ClientReactor<Delivery::kPull>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion
  ASSERT_TRUE(WaitForDone(done, *reactor, "stream completion"));

  // Verify results
  EXPECT_TRUE(received_status.ok()) << "Status: " << received_status.error_message();
  ASSERT_EQ(received_features.size(), expected_features.size());
  for (size_t i = 0; i < expected_features.size(); ++i) {
    EXPECT_EQ(received_features[i].name(), expected_features[i].name());
  }
}

/// @test Delivery::kPull: validates cancellation triggers EventLoop dispatch.
///
/// Verifies that `TryCancel()` correctly terminates an RPC and still dispatches
/// the final status through the EventLoop:
/// 1. RPC is initiated normally
/// 2. `TryCancel()` is called immediately (before response arrives)
/// 3. gRPC signals cancellation via `OnDone` callback on gRPC thread
/// 4. Callback triggers EventLoop event for final status processing
/// 5. EventLoop handler receives CANCELLED or OK status
///
/// The test accepts both CANCELLED and OK as valid outcomes because:
/// - CANCELLED: Cancel signal arrived before server processed request
/// - OK: Server response arrived before cancel signal took effect
///
/// Thread assertions confirm the EventLoop dispatch path is exercised.
TEST_F(ClientReactorIntegrationTest, GetFeature_Pull_TryCancel_DispatchesToEventLoop) {
  routeguide::Feature feature;
  feature.set_name("Should not receive");
  test_service_.SetGetFeatureResponse(feature);

  std::atomic<bool> done{false};
  grpc::Status received_status;
  std::unique_ptr<routeguide::GetFeature::ClientReactor<Delivery::kPull>> reactor;

  static constexpr auto kTestOnDone = "TestPullCancelOnDone";
  RpcReactor::EventConnection on_done_guard(kTestOnDone, [&](const EventLoop::Event* event) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);
    auto* r = static_cast<routeguide::GetFeature::ClientReactor<Delivery::kPull>*>(event->getData());
    received_status = r->Status();
    done = true;
  });

  const routeguide::Point request = rg_utils::MakePoint(123, 0);

  routeguide::GetFeature::Callbacks<Delivery::kPull> cbs;
  cbs.done = [](grpc::ClientUnaryReactor* r, const grpc::Status&, const routeguide::Feature&) {
    EventLoop::TriggerEvent(kTestOnDone, r);
  };

  reactor = std::make_unique<routeguide::GetFeature::ClientReactor<Delivery::kPull>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Cancel immediately
  reactor->TryCancel();

  ASSERT_TRUE(WaitForDone(done, *reactor, "cancel completion"));

  // Cancel or OK are both valid (depending on timing)
  EXPECT_TRUE(received_status.error_code() == grpc::StatusCode::CANCELLED ||
              received_status.error_code() == grpc::StatusCode::OK);
}

// =============================================================================
// Delivery::kPush: the event carries the owned message, and the handler reclaims it
// =============================================================================

/// @test Delivery::kPush: validates unary RPC with EventLoop dispatch.
///
/// Verifies the full production flow:
/// 1. gRPC callback (OnDone) executes on gRPC thread pool
/// 2. Callback triggers EventLoop::TriggerEvent()
/// 3. EventLoop handler executes on EventLoop background thread
/// 4. The handler reclaims the response ownership the callback released into the queue
///
/// Thread assertions confirm callbacks do NOT run on the main thread.
TEST_F(ClientReactorIntegrationTest, GetFeature_Push_ValidPoint_DispatchesToEventLoop) {
  // Configure expected response
  routeguide::Feature expected_feature;
  expected_feature.set_name("Test Feature");
  expected_feature.mutable_location()->set_latitude(123456789);
  expected_feature.mutable_location()->set_longitude(-987654321);
  test_service_.SetGetFeatureResponse(expected_feature);

  // Test state
  std::atomic<bool> done{false};
  routeguide::Feature received_feature;
  grpc::Status received_status;
  std::unique_ptr<routeguide::GetFeature::ClientReactor<Delivery::kPush>> reactor;

  // Register event handler (Servant role in Active Object pattern)
  // In NON_BLOCK mode, EventLoop runs in a background thread
  static constexpr auto kTestOnDone = "TestPushGetFeatureOnDone";
  RpcReactor::EventConnection on_done_guard(kTestOnDone, [&](const EventLoop::Event* event) {
    // Reclaim first, so no assertion below can leak the response
    const std::unique_ptr<routeguide::Feature> feature{static_cast<routeguide::Feature*>(event->getData())};

    // In NON_BLOCK mode, this runs on EventLoop's background thread (not main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    // Status() is read by the main thread after `done`, not here: this handler can run before the
    // constructing statement below has assigned `reactor`, since StartCall() happens inside it.
    received_feature = std::move(*feature);
    done = true;
  });

  // Create request
  const routeguide::Point request = rg_utils::MakePoint(123456789, -987654321);

  // Create callbacks (triggered on gRPC thread)
  routeguide::GetFeature::Callbacks<Delivery::kPush> cbs;
  cbs.done = [&main_thread_id = main_thread_id_](grpc::ClientUnaryReactor*, const grpc::Status&,
                                                 std::unique_ptr<routeguide::Feature> response) {
    // Verify we're on gRPC thread (NOT main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    EventLoop::TriggerEvent(kTestOnDone, response.release());
  };

  // Create reactor (Method Request in Active Object pattern)
  reactor = std::make_unique<routeguide::GetFeature::ClientReactor<Delivery::kPush>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion - EventLoop runs in background thread (NON_BLOCK mode)
  ASSERT_TRUE(WaitForDone(done, *reactor, "RPC completion"));

  // Verify results. `done` orders this read after the handler, and the reactor is assigned by now.
  received_status = reactor->Status();
  EXPECT_TRUE(received_status.ok()) << "Status: " << received_status.error_message();
  EXPECT_EQ(received_feature.name(), expected_feature.name());
  EXPECT_EQ(received_feature.location().latitude(), expected_feature.location().latitude());
  EXPECT_EQ(received_feature.location().longitude(), expected_feature.location().longitude());
}

// =============================================================================
// ListFeatures Streaming Tests with EventLoop
// =============================================================================

/// @test Delivery::kPush: validates server streaming RPC with EventLoop dispatch.
///
/// Tests the ownership-transfer pattern for streaming responses:
/// 1. Server sends multiple features via stream
/// 2. Each `OnReadDone(true)` callback releases its owned message into the EventLoop queue
/// 3. The `EventLoop` handler reclaims that pointer and owns the message for its own scope
/// 4. After stream ends, `OnDone` dispatches final status via EventLoop
///
/// The test validates:
/// - All streamed responses are received and dispatched correctly
/// - Thread assertions confirm gRPC → EventLoop thread transition
/// - Response data integrity across thread boundaries
/// - Network-order delivery, and `OnDone` handled behind every message, since both ride one queue
TEST_F(ClientReactorIntegrationTest, ListFeatures_Push_MultipleResponses_DispatchesToEventLoop) {
  // Configure server to return multiple features
  std::vector<routeguide::Feature> expected_features;
  expected_features.reserve(3);
  for (int i = 0; i < 3; ++i) {
    routeguide::Feature feature;
    feature.set_name("Feature " + std::to_string(i));
    feature.mutable_location()->set_latitude(i * 100);
    feature.mutable_location()->set_longitude(i * -100);
    expected_features.push_back(feature);
  }
  test_service_.SetListFeaturesResponse(expected_features);

  // Test state
  std::atomic<bool> done{false};
  std::vector<routeguide::Feature> received_features;
  size_t features_at_done = 0;
  grpc::Status received_status;
  std::unique_ptr<routeguide::ListFeatures::ClientReactor<Delivery::kPush>> reactor;

  // Register event handlers for streaming
  static constexpr auto kTestOnReadOk = "TestPushListFeaturesOnReadOk";
  static constexpr auto kTestOnDone = "TestPushListFeaturesOnDone";

  const RpcReactor::EventConnection on_read_ok_guard(kTestOnReadOk, [&](const EventLoop::Event* event) {
    // Verify we're on EventLoop thread (not main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    // Reclaim before the assertions below, so a failure cannot leak the message
    const std::unique_ptr<routeguide::Feature> feature{static_cast<routeguide::Feature*>(event->getData())};
    received_features.push_back(*feature);
  });

  const RpcReactor::EventConnection on_done_guard(kTestOnDone, [&](const EventLoop::Event* event) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    auto* r = static_cast<routeguide::ListFeatures::ClientReactor<Delivery::kPush>*>(event->getData());
    received_status = r->Status();
    // Every message event was enqueued before this one, so all of them are already handled.
    features_at_done = received_features.size();
    done = true;
  });

  // Create request
  const routeguide::Rectangle request;

  // Create callbacks
  routeguide::ListFeatures::Callbacks<Delivery::kPush> cbs;
  cbs.read_ok = [&main_thread_id = main_thread_id_](grpc::ClientReadReactor<routeguide::Feature>*,
                                               std::unique_ptr<routeguide::Feature> response) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    // Hand the message ownership to the queue; the reactor re-arms its read right after this.
    EventLoop::TriggerEvent(kTestOnReadOk, response.release());
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&main_thread_id = main_thread_id_](grpc::ClientReadReactor<routeguide::Feature>* r,
                                                  const grpc::Status&) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    EventLoop::TriggerEvent(kTestOnDone, r);
  };

  // Create reactor
  reactor = std::make_unique<routeguide::ListFeatures::ClientReactor<Delivery::kPush>>(
      *stub_, CreateClientContext(), request, std::move(cbs), RpcReactor::Client::ReadPacing::kContinuous);

  // Wait for completion
  ASSERT_TRUE(WaitForDone(done, *reactor, "stream completion"));

  // Verify results
  EXPECT_TRUE(received_status.ok()) << "Status: " << received_status.error_message();
  ASSERT_EQ(received_features.size(), expected_features.size());
  // In network order: the queue is FIFO and the reactor enqueues one message per reaction.
  for (size_t i = 0; i < expected_features.size(); ++i) {
    EXPECT_EQ(received_features[i].name(), expected_features[i].name()) << "Message " << i << " arrived out of order";
  }
  // The done event rode the same queue behind every message, so none was still pending when it ran.
  EXPECT_EQ(features_at_done, expected_features.size())
      << "OnDone was handled before all messages had been dispatched";
}

/// @test Delivery::kPush: validates cancellation triggers EventLoop dispatch.
///
/// Verifies that `TryCancel()` correctly terminates an RPC and still dispatches
/// the final status through the EventLoop:
/// 1. RPC is initiated normally
/// 2. `TryCancel()` is called immediately (before response arrives)
/// 3. gRPC signals cancellation via `OnDone` callback on gRPC thread
/// 4. Callback triggers EventLoop event for final status processing
/// 5. EventLoop handler receives CANCELLED or OK status
///
/// The test accepts both CANCELLED and OK as valid outcomes because:
/// - CANCELLED: Cancel signal arrived before server processed request
/// - OK: Server response arrived before cancel signal took effect
///
/// Thread assertions confirm the EventLoop dispatch path is exercised.
TEST_F(ClientReactorIntegrationTest, GetFeature_Push_TryCancel_DispatchesToEventLoop) {
  routeguide::Feature feature;
  feature.set_name("Should not receive");
  test_service_.SetGetFeatureResponse(feature);

  std::atomic<bool> done{false};
  grpc::Status received_status;
  std::unique_ptr<routeguide::GetFeature::ClientReactor<Delivery::kPush>> reactor;

  static constexpr auto kTestOnDone = "TestPushCancelOnDone";
  RpcReactor::EventConnection on_done_guard(kTestOnDone, [&](const EventLoop::Event* event) {
    // Reclaim the response even on the cancelled path, where its content is not used
    const std::unique_ptr<routeguide::Feature> discarded{static_cast<routeguide::Feature*>(event->getData())};
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);
    // Status() is read by the main thread after `done`, for the reason given in the test above
    done = true;
  });

  const routeguide::Point request = rg_utils::MakePoint(123, 0);

  routeguide::GetFeature::Callbacks<Delivery::kPush> cbs;
  cbs.done = [](grpc::ClientUnaryReactor*, const grpc::Status&, std::unique_ptr<routeguide::Feature> response) {
    EventLoop::TriggerEvent(kTestOnDone, response.release());
  };

  reactor = std::make_unique<routeguide::GetFeature::ClientReactor<Delivery::kPush>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Cancel immediately
  reactor->TryCancel();

  ASSERT_TRUE(WaitForDone(done, *reactor, "cancel completion"));

  // Cancel or OK are both valid (depending on timing)
  received_status = reactor->Status();
  EXPECT_TRUE(received_status.error_code() == grpc::StatusCode::CANCELLED ||
              received_status.error_code() == grpc::StatusCode::OK);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  // Register global environment to manage EventLoop lifecycle (start once, stop once)
  ::testing::AddGlobalTestEnvironment(new EventLoopEnvironment());
  return RUN_ALL_TESTS();
}
