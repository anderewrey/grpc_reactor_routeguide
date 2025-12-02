///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///
///
/// POC Option A: Real EventLoop + In-Process Server
///
/// Tests the full integration path: reactor → gRPC → EventLoop → application thread.
/// This approach validates the production usage pattern where gRPC callbacks trigger
/// EventLoop events that dispatch response processing to the application thread.
///
/// The test fixture creates:
/// - An in-process gRPC server with controllable responses (TestRouteGuideService)
/// - A real EventLoop running in NON_BLOCK mode (background thread)
/// - Client reactors that dispatch via EventLoop::TriggerEvent()
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

#include <Event.h>
#include <EventLoop.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "applications/reactor/reactor_client_routeguide.h"

namespace {

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
                                       const routeguide::Point* point,
                                       routeguide::Feature* feature) override {
    *feature = configured_feature_;
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }

  grpc::ServerWriteReactor<routeguide::Feature>* ListFeatures(
      grpc::CallbackServerContext* context,
      const routeguide::Rectangle* request) override {
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
class ReactorIntegrationTest_OptionA : public ::testing::Test {
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

    // Store main thread ID for assertions
    main_thread_id_ = std::this_thread::get_id();

    // Start EventLoop in non-blocking mode (runs in background thread)
    EventLoop::SetMode(EventLoop::Mode::NON_BLOCK);
    EventLoop::Run();
  }

  void TearDown() override {
    // Stop EventLoop
    EventLoop::Halt();

    if (server_) {
      server_->Shutdown();
    }
  }

  std::unique_ptr<grpc::ClientContext> CreateClientContext() {
    return std::make_unique<grpc::ClientContext>();
  }

  TestRouteGuideService test_service_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<routeguide::RouteGuide::Stub> stub_;
  std::thread::id main_thread_id_;
};

/// @test Validates unary RPC with EventLoop dispatch.
///
/// Verifies the full production flow:
/// 1. gRPC callback (OnDone) executes on gRPC thread pool
/// 2. Callback triggers EventLoop::TriggerEvent()
/// 3. EventLoop handler executes on EventLoop background thread
/// 4. Response data is correctly extracted via GetResponse()
///
/// Thread assertions confirm callbacks do NOT run on the main thread.
TEST_F(ReactorIntegrationTest_OptionA, GetFeature_ValidPoint_ReturnsFeature) {
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
  std::unique_ptr<routeguide::GetFeature::ClientReactor> reactor;

  // Register event handler (Servant role in Active Object pattern)
  // In NON_BLOCK mode, EventLoop runs in a background thread
  static constexpr auto kTestOnDone = "TestGetFeatureOnDone";
  EventLoop::RegisterEvent(kTestOnDone, [&](const EventLoop::Event* event) {
    // In NON_BLOCK mode, this runs on EventLoop's background thread (not main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    auto* r = static_cast<routeguide::GetFeature::ClientReactor*>(event->getData());
    EXPECT_EQ(r, reactor.get());

    received_status = r->Status();
    if (received_status.ok()) {
      r->GetResponse(received_feature);
    }
    done = true;
  });

  // Create request
  routeguide::Point request;
  request.set_latitude(123456789);
  request.set_longitude(-987654321);

  // Create callbacks (triggered on gRPC thread)
  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&main_thread_id = main_thread_id_](grpc::ClientUnaryReactor* r, const grpc::Status&,
                                                  const routeguide::Feature&) {
    // Verify we're on gRPC thread (NOT main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    EventLoop::TriggerEvent(kTestOnDone, r);
  };

  // Create reactor (Method Request in Active Object pattern)
  reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion - EventLoop runs in background thread (NON_BLOCK mode)
  auto start = std::chrono::steady_clock::now();
  while (!done) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
      FAIL() << "Timeout waiting for RPC completion";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Verify results
  EXPECT_TRUE(received_status.ok()) << "Status: " << received_status.error_message();
  EXPECT_EQ(received_feature.name(), expected_feature.name());
  EXPECT_EQ(received_feature.location().latitude(), expected_feature.location().latitude());
  EXPECT_EQ(received_feature.location().longitude(), expected_feature.location().longitude());
}

// =============================================================================
// ListFeatures Streaming Tests with EventLoop
// =============================================================================

/// @test Validates server streaming RPC with EventLoop dispatch.
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
TEST_F(ReactorIntegrationTest_OptionA, ListFeatures_MultipleResponses_DispatchesToEventLoop) {
  // Configure server to return multiple features
  std::vector<routeguide::Feature> expected_features;
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
  std::unique_ptr<routeguide::ListFeatures::ClientReactor> reactor;

  // Register event handlers for streaming
  static constexpr auto kTestOnReadOk = "TestListFeaturesOnReadOk";
  static constexpr auto kTestOnDone = "TestListFeaturesOnDone";

  EventLoop::RegisterEvent(kTestOnReadOk, [&](const EventLoop::Event* event) {
    // Verify we're on EventLoop thread (not main thread)
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    auto* r = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
    routeguide::Feature feature;
    r->GetResponse(feature);
    received_features.push_back(feature);
  });

  EventLoop::RegisterEvent(kTestOnDone, [&](const EventLoop::Event* event) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);

    auto* r = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
    received_status = r->Status();
    done = true;
  });

  // Create request
  routeguide::Rectangle request;

  // Create callbacks
  routeguide::ListFeatures::Callbacks cbs;
  cbs.ok = [&main_thread_id = main_thread_id_](grpc::ClientReadReactor<routeguide::Feature>* r,
                                                const routeguide::Feature&) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    EventLoop::TriggerEvent(kTestOnReadOk, r);
    return true;  // Hold for GetResponse via EventLoop
  };
  cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&main_thread_id = main_thread_id_](grpc::ClientReadReactor<routeguide::Feature>* r,
                                                  const grpc::Status&) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id);
    EventLoop::TriggerEvent(kTestOnDone, r);
  };

  // Create reactor
  reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion
  auto start = std::chrono::steady_clock::now();
  while (!done) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
      FAIL() << "Timeout waiting for stream completion";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Verify results
  EXPECT_TRUE(received_status.ok()) << "Status: " << received_status.error_message();
  ASSERT_EQ(received_features.size(), expected_features.size());
  for (size_t i = 0; i < expected_features.size(); ++i) {
    EXPECT_EQ(received_features[i].name(), expected_features[i].name());
  }
}

/// @test Validates cancellation triggers EventLoop dispatch.
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
TEST_F(ReactorIntegrationTest_OptionA, TryCancel_UnaryRpc_DispatchesToEventLoop) {
  routeguide::Feature feature;
  feature.set_name("Should not receive");
  test_service_.SetGetFeatureResponse(feature);

  std::atomic<bool> done{false};
  grpc::Status received_status;
  std::unique_ptr<routeguide::GetFeature::ClientReactor> reactor;

  static constexpr auto kTestOnDone = "TestCancelOnDone";
  EventLoop::RegisterEvent(kTestOnDone, [&](const EventLoop::Event* event) {
    EXPECT_NE(std::this_thread::get_id(), main_thread_id_);
    auto* r = static_cast<routeguide::GetFeature::ClientReactor*>(event->getData());
    received_status = r->Status();
    done = true;
  });

  routeguide::Point request;
  request.set_latitude(123);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [](grpc::ClientUnaryReactor* r, const grpc::Status&, const routeguide::Feature&) {
    EventLoop::TriggerEvent(kTestOnDone, r);
  };

  reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Cancel immediately
  reactor->TryCancel();

  auto start = std::chrono::steady_clock::now();
  while (!done) {
    if (std::chrono::steady_clock::now() - start > std::chrono::seconds(5)) {
      FAIL() << "Timeout waiting for cancel completion";
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }

  // Cancel or OK are both valid (depending on timing)
  EXPECT_TRUE(received_status.error_code() == grpc::StatusCode::CANCELLED ||
              received_status.error_code() == grpc::StatusCode::OK);
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
