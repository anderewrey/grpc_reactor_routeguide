///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///
/// ActiveUnaryReactor Test Suite
///
/// Tests the unary RPC reactor pattern using GetFeature RPC.
/// Uses std::promise/future for synchronization.
///
/// The test fixture creates:
/// - An in-process gRPC server with controllable GetFeature responses
/// - Client reactors with callbacks that complete promises directly
/// - Configurable error injection for error path testing
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace {

/// Controllable test service for unary RPC testing
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

  grpc::ServerUnaryReactor* GetFeature(grpc::CallbackServerContext* context,
                                       const routeguide::Point* /*point*/,
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

 private:
  routeguide::Feature configured_feature_;
  bool return_error_ = false;
  grpc::StatusCode error_code_ = grpc::StatusCode::OK;
  std::string error_message_;
};

/// Result container for GetFeature tests
struct GetFeatureResult {
  grpc::Status status;
  routeguide::Feature feature;
  bool completed = false;
};

/// Test fixture with in-process server
class ActiveUnaryReactorTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {};

// =============================================================================
// GetFeature Unary RPC Tests
// =============================================================================

/// @test Validates successful unary RPC response extraction.
///
/// Tests the basic unary RPC flow with synchronous promise/future pattern:
/// 1. Server is configured with a known response
/// 2. Client sends request with matching coordinates
/// 3. OnDone callback fires, completing the promise
/// 4. The done callback receives the owned response, which is verified
///
/// Verifies that:
/// - Status is OK for successful responses
/// - Feature name and location match expected values
/// - Response data is correctly transferred through reactor
TEST_F(ActiveUnaryReactorTest, GetFeature_ValidPoint_ReturnsFeature) {
  // Configure expected response
  routeguide::Feature expected_feature;
  expected_feature.set_name("Test Feature");
  expected_feature.mutable_location()->set_latitude(407128000);
  expected_feature.mutable_location()->set_longitude(-740060000);
  test_service_.SetGetFeatureResponse(expected_feature);

  std::promise<GetFeatureResult> result_promise;
  std::future<GetFeatureResult> result_future = result_promise.get_future();

  // Create request
  const routeguide::Point request = rg_utils::MakePoint(407128000, -740060000);

  // Create callbacks
  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&result_promise](grpc::ClientUnaryReactor*,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::Feature> response) {
    GetFeatureResult result;
    result.status = status;
    if (status.ok()) {
      result.feature = std::move(*response);
    }
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  // Create reactor
  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion
  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Timeout waiting for RPC completion";

  const GetFeatureResult result = result_future.get();

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
/// 2. Client requests an unknown point
/// 3. RPC completes successfully with OK status
/// 4. Returned feature has empty name
///
/// This simulates the RouteGuide behavior where requesting a point
/// not in the database returns a Feature with an empty name.
TEST_F(ActiveUnaryReactorTest, GetFeature_UnknownPoint_ReturnsEmptyFeature) {
  // Configure empty response (simulating unknown point)
  const routeguide::Feature empty_feature;
  test_service_.SetGetFeatureResponse(empty_feature);

  std::promise<GetFeatureResult> result_promise;
  std::future<GetFeatureResult> result_future = result_promise.get_future();

  const routeguide::Point request = rg_utils::MakePoint(0, 0);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&result_promise](grpc::ClientUnaryReactor*,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::Feature> response) {
    GetFeatureResult result;
    result.status = status;
    if (status.ok()) {
      result.feature = std::move(*response);
    }
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  const GetFeatureResult result = result_future.get();

  EXPECT_TRUE(result.status.ok());
  EXPECT_TRUE(result.feature.name().empty());
}

/// @test Validates server error propagation through reactor.
///
/// Tests that server-side errors are correctly propagated to the client:
/// 1. Server is configured to return INTERNAL error with custom message
/// 2. Client sends a valid request
/// 3. OnDone callback fires with error status
/// 4. Error code and message match server configuration
///
/// Verifies that:
/// - Status is not OK
/// - Error code is INTERNAL (as configured)
/// - Error message matches "Test error message"
TEST_F(ActiveUnaryReactorTest, GetFeature_ServerError_PropagatesStatus) {
  test_service_.SetErrorResponse(grpc::StatusCode::INTERNAL, "Test error message");

  std::promise<GetFeatureResult> result_promise;
  std::future<GetFeatureResult> result_future = result_promise.get_future();

  const routeguide::Point request = rg_utils::MakePoint(123, 456);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&result_promise](grpc::ClientUnaryReactor*,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::Feature>) {
    GetFeatureResult result;
    result.status = status;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  const GetFeatureResult result = result_future.get();

  EXPECT_FALSE(result.status.ok());
  EXPECT_EQ(result.status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(result.status.error_message(), "Test error message");
}

/// @test Validates NOT_FOUND error propagation.
///
/// Tests NOT_FOUND status code handling:
/// 1. Server configured to return NOT_FOUND
/// 2. Client sends request
/// 3. Error is properly propagated with correct code
TEST_F(ActiveUnaryReactorTest, GetFeature_NotFoundError_PropagatesStatus) {
  test_service_.SetErrorResponse(grpc::StatusCode::NOT_FOUND, "Feature not found");

  std::promise<GetFeatureResult> result_promise;
  std::future<GetFeatureResult> result_future = result_promise.get_future();

  const routeguide::Point request = rg_utils::MakePoint(999999999, 999999999);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&result_promise](grpc::ClientUnaryReactor*,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::Feature>) {
    GetFeatureResult result;
    result.status = status;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  const GetFeatureResult result = result_future.get();

  EXPECT_FALSE(result.status.ok());
  EXPECT_EQ(result.status.error_code(), grpc::StatusCode::NOT_FOUND);
  EXPECT_EQ(result.status.error_message(), "Feature not found");
}

/// @test Validates unary RPC cancellation triggers OnDone.
///
/// Tests that TryCancel() correctly terminates a unary RPC:
/// 1. Server is configured with a valid response
/// 2. Client initiates RPC and immediately calls TryCancel()
/// 3. gRPC processes cancel signal (best-effort, out-of-band)
/// 4. OnDone fires with final status
///
/// The test accepts both CANCELLED and OK as valid outcomes:
/// - CANCELLED: Cancel arrived before server processed request
/// - OK: Response arrived before cancel took effect
TEST_F(ActiveUnaryReactorTest, GetFeature_TryCancel_TriggersOnDone) {
  routeguide::Feature feature;
  feature.set_name("Should not receive");
  test_service_.SetGetFeatureResponse(feature);

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Point request = rg_utils::MakePoint(123, 456);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&done_promise](grpc::ClientUnaryReactor*,
                             const grpc::Status& status,
                             std::unique_ptr<routeguide::Feature>) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Cancel immediately
  reactor->TryCancel();

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  const grpc::Status status = done_future.get();

  // Cancel may result in CANCELLED or OK (if response arrived before cancel)
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::CANCELLED ||
              status.error_code() == grpc::StatusCode::OK)
      << "Expected CANCELLED or OK, got: " << status.error_code();
}

/// @test Validates expired deadline returns DEADLINE_EXCEEDED.
///
/// Tests gRPC deadline enforcement:
/// 1. Client creates context with already-expired deadline
/// 2. Client attempts RPC with expired context
/// 3. gRPC immediately rejects the RPC
/// 4. OnDone fires with DEADLINE_EXCEEDED status
TEST_F(ActiveUnaryReactorTest, GetFeature_DeadlineExceeded_PropagatesStatus) {
  routeguide::Feature feature;
  feature.set_name("Delayed feature");
  test_service_.SetGetFeatureResponse(feature);

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Point request = rg_utils::MakePoint(123, 456);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&done_promise](grpc::ClientUnaryReactor*,
                             const grpc::Status& status,
                             std::unique_ptr<routeguide::Feature>) {
    done_promise.set_value(status);
  };

  // Create context with already-expired deadline
  auto context = std::make_unique<grpc::ClientContext>();
  context->set_deadline(std::chrono::system_clock::now() - std::chrono::milliseconds(100));

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, std::move(context), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  const grpc::Status status = done_future.get();

  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED)
      << "Expected DEADLINE_EXCEEDED, got: " << status.error_code();
}

/// @test Validates concurrent unary RPCs all complete successfully.
///
/// Tests reactor thread safety with parallel requests:
/// 1. 10 concurrent GetFeature RPCs are initiated simultaneously
/// 2. Each RPC has its own reactor instance and callbacks
/// 3. gRPC thread pool processes requests in parallel
/// 4. Atomic counter tracks completions
TEST_F(ActiveUnaryReactorTest, GetFeature_MultipleConcurrent_AllComplete) {
  routeguide::Feature feature;
  feature.set_name("Concurrent feature");
  test_service_.SetGetFeatureResponse(feature);

  constexpr int kNumConcurrentRpcs = 10;

  std::atomic<int> completed_count{0};
  std::promise<void> all_done_promise;
  const std::future<void> all_done_future = all_done_promise.get_future();

  std::vector<std::unique_ptr<routeguide::GetFeature::ClientReactor>> reactors;
  reactors.reserve(kNumConcurrentRpcs);
  std::vector<grpc::Status> statuses(kNumConcurrentRpcs);
  // Each callback parks its own response, so the addresses can be compared for aliasing afterwards.
  std::vector<std::unique_ptr<routeguide::Feature>> parked(kNumConcurrentRpcs);

  for (int i = 0; i < kNumConcurrentRpcs; ++i) {
    const routeguide::Point request = rg_utils::MakePoint(i * 100, i * -100);

    routeguide::GetFeature::Callbacks cbs;
    cbs.done = [&completed_count, &all_done_promise, &statuses, &parked, i, kNumConcurrentRpcs](
                   grpc::ClientUnaryReactor*, const grpc::Status& status,
                   std::unique_ptr<routeguide::Feature> response) {
      statuses[i] = status;
      parked[i] = std::move(response);
      if (++completed_count == kNumConcurrentRpcs) {
        all_done_promise.set_value();
      }
    };

    reactors.push_back(std::make_unique<routeguide::GetFeature::ClientReactor>(
        *stub_, CreateClientContext(), request, std::move(cbs)));
  }

  auto wait_result = all_done_future.wait_for(std::chrono::seconds(10));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Timeout waiting for concurrent RPCs";

  EXPECT_EQ(completed_count.load(), kNumConcurrentRpcs);
  std::set<const routeguide::Feature*> addresses;
  for (int i = 0; i < kNumConcurrentRpcs; ++i) {
    EXPECT_TRUE(statuses[i].ok()) << "RPC " << i << " failed: " << statuses[i].error_message();
    ASSERT_NE(parked[i], nullptr) << "RPC " << i << " delivered no response";
    addresses.insert(parked[i].get());
  }
  // Every reactor owns its own response: none of them aliases another's.
  EXPECT_EQ(addresses.size(), static_cast<size_t>(kNumConcurrentRpcs));
}

/// @test Validates the done callback receives a non-null response on a failed RPC.
///
/// The response ownership always transfers, so consumers may dereference it without checking for
/// null. On a failed RPC gRPC writes no response, so the callback receives an empty message and the
/// status is what reports the failure:
///
/// 1. Server is configured to fail with PERMISSION_DENIED
/// 2. The done callback receives that status together with a non-null, empty Feature
///
/// This replaces the former GetResponseBeforeOnDone test, whose subject was the removed accessor.
TEST_F(ActiveUnaryReactorTest, GetFeature_FailedRpc_DeliversNonNullEmptyResponse) {
  routeguide::Feature configured_feature;
  configured_feature.set_name("Never delivered");
  test_service_.SetGetFeatureResponse(configured_feature);
  test_service_.SetErrorResponse(grpc::StatusCode::PERMISSION_DENIED, "Denied");

  std::promise<void> done_promise;
  const std::future<void> done_future = done_promise.get_future();
  bool response_was_non_null = false;
  bool response_was_empty = false;
  grpc::Status received_status;

  const routeguide::Point request = rg_utils::MakePoint(123, 456);

  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&done_promise, &received_status, &response_was_non_null, &response_was_empty](
                 grpc::ClientUnaryReactor*, const grpc::Status& status,
                 std::unique_ptr<routeguide::Feature> response) {
    received_status = status;
    response_was_non_null = (response != nullptr);
    if (response_was_non_null) {
      response_was_empty = response->name().empty() && !response->has_location();
    }
    done_promise.set_value();
  };

  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  EXPECT_FALSE(received_status.ok());
  EXPECT_EQ(received_status.error_code(), grpc::StatusCode::PERMISSION_DENIED);
  EXPECT_TRUE(response_was_non_null) << "the response must transfer even when the RPC failed";
  EXPECT_TRUE(response_was_empty) << "a failed RPC must yield an empty message, not stale content";
}

}  // namespace
