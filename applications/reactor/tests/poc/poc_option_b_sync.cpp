///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///
/// POC Option B: Synchronous Callbacks (No EventLoop)
///
/// Tests reactor logic without EventLoop dependency. Uses std::promise/future for
/// synchronization, enabling simpler test setup and broader scenario coverage.
///
/// The test fixture creates:
/// - An in-process gRPC server with controllable responses (TestRouteGuideService)
/// - Client reactors with callbacks that complete promises directly
/// - No EventLoop - synchronization via std::future::wait_for()
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
#include <future>
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

  void SetErrorResponse(grpc::StatusCode code, const std::string& message) {
    error_code_ = code;
    error_message_ = message;
    return_error_ = true;
  }

  void ClearErrorResponse() {
    return_error_ = false;
  }

  void SetListFeaturesResponse(const std::vector<routeguide::Feature>& features) {
    configured_features_ = features;
    list_features_error_after_ = -1;  // No error
  }

  void SetListFeaturesErrorAfter(int count, grpc::StatusCode code, const std::string& message) {
    list_features_error_after_ = count;
    list_features_error_code_ = code;
    list_features_error_message_ = message;
  }

  grpc::ServerUnaryReactor* GetFeature(grpc::CallbackServerContext* context,
                                       const routeguide::Point* point,
                                       routeguide::Feature* feature) override {
    auto* reactor = context->DefaultReactor();
    if (return_error_) {
      reactor->Finish(grpc::Status(error_code_, error_message_));
    } else {
      *feature = configured_feature_;
      reactor->Finish(grpc::Status::OK);
    }
    return reactor;
  }

  grpc::ServerWriteReactor<routeguide::Feature>* ListFeatures(
      grpc::CallbackServerContext* context,
      const routeguide::Rectangle* request) override {
    class ListFeaturesReactor : public grpc::ServerWriteReactor<routeguide::Feature> {
     public:
      ListFeaturesReactor(std::vector<routeguide::Feature> features,
                          int error_after, grpc::StatusCode error_code,
                          const std::string& error_message)
          : features_(std::move(features)), index_(0),
            error_after_(error_after), error_code_(error_code),
            error_message_(error_message) {
        NextWrite();
      }

      void OnWriteDone(bool ok) override {
        if (ok) {
          NextWrite();
        } else {
          Finish(grpc::Status(grpc::StatusCode::UNKNOWN, "Write failed"));
        }
      }

      void OnDone() override {
        delete this;
      }

     private:
      void NextWrite() {
        // Check if we should inject an error
        if (error_after_ >= 0 && static_cast<int>(index_) >= error_after_) {
          Finish(grpc::Status(error_code_, error_message_));
          return;
        }
        if (index_ < features_.size()) {
          StartWrite(&features_[index_++]);
        } else {
          Finish(grpc::Status::OK);
        }
      }

      std::vector<routeguide::Feature> features_;
      size_t index_;
      int error_after_;
      grpc::StatusCode error_code_;
      std::string error_message_;
    };

    return new ListFeaturesReactor(configured_features_, list_features_error_after_,
                                   list_features_error_code_, list_features_error_message_);
  }

 private:
  routeguide::Feature configured_feature_;
  std::vector<routeguide::Feature> configured_features_;
  bool return_error_ = false;
  grpc::StatusCode error_code_ = grpc::StatusCode::OK;
  std::string error_message_;
  int list_features_error_after_ = -1;
  grpc::StatusCode list_features_error_code_ = grpc::StatusCode::OK;
  std::string list_features_error_message_;
};

/// Result container for async test
struct TestResult {
  grpc::Status status;
  routeguide::Feature feature;
  bool completed = false;
};

/// Test fixture with in-process server (no EventLoop)
class ReactorIntegrationTest_OptionB : public ::testing::Test {
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
  }

  std::unique_ptr<grpc::ClientContext> CreateClientContext() {
    return std::make_unique<grpc::ClientContext>();
  }

  TestRouteGuideService test_service_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<routeguide::RouteGuide::Stub> stub_;
};

/// @test Validates successful unary RPC response extraction.
///
/// Tests the basic unary RPC flow with synchronous promise/future pattern:
/// 1. Server is configured with a known response
/// 2. Client sends request with matching coordinates
/// 3. `OnDone` callback fires, completing the promise
/// 4. Response is extracted via `GetResponse()` and verified
///
/// Validates that:
/// - Status is OK for successful responses
/// - Feature name and location match expected values
/// - Response data is correctly transferred through reactor
TEST_F(ReactorIntegrationTest_OptionB, GetFeature_ValidPoint_ReturnsFeature) {
  // Configure expected response
  routeguide::Feature expected_feature;
  expected_feature.set_name("Test Feature");
  expected_feature.mutable_location()->set_latitude(123456789);
  expected_feature.mutable_location()->set_longitude(-987654321);
  test_service_.SetGetFeatureResponse(expected_feature);

  // Use promise/future for synchronization (no EventLoop)
  std::promise<TestResult> result_promise;
  std::future<TestResult> result_future = result_promise.get_future();

  // Create request
  routeguide::Point request;
  request.set_latitude(123456789);
  request.set_longitude(-987654321);

  // Create callbacks that complete the promise directly
  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&result_promise](grpc::ClientUnaryReactor* base_reactor, const grpc::Status& status,
                                const routeguide::Feature& response) {
    TestResult result;
    result.status = status;
    if (status.ok()) {
      // Cast to derived type to access GetResponse()
      auto* reactor = static_cast<routeguide::GetFeature::ClientReactor*>(base_reactor);
      reactor->GetResponse(result.feature);
    }
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  // Create reactor
  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion with timeout
  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Timeout waiting for RPC completion";

  TestResult result = result_future.get();

  // Verify results
  EXPECT_TRUE(result.completed);
  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  EXPECT_EQ(result.feature.name(), expected_feature.name());
  EXPECT_EQ(result.feature.location().latitude(), expected_feature.location().latitude());
  EXPECT_EQ(result.feature.location().longitude(), expected_feature.location().longitude());
}

/// @test Validates unary RPC with empty response (unknown point scenario).
///
/// Tests the edge case where a valid RPC returns an empty feature:
/// 1. Server is configured with an empty Feature (no name, default location)
/// 2. Client requests an unknown point (0, 0)
/// 3. RPC completes successfully with OK status
/// 4. Returned feature has empty name
///
/// This simulates the RouteGuide behavior where requesting a point
/// not in the database returns a Feature with an empty name.
TEST_F(ReactorIntegrationTest_OptionB, GetFeature_UnknownPoint_ReturnsEmptyFeature) {
  // Configure empty response (simulating unknown point)
  routeguide::Feature empty_feature;
  // name is empty, location is default
  test_service_.SetGetFeatureResponse(empty_feature);

  std::promise<TestResult> result_promise;
  std::future<TestResult> result_future = result_promise.get_future();

  routeguide::Point request;
  request.set_latitude(0);
  request.set_longitude(0);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&result_promise](grpc::ClientUnaryReactor* base_reactor, const grpc::Status& status,
                                const routeguide::Feature& response) {
    TestResult result;
    result.status = status;
    if (status.ok()) {
      auto* reactor = static_cast<routeguide::GetFeature::ClientReactor*>(base_reactor);
      reactor->GetResponse(result.feature);
    }
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  TestResult result = result_future.get();

  EXPECT_TRUE(result.status.ok());
  EXPECT_TRUE(result.feature.name().empty());
}

/// @test Validates server error propagation through reactor.
///
/// Tests that server-side errors are correctly propagated to the client:
/// 1. Server is configured to return INTERNAL error with custom message
/// 2. Client sends a valid request
/// 3. `OnDone` callback fires with error status
/// 4. Error code and message match server configuration
///
/// Verifies that:
/// - Status is not OK
/// - Error code is INTERNAL (as configured)
/// - Error message matches "Test error message"
TEST_F(ReactorIntegrationTest_OptionB, GetFeature_ServerError_PropagatesStatus) {
  // Configure server to return error
  test_service_.SetErrorResponse(grpc::StatusCode::INTERNAL, "Test error message");

  std::promise<TestResult> result_promise;
  std::future<TestResult> result_future = result_promise.get_future();

  routeguide::Point request;
  request.set_latitude(123);
  request.set_longitude(456);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&result_promise](grpc::ClientUnaryReactor*, const grpc::Status& status,
                                const routeguide::Feature&) {
    TestResult result;
    result.status = status;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  TestResult result = result_future.get();

  EXPECT_FALSE(result.status.ok());
  EXPECT_EQ(result.status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(result.status.error_message(), "Test error message");
}

// =============================================================================
// ListFeatures Streaming Tests (P0 - High Risk)
// =============================================================================

/// @test Validates server streaming RPC receives all responses.
///
/// Tests the complete streaming flow with multiple responses:
/// 1. Server is configured to send 3 features
/// 2. Client initiates ListFeatures stream
/// 3. Each `OnReadDone(true)` callback fires with a feature
/// 4. Callback returns `false` (no hold - auto-continue pattern)
/// 5. After all features, stream ends with `OnReadDone(false)`
/// 6. `OnDone` fires with OK status
///
/// Verifies that:
/// - All 3 features are received
/// - Each feature's name and location match expected values
/// - Final status is OK
TEST_F(ReactorIntegrationTest_OptionB, ListFeatures_MultipleResponses_ReceivesAll) {
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

  // Collect received features
  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  // Create request (bounding rectangle)
  routeguide::Rectangle request;
  request.mutable_lo()->set_latitude(0);
  request.mutable_lo()->set_longitude(-300);
  request.mutable_hi()->set_latitude(300);
  request.mutable_hi()->set_longitude(0);

  // Create callbacks - use false to let reactor auto-continue reads (simpler pattern)
  routeguide::ListFeatures::Callbacks cbs;
  cbs.ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                 const routeguide::Feature& response) {
    // Copy feature directly from callback argument (already populated by gRPC)
    received_features.push_back(response);
    return false;  // Don't hold - let reactor auto-continue to next read
  };
  cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Stream ended - no more reads
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  // Create reactor
  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion
  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Timeout waiting for stream completion";

  grpc::Status status = done_future.get();

  // Verify results
  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  ASSERT_EQ(received_features.size(), expected_features.size());
  for (size_t i = 0; i < expected_features.size(); ++i) {
    EXPECT_EQ(received_features[i].name(), expected_features[i].name());
    EXPECT_EQ(received_features[i].location().latitude(), expected_features[i].location().latitude());
    EXPECT_EQ(received_features[i].location().longitude(), expected_features[i].location().longitude());
  }
}

/// @test Validates empty server streaming RPC completes successfully.
///
/// Tests the edge case of a stream with zero responses:
/// 1. Server is configured with empty feature list
/// 2. Client initiates ListFeatures stream
/// 3. `OnReadDone(false)` fires immediately (no data to read)
/// 4. `OnDone` fires with OK status
///
/// Verifies that:
/// - `ok` callback is never invoked (no features received)
/// - Final status is OK (empty stream is valid, not an error)
/// - Reactor terminates cleanly without hanging
TEST_F(ReactorIntegrationTest_OptionB, ListFeatures_EmptyStream_CompletesSuccessfully) {
  // Configure server to return empty stream
  test_service_.SetListFeaturesResponse({});

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();
  int read_count = 0;

  routeguide::Rectangle request;
  request.mutable_lo()->set_latitude(0);
  request.mutable_lo()->set_longitude(0);
  request.mutable_hi()->set_latitude(0);
  request.mutable_hi()->set_longitude(0);

  routeguide::ListFeatures::Callbacks cbs;
  cbs.ok = [&read_count](grpc::ClientReadReactor<routeguide::Feature>*, const routeguide::Feature&) {
    ++read_count;  // Should never be called for empty stream
    return false;
  };
  cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Expected: stream ends immediately
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  EXPECT_EQ(read_count, 0) << "Expected no reads for empty stream";
}

// =============================================================================
// P1 Tests - Medium Risk
// =============================================================================

/// @test Validates mid-stream server error propagation.
///
/// Tests partial stream completion followed by server error:
/// 1. Server is configured with 5 features but error after 2
/// 2. Client receives first 2 features successfully
/// 3. Server terminates stream with INTERNAL error
/// 4. `OnDone` fires with error status
///
/// Verifies that:
/// - Exactly 2 features were received before error
/// - Final status is INTERNAL (as configured)
/// - Error message matches "Mid-stream error"
/// - Partial data is preserved despite stream failure
TEST_F(ReactorIntegrationTest_OptionB, ListFeatures_ServerErrorMidStream_PropagatesStatus) {
  // Configure server to return 2 features then error
  std::vector<routeguide::Feature> features;
  for (int i = 0; i < 5; ++i) {
    routeguide::Feature feature;
    feature.set_name("Feature " + std::to_string(i));
    features.push_back(feature);
  }
  test_service_.SetListFeaturesResponse(features);
  test_service_.SetListFeaturesErrorAfter(2, grpc::StatusCode::INTERNAL, "Mid-stream error");

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                 const routeguide::Feature& response) {
    received_features.push_back(response);
    return false;
  };
  cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  // Should have received 2 features before error
  EXPECT_EQ(received_features.size(), 2u);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(status.error_message(), "Mid-stream error");
}

/// @test Validates unary RPC cancellation triggers OnDone.
///
/// Tests that `TryCancel()` correctly terminates a unary RPC:
/// 1. Server is configured with a valid response
/// 2. Client initiates RPC and immediately calls `TryCancel()`
/// 3. gRPC processes cancel signal (best-effort, out-of-band)
/// 4. `OnDone` fires with final status
///
/// The test accepts both CANCELLED and OK as valid outcomes:
/// - CANCELLED: Cancel arrived before server processed request
/// - OK: Response arrived before cancel took effect
///
/// The key assertion is that `OnDone` is called regardless of timing.
TEST_F(ReactorIntegrationTest_OptionB, TryCancel_UnaryRpc_TriggersOnDone) {
  // Configure a valid response (won't be received due to cancel)
  routeguide::Feature feature;
  feature.set_name("Should not receive");
  test_service_.SetGetFeatureResponse(feature);

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Point request;
  request.set_latitude(123);
  request.set_longitude(456);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&done_promise](grpc::ClientUnaryReactor*, const grpc::Status& status,
                              const routeguide::Feature&) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Cancel immediately
  reactor->TryCancel();

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  // Cancel may result in CANCELLED or OK (if response arrived before cancel)
  // The key assertion is that OnDone was called
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::CANCELLED ||
              status.error_code() == grpc::StatusCode::OK)
      << "Expected CANCELLED or OK, got: " << status.error_code();
}

/// @test Validates streaming RPC cancellation triggers OnDone.
///
/// Tests that `TryCancel()` correctly terminates an active stream:
/// 1. Server is configured with 100 features (long stream)
/// 2. Client initiates stream and begins receiving
/// 3. After brief delay, client calls `TryCancel()`
/// 4. gRPC processes cancel signal mid-stream
/// 5. `OnDone` fires with final status
///
/// The test accepts both CANCELLED and OK as valid outcomes:
/// - CANCELLED: Cancel arrived while stream was active
/// - OK: All responses arrived before cancel took effect
///
/// The key assertion is that `OnDone` is called, terminating the reactor cleanly.
TEST_F(ReactorIntegrationTest_OptionB, TryCancel_StreamingRpc_TriggersOnDone) {
  // Configure server to return many features (gives time to cancel)
  std::vector<routeguide::Feature> features;
  for (int i = 0; i < 100; ++i) {
    routeguide::Feature feature;
    feature.set_name("Feature " + std::to_string(i));
    features.push_back(feature);
  }
  test_service_.SetListFeaturesResponse(features);

  std::atomic<int> read_count{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.ok = [&read_count](grpc::ClientReadReactor<routeguide::Feature>*,
                          const routeguide::Feature&) {
    ++read_count;
    return false;
  };
  cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Cancel after a brief moment
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  reactor->TryCancel();

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  // Should have received fewer than all features due to cancellation
  // (or all if cancel came too late - that's also valid)
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::CANCELLED ||
              status.error_code() == grpc::StatusCode::OK)
      << "Expected CANCELLED or OK, got: " << status.error_code();

  // Key assertion: OnDone was triggered (we got here means it worked)
}

// =============================================================================
// P2 Tests - Lower Risk
// =============================================================================

/// @test Validates expired deadline returns DEADLINE_EXCEEDED.
///
/// Tests gRPC deadline enforcement:
/// 1. Client creates context with already-expired deadline (100ms in the past)
/// 2. Client attempts RPC with expired context
/// 3. gRPC immediately rejects the RPC
/// 4. `OnDone` fires with DEADLINE_EXCEEDED status
///
/// This test validates that:
/// - gRPC enforces deadlines before sending requests
/// - DEADLINE_EXCEEDED status is correctly propagated
/// - Reactor terminates cleanly on deadline failure
///
/// Note: Using an already-expired deadline ensures deterministic test behavior.
TEST_F(ReactorIntegrationTest_OptionB, ContextDeadline_Exceeded_PropagatesDeadlineExceeded) {
  // Configure a valid response
  routeguide::Feature feature;
  feature.set_name("Delayed feature");
  test_service_.SetGetFeatureResponse(feature);

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Point request;
  request.set_latitude(123);
  request.set_longitude(456);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&done_promise](grpc::ClientUnaryReactor*, const grpc::Status& status,
                              const routeguide::Feature&) {
    done_promise.set_value(status);
  };

  // Create context with very short deadline (already expired)
  auto context = std::make_unique<grpc::ClientContext>();
  context->set_deadline(std::chrono::system_clock::now() - std::chrono::milliseconds(100));

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, std::move(context), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  // Expired deadline should result in DEADLINE_EXCEEDED
  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED)
      << "Expected DEADLINE_EXCEEDED, got: " << status.error_code()
      << " (" << status.error_message() << ")";
}

/// @test Validates concurrent RPCs all complete successfully.
///
/// Tests reactor thread safety with parallel requests:
/// 1. 10 concurrent GetFeature RPCs are initiated simultaneously
/// 2. Each RPC has its own reactor instance and callbacks
/// 3. gRPC thread pool processes requests in parallel
/// 4. Atomic counter tracks completions
/// 5. Promise completes when all 10 RPCs finish
///
/// Verifies that:
/// - All 10 RPCs complete (no deadlocks or race conditions)
/// - All statuses are OK
/// - Reactor implementation is thread-safe
///
/// This test validates the reactor design for high-concurrency scenarios.
TEST_F(ReactorIntegrationTest_OptionB, MultipleConcurrentGetFeature_AllComplete) {
  // Configure response
  routeguide::Feature feature;
  feature.set_name("Concurrent feature");
  test_service_.SetGetFeatureResponse(feature);

  constexpr int kNumConcurrentRpcs = 10;

  // Track completion of all RPCs
  std::atomic<int> completed_count{0};
  std::promise<void> all_done_promise;
  std::future<void> all_done_future = all_done_promise.get_future();

  std::vector<std::unique_ptr<routeguide::GetFeature::ClientReactor>> reactors;
  std::vector<grpc::Status> statuses(kNumConcurrentRpcs);

  for (int i = 0; i < kNumConcurrentRpcs; ++i) {
    routeguide::Point request;
    request.set_latitude(i * 100);
    request.set_longitude(i * -100);

    routeguide::GetFeature::Callbacks cbs;
    cbs.done = [&completed_count, &all_done_promise, &statuses, i, kNumConcurrentRpcs](
                   grpc::ClientUnaryReactor*, const grpc::Status& status, const routeguide::Feature&) {
      statuses[i] = status;
      if (++completed_count == kNumConcurrentRpcs) {
        all_done_promise.set_value();
      }
    };

    reactors.push_back(std::make_unique<routeguide::GetFeature::ClientReactor>(
        *stub_, CreateClientContext(), request, std::move(cbs)));
  }

  auto wait_result = all_done_future.wait_for(std::chrono::seconds(10));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Timeout waiting for concurrent RPCs";

  // Verify all completed successfully
  EXPECT_EQ(completed_count.load(), kNumConcurrentRpcs);
  for (int i = 0; i < kNumConcurrentRpcs; ++i) {
    EXPECT_TRUE(statuses[i].ok()) << "RPC " << i << " failed: " << statuses[i].error_message();
  }
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
