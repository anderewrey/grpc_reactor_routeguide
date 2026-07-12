///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2026 anderewrey
///
/// ActiveWriteReactor Test Suite
///
/// Tests the client-streaming (write) reactor pattern using RecordRoute RPC.
/// Uses std::promise/future for synchronization.
///
/// The test fixture creates:
/// - An in-process gRPC server with RecordRoute implementation using rg_utils
/// - Client reactors with callbacks that complete promises directly
/// - Pre-configured feature list for deterministic test scenarios
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace {

/// Test service implementing RecordRoute with distance/feature counting
/// Mirrors route_guide_callback_server.cpp implementation using rg_utils
class TestRouteGuideService final : public routeguide::RouteGuide::CallbackService {
 public:
  void SetFeatureList(const FeatureList& features) {
    feature_list_ = features;
  }

  void SetErrorResponse(grpc::StatusCode code, const std::string& message) {
    error_code_ = code;
    error_message_ = message;
    return_error_ = true;
  }

  void ClearErrorResponse() {
    return_error_ = false;
  }

  /// RecordRoute: Client streams Points, server returns RouteSummary
  grpc::ServerReadReactor<routeguide::Point>* RecordRoute(
      grpc::CallbackServerContext* context,
      routeguide::RouteSummary* summary) override {
    class RecordRouteReactor : public grpc::ServerReadReactor<routeguide::Point> {
     public:
      RecordRouteReactor(routeguide::RouteSummary* summary,
                         const FeatureList& feature_list,
                         bool return_error,
                         grpc::StatusCode error_code,
                         const std::string& error_message)
          : summary_(summary),
            feature_list_(feature_list),
            return_error_(return_error),
            error_code_(error_code),
            error_message_(error_message) {
        start_time_ = std::chrono::steady_clock::now();
        StartRead(&point_);
      }

      void OnReadDone(bool ok) override {
        if (ok) {
          point_count_++;
          // Check if point is a known feature
          if (const auto name = rg_utils::GetFeatureName(point_, feature_list_);
              name != nullptr && strlen(name) > 0) {
            feature_count_++;
          }
          // Calculate distance from previous point
          if (point_count_ > 1) {
            distance_ += rg_utils::GetDistance(previous_, point_);
          }
          previous_ = point_;
          StartRead(&point_);
        } else {
          // Client finished sending - finalize summary
          if (return_error_) {
            Finish(grpc::Status(error_code_, error_message_));
            return;
          }
          auto end_time = std::chrono::steady_clock::now();
          auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time_);
          summary_->set_point_count(point_count_);
          summary_->set_feature_count(feature_count_);
          summary_->set_distance(static_cast<int>(distance_));
          summary_->set_elapsed_time(elapsed.count());
          Finish(grpc::Status::OK);
        }
      }

      void OnDone() override {
        delete this;
      }

     private:
      routeguide::RouteSummary* summary_;
      const FeatureList& feature_list_;
      routeguide::Point point_;
      routeguide::Point previous_;
      int point_count_ = 0;
      int feature_count_ = 0;
      double distance_ = 0.0;
      std::chrono::steady_clock::time_point start_time_;
      bool return_error_;
      grpc::StatusCode error_code_;
      std::string error_message_;
    };

    return new RecordRouteReactor(summary, feature_list_, return_error_, error_code_, error_message_);
  }

 private:
  FeatureList feature_list_;
  bool return_error_ = false;
  grpc::StatusCode error_code_ = grpc::StatusCode::OK;
  std::string error_message_;
};

/// Result container for RecordRoute tests
struct RecordRouteResult {
  grpc::Status status;
  routeguide::RouteSummary summary;
  int write_done_count = 0;
  bool completed = false;
};

/// Test fixture with in-process server and pre-configured features
class ActiveWriteReactorTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {
 protected:
  void SetUp() override {
    RouteGuideTestFixtureBase::SetUp();

    // Create test features at known coordinates
    // Using RouteGuide format: degrees × 10,000,000
    // Feature 1: New York City area (40.7128° N, 74.0060° W)
    test_features_.push_back(rg_utils::MakeFeature("NYC Landmark", 407128000, -740060000));
    // Feature 2: Slightly north (40.8° N, 74.0° W)
    test_features_.push_back(rg_utils::MakeFeature("North Point", 408000000, -740000000));
    // Feature 3: Further north (41.0° N, 74.0° W)
    test_features_.push_back(rg_utils::MakeFeature("Far North", 410000000, -740000000));

    test_service_.SetFeatureList(test_features_);
  }

  /// Calculate expected distance between test points using rg_utils
  double CalculateExpectedDistance(const std::vector<routeguide::Point>& points) {
    double total = 0.0;
    for (size_t i = 1; i < points.size(); ++i) {
      total += rg_utils::GetDistance(points[i - 1], points[i]);
    }
    return total;
  }

  FeatureList test_features_;
};

// =============================================================================
// RecordRoute Client-Streaming Tests
// =============================================================================

/// @test Validates successful client streaming with multiple points.
///
/// Tests the complete client-streaming flow:
/// 1. Client creates RecordRoute reactor
/// 2. Client sends 3 points via SendRequest(), waiting for OnWriteDone between each
/// 3. Client signals end of stream via CloseRequestStream()
/// 4. Server processes all points, calculates distance and feature count
/// 5. OnDone callback fires with RouteSummary response
///
/// Verifies that:
/// - Status is OK
/// - point_count matches sent points (3)
/// - feature_count matches known features (2 - NYC and North Point)
/// - distance is calculated correctly using Haversine formula
///
/// Note: gRPC requires waiting for OnWriteDone() before calling SendRequest() again.
/// Overlapping writes cause GRPC_CALL_ERROR_TOO_MANY_OPERATIONS.
TEST_F(ActiveWriteReactorTest, RecordRoute_MultiplePoints_ReturnsCorrectSummary) {
  std::promise<RecordRouteResult> result_promise;
  std::future<RecordRouteResult> result_future = result_promise.get_future();

  // Synchronization for sequential writes (gRPC requires waiting for OnWriteDone)
  std::mutex write_mutex;
  std::condition_variable write_cv;
  bool write_ready = true;

  // Create callbacks
  routeguide::RecordRoute::Callbacks cbs;
  cbs.write_done = [&write_mutex, &write_cv, &write_ready](
                       grpc::ClientWriteReactor<routeguide::Point>*, bool ok) {
    std::lock_guard<std::mutex> lock(write_mutex);
    write_ready = true;
    write_cv.notify_one();
  };
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>* base_reactor,
                                const grpc::Status& status,
                                const routeguide::RouteSummary& response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = response;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  // Create reactor
  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send points - two are known features, one is not
  std::vector<routeguide::Point> points;
  points.push_back(rg_utils::MakePoint(407128000, -740060000));  // NYC Landmark (feature)
  points.push_back(rg_utils::MakePoint(408000000, -740000000));  // North Point (feature)
  points.push_back(rg_utils::MakePoint(409000000, -740000000));  // Unknown point (not a feature)

  // Computed before sending: SendRequest() takes ownership of each point, so points cannot be
  // read again afterward.
  double expected_distance = CalculateExpectedDistance(points);

  for (auto& point : points) {
    // Wait for previous write to complete before sending next
    {
      std::unique_lock<std::mutex> lock(write_mutex);
      write_cv.wait(lock, [&write_ready] { return write_ready; });
      write_ready = false;
    }
    reactor->SendRequest(std::move(point));
  }

  // Wait for last write to complete before WritesDone
  {
    std::unique_lock<std::mutex> lock(write_mutex);
    write_cv.wait_for(lock, std::chrono::seconds(1), [&write_ready] { return write_ready; });
  }

  // Signal end of stream
  reactor->CloseRequestStream();

  // Wait for completion
  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Timeout waiting for RPC completion";

  RecordRouteResult result = result_future.get();

  // Verify results
  EXPECT_TRUE(result.completed);
  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  EXPECT_EQ(result.summary.point_count(), 3);
  EXPECT_EQ(result.summary.feature_count(), 2);  // NYC and North Point are known features

  // Verify distance is calculated (should be > 0 for different coordinates)
  EXPECT_GT(result.summary.distance(), 0);
  // Allow small tolerance for floating point to int conversion
  EXPECT_NEAR(result.summary.distance(), expected_distance, 1.0);
}

/// @test Validates empty stream (zero points) completes successfully.
///
/// Tests the edge case of immediately calling WritesDone:
/// 1. Client creates RecordRoute reactor
/// 2. Client immediately calls CloseRequestStream() without sending any points
/// 3. Server receives empty stream
/// 4. OnDone callback fires with RouteSummary showing zero counts
///
/// Verifies that:
/// - Status is OK (empty stream is valid)
/// - point_count is 0
/// - feature_count is 0
/// - distance is 0
TEST_F(ActiveWriteReactorTest, RecordRoute_EmptyStream_ReturnsZeroCounts) {
  std::promise<RecordRouteResult> result_promise;
  std::future<RecordRouteResult> result_future = result_promise.get_future();

  routeguide::RecordRoute::Callbacks cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                                const grpc::Status& status,
                                const routeguide::RouteSummary& response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = response;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Immediately signal end of stream without sending any points
  reactor->CloseRequestStream();

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  RecordRouteResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  EXPECT_EQ(result.summary.point_count(), 0);
  EXPECT_EQ(result.summary.feature_count(), 0);
  EXPECT_EQ(result.summary.distance(), 0);
}

/// @test Validates single point stream (no distance calculation).
///
/// Tests sending exactly one point:
/// 1. Client sends single point
/// 2. Client calls CloseRequestStream()
/// 3. Server returns summary with point_count=1, distance=0
///
/// Verifies that:
/// - Single point is counted
/// - Distance is 0 (need 2+ points for distance)
/// - Feature is counted if it's a known location
TEST_F(ActiveWriteReactorTest, RecordRoute_SinglePoint_ReturnsZeroDistance) {
  std::promise<RecordRouteResult> result_promise;
  std::future<RecordRouteResult> result_future = result_promise.get_future();

  routeguide::RecordRoute::Callbacks cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                                const grpc::Status& status,
                                const routeguide::RouteSummary& response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = response;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send single point (known feature)
  reactor->SendRequest(rg_utils::MakePoint(407128000, -740060000));  // NYC Landmark
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  reactor->CloseRequestStream();

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  RecordRouteResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  EXPECT_EQ(result.summary.point_count(), 1);
  EXPECT_EQ(result.summary.feature_count(), 1);  // NYC Landmark is a known feature
  EXPECT_EQ(result.summary.distance(), 0);  // No distance with single point
}

/// @test Validates overlapping writes are rejected with write_pending guard.
///
/// gRPC requires that only one StartWrite() be in flight at a time.
/// SendRequest() checks write_pending_ and returns false if a write is in progress,
/// preventing GRPC_CALL_ERROR_TOO_MANY_OPERATIONS.
///
/// Tests the protection mechanism:
/// 1. Client sends first point (accepted, returns true)
/// 2. Client immediately sends second point without waiting (rejected, returns false)
/// 3. Client waits for OnWriteDone, then sends third point (accepted)
/// 4. RPC completes successfully with only 2 points (the accepted ones)
///
/// Verifies that:
/// - First SendRequest() returns true
/// - Overlapping SendRequest() returns false
/// - After waiting, SendRequest() returns true again
/// - Final point_count reflects only accepted writes
TEST_F(ActiveWriteReactorTest, RecordRoute_OverlappingWrites_RejectedByWritePending) {
  std::promise<RecordRouteResult> result_promise;
  std::future<RecordRouteResult> result_future = result_promise.get_future();

  // Track write completion for synchronization
  std::mutex write_mutex;
  std::condition_variable write_cv;
  bool write_ready = false;

  routeguide::RecordRoute::Callbacks cbs;
  cbs.write_done = [&write_mutex, &write_cv, &write_ready](
                       grpc::ClientWriteReactor<routeguide::Point>*, bool ok) {
    std::lock_guard<std::mutex> lock(write_mutex);
    write_ready = true;
    write_cv.notify_one();
  };
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                                const grpc::Status& status,
                                const routeguide::RouteSummary& response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = response;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // First write should be accepted
  bool first_accepted = reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000));
  EXPECT_TRUE(first_accepted) << "First SendRequest should be accepted";

  // Second write immediately (without waiting) should be rejected
  bool second_accepted = reactor->SendRequest(rg_utils::MakePoint(401000000, -740000000));
  EXPECT_FALSE(second_accepted) << "Overlapping SendRequest should be rejected";

  // Wait for first write to complete
  {
    std::unique_lock<std::mutex> lock(write_mutex);
    write_cv.wait_for(lock, std::chrono::seconds(1), [&write_ready] { return write_ready; });
    write_ready = false;
  }

  // Third write should now be accepted
  bool third_accepted = reactor->SendRequest(rg_utils::MakePoint(402000000, -740000000));
  EXPECT_TRUE(third_accepted) << "SendRequest after OnWriteDone should be accepted";

  // Wait for third write to complete
  {
    std::unique_lock<std::mutex> lock(write_mutex);
    write_cv.wait_for(lock, std::chrono::seconds(1), [&write_ready] { return write_ready; });
  }

  reactor->CloseRequestStream();

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  RecordRouteResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  // Only 2 points were actually sent (first and third; second was rejected)
  EXPECT_EQ(result.summary.point_count(), 2);
}

/// @test Validates OnWriteDone callback fires for each write.
///
/// Tests that the write_done callback is invoked after each SendRequest:
/// 1. Client sends 3 points
/// 2. OnWriteDone callback fires for each point with ok=true
/// 3. Callback count tracked
///
/// Verifies that:
/// - write_done callback fires exactly 3 times
/// - Each callback has ok=true
TEST_F(ActiveWriteReactorTest, RecordRoute_OnWriteDone_FiresForEachWrite) {
  std::promise<RecordRouteResult> result_promise;
  std::future<RecordRouteResult> result_future = result_promise.get_future();
  std::atomic<int> write_done_count{0};

  routeguide::RecordRoute::Callbacks cbs;
  cbs.write_done = [&write_done_count](grpc::ClientWriteReactor<routeguide::Point>*, bool ok) {
    if (ok) {
      ++write_done_count;
    }
  };
  cbs.done = [&result_promise, &write_done_count](grpc::ClientWriteReactor<routeguide::Point>*,
                                                   const grpc::Status& status,
                                                   const routeguide::RouteSummary& response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = response;
    result.write_done_count = write_done_count.load();
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send 3 points, waiting for each write to complete
  for (int i = 0; i < 3; ++i) {
    reactor->SendRequest(rg_utils::MakePoint(400000000 + i * 1000000, -740000000));
    // Wait for write to be acknowledged before sending next
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  reactor->CloseRequestStream();

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  RecordRouteResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  EXPECT_EQ(result.write_done_count, 3) << "Expected 3 OnWriteDone callbacks";
}

/// @test Validates TryCancel terminates client streaming RPC.
///
/// Tests that TryCancel correctly terminates an active stream:
/// 1. Client sends some points
/// 2. Client calls TryCancel() before CloseRequestStream()
/// 3. gRPC processes cancel signal
/// 4. OnDone fires with CANCELLED status
///
/// The test accepts both CANCELLED and OK as valid outcomes:
/// - CANCELLED: Cancel arrived before server processed all data
/// - OK: All data processed before cancel took effect
TEST_F(ActiveWriteReactorTest, RecordRoute_TryCancel_TerminatesStream) {
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::RecordRoute::Callbacks cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&done_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                              const grpc::Status& status,
                              const routeguide::RouteSummary&) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send a point then cancel
  reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000));
  reactor->TryCancel();

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  // Cancel may result in CANCELLED or OK (if processed before cancel)
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::CANCELLED ||
              status.error_code() == grpc::StatusCode::OK)
      << "Expected CANCELLED or OK, got: " << status.error_code();
}

/// @test Validates server error propagation in client streaming.
///
/// Tests that server-side errors are correctly propagated:
/// 1. Server is configured to return INTERNAL error
/// 2. Client sends points and calls CloseRequestStream()
/// 3. Server processes and returns error
/// 4. OnDone callback fires with error status
///
/// Verifies that:
/// - Status is not OK
/// - Error code is INTERNAL (as configured)
/// - Error message matches "Server test error"
TEST_F(ActiveWriteReactorTest, RecordRoute_ServerError_PropagatesStatus) {
  // Configure server to return error after receiving stream
  test_service_.SetErrorResponse(grpc::StatusCode::INTERNAL, "Server test error");

  std::promise<RecordRouteResult> result_promise;
  std::future<RecordRouteResult> result_future = result_promise.get_future();

  routeguide::RecordRoute::Callbacks cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                                const grpc::Status& status,
                                const routeguide::RouteSummary& response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = response;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send a point and end stream
  reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000));
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  reactor->CloseRequestStream();

  auto wait_result = result_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  RecordRouteResult result = result_future.get();

  EXPECT_FALSE(result.status.ok());
  EXPECT_EQ(result.status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(result.status.error_message(), "Server test error");
}

}  // namespace
