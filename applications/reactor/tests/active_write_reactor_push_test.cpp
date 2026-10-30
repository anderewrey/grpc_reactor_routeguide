///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
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
#include <cstring>
#include <future>
#include <memory>
#include <semaphore>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace {

/// Every reactor in this file is instantiated with Delivery::kPush; its pull counterpart is
/// covered by active_write_reactor_pull_test.cpp.
constexpr auto kDelivery = RpcReactor::Client::Delivery::kPush;
constexpr auto kSubmission = RpcReactor::Client::Submission::kApplicationPushes;

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

  /// Makes the server end every RecordRoute RPC with this status as soon as it starts, without reading
  /// anything: the client learns of it only through its own next write, its close, or OnDone().
  void SetEndEarly(grpc::StatusCode code, const std::string& message) {
    error_code_ = code;
    error_message_ = message;
    end_early_ = true;
  }

  /// RecordRoute: Client streams Points, server returns RouteSummary
  grpc::ServerReadReactor<routeguide::Point>* RecordRoute(
      grpc::CallbackServerContext* /*context*/,
      routeguide::RouteSummary* summary) override {
    class RecordRouteReactor : public grpc::ServerReadReactor<routeguide::Point> {
     public:
      RecordRouteReactor(routeguide::RouteSummary* summary,
                         const FeatureList& feature_list,
                         bool return_error,
                         grpc::StatusCode error_code,
                         const std::string& error_message,
                         bool end_early)
          : summary_(summary),
            feature_list_(feature_list),
            return_error_(return_error),
            error_code_(error_code),
            error_message_(error_message) {
        start_time_ = std::chrono::steady_clock::now();
        if (end_early) {
          Finish(grpc::Status(error_code_, error_message_));
          return;
        }
        StartRead(&point_);
      }

      void OnReadDone(bool ok) override {
        if (ok) {
          point_count_++;
          // Check if point is a known feature
          if (const auto* const name = rg_utils::GetFeatureName(point_, feature_list_);
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
          summary_->set_elapsed_time(static_cast<int32_t>(elapsed.count()));
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

    return new RecordRouteReactor(summary, feature_list_, return_error_, error_code_, error_message_, end_early_);
  }

 private:
  FeatureList feature_list_;
  bool return_error_ = false;
  bool end_early_ = false;
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
  static double CalculateExpectedDistance(const std::vector<routeguide::Point>& points) {
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

  std::binary_semaphore write_slot{1};  // gRPC requires waiting for OnWriteDone between writes

  // Create callbacks
  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [&write_slot](grpc::ClientWriteReactor<routeguide::Point>*, bool) {
    write_slot.release();
  };
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>* /*base_reactor*/,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::RouteSummary> response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = std::move(*response);
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  // Create reactor
  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send points - two are known features, one is not
  std::vector<routeguide::Point> points;
  points.push_back(rg_utils::MakePoint(407128000, -740060000));  // NYC Landmark (feature)
  points.push_back(rg_utils::MakePoint(408000000, -740000000));  // North Point (feature)
  points.push_back(rg_utils::MakePoint(409000000, -740000000));  // Unknown point (not a feature)

  // Computed before sending: SendRequest() takes ownership of each point, so points cannot be
  // read again afterward.
  const double expected_distance = CalculateExpectedDistance(points);

  for (auto& point : points) {
    EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Previous write never completed";
    EXPECT_TRUE(reactor->SendRequest(std::move(point)));
  }
  // Wait for last write to complete before WritesDone
  EXPECT_TRUE(write_slot.try_acquire_for(std::chrono::seconds(5))) << "Last write never completed";

  // Signal end of stream
  EXPECT_TRUE(reactor->CloseRequestStream());

  // Wait for completion
  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for RPC completion";

  const RecordRouteResult result = result_future.get();

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

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::RouteSummary> response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = std::move(*response);
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Immediately signal end of stream without sending any points
  EXPECT_TRUE(reactor->CloseRequestStream());

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RecordRouteResult result = result_future.get();

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

  std::binary_semaphore write_done{0};  // Released once the single write completes

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [&write_done](grpc::ClientWriteReactor<routeguide::Point>*, bool) { write_done.release(); };
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::RouteSummary> response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = std::move(*response);
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send single point (known feature)
  reactor->SendRequest(rg_utils::MakePoint(407128000, -740060000));  // NYC Landmark
  // Waits for the write rather than sleeping: CloseRequestStream() is rejected while a write is pending,
  // and a rejected close would leave the RPC open until the timeout.
  EXPECT_TRUE(write_done.try_acquire_for(std::chrono::seconds(5))) << "Write never completed";
  EXPECT_TRUE(reactor->CloseRequestStream());

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RecordRouteResult result = result_future.get();

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
/// Spams writes from the test thread and checks the invariant the guard enforces, rather than betting
/// on a write still being pending at the next call, which a fast transport can make false:
/// 1. Client calls SendRequest() in a row, without waiting, until one is accepted and one rejected
/// 2. Each accepted write is only legal if every earlier one has completed
/// 3. Once every accepted write completed, the client closes the stream
///
/// Verifies that:
/// - Accepted writes never exceed completed ones by more than the single one in flight
/// - Some calls are accepted, and some are rejected as overlapping
/// - Final point_count reflects only accepted writes
TEST_F(ActiveWriteReactorTest, RecordRoute_OverlappingWrites_RejectedByWritePending) {
  std::promise<RecordRouteResult> result_promise;
  std::future<RecordRouteResult> result_future = result_promise.get_future();

  std::atomic<int> completed{0};

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [&completed](grpc::ClientWriteReactor<routeguide::Point>*, bool) { ++completed; };
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::RouteSummary> response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = std::move(*response);
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Runs until it has seen both an acceptance and a rejection, since a fast transport can complete
  // many writes before the next call.
  constexpr int kMaxAttempts = 1000;
  int accepted = 0;
  int rejected = 0;
  for (int i = 0; i < kMaxAttempts && (accepted == 0 || rejected == 0); ++i) {
    if (reactor->SendRequest(rg_utils::MakePoint(400000000 + i, -740000000))) {
      ++accepted;
    } else {
      ++rejected;
    }
    // Not an ASSERT: returning now would leave the RPC running against this test's locals.
    if (accepted > completed.load() + 1) {
      ADD_FAILURE() << "Two writes were in flight at once, so the guard did not reject the overlap";
      break;
    }
  }
  EXPECT_GT(accepted, 0) << "Every write was rejected";
  EXPECT_GT(rejected, 0) << "No write was rejected, so no overlap was exercised";

  // Close only once the last accepted write completed, since a pending write rejects the close.
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (completed.load() < accepted && std::chrono::steady_clock::now() < deadline) {
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  }
  EXPECT_EQ(completed.load(), accepted) << "An accepted write never completed";
  EXPECT_TRUE(reactor->CloseRequestStream());

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RecordRouteResult result = result_future.get();

  EXPECT_TRUE(result.status.ok()) << "Status: " << result.status.error_message();
  // Only the accepted writes reached the server
  EXPECT_EQ(result.summary.point_count(), accepted);
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

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [&write_done_count](grpc::ClientWriteReactor<routeguide::Point>*, bool ok) {
    if (ok) {
      ++write_done_count;
    }
  };
  cbs.done = [&result_promise, &write_done_count](grpc::ClientWriteReactor<routeguide::Point>*,
                                                  const grpc::Status& status,
                                                  std::unique_ptr<routeguide::RouteSummary> response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = std::move(*response);
    result.write_done_count = write_done_count.load();
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send 3 points, waiting for each write to be acknowledged before sending the next. A fixed sleep is
  // not enough on a slow run, such as under ThreadSanitizer: a write still in flight makes the next
  // SendRequest() and the close below refused.
  for (int i = 0; i < 3; ++i) {
    ASSERT_TRUE(reactor->SendRequest(rg_utils::MakePoint(400000000 + (i * 1000000), -740000000)));
    for (int wait = 0; wait < 1000 && write_done_count.load() <= i; ++wait) {
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  }
  // Accepted, not merely attempted: an unclosed write side keeps the write-flow hold.
  EXPECT_TRUE(reactor->CloseRequestStream());

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RecordRouteResult result = result_future.get();

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

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&done_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                             const grpc::Status& status,
                             std::unique_ptr<routeguide::RouteSummary>) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send a point then cancel
  reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000));
  reactor->TryCancel();

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

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

  std::binary_semaphore write_done{0};  // Released once the single write completes

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [&write_done](grpc::ClientWriteReactor<routeguide::Point>*, bool) { write_done.release(); };
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*,
                               const grpc::Status& status,
                               std::unique_ptr<routeguide::RouteSummary> response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = std::move(*response);
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  // Send a point and end stream
  reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000));
  // Waits for the write rather than sleeping, as in RecordRoute_SinglePoint_ReturnsZeroDistance.
  EXPECT_TRUE(write_done.try_acquire_for(std::chrono::seconds(5))) << "Write never completed";
  EXPECT_TRUE(reactor->CloseRequestStream());

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));

  const RecordRouteResult result = result_future.get();

  EXPECT_FALSE(result.status.ok());
  EXPECT_EQ(result.status.error_code(), grpc::StatusCode::INTERNAL);
  EXPECT_EQ(result.status.error_message(), "Server test error");
  // The response still transferred, holding an empty summary: the callback above dereferenced it
  // unconditionally, which is only safe because a failed RPC yields a message rather than nothing.
  EXPECT_EQ(result.summary.point_count(), 0);
  EXPECT_EQ(result.summary.feature_count(), 0);
  EXPECT_EQ(result.summary.distance(), 0);
}

/// The RecordRoute reactor, observed: it signals once OnDone() has run. Only for a test that binds no done
/// slot, which otherwise has no way to know when the reactor may be destroyed. gRPC requires a reactor to
/// outlive OnDone(), so destroying it after a fixed sleep races any reaction still running, which
/// ThreadSanitizer reports. Built as routeguide::RecordRoute::ClientReactor is, since that class is final.
class ObservedRecordRouteReactor final
    : public RpcReactor::Client::ActiveWriteReactor<routeguide::Point, routeguide::RouteSummary, kDelivery,
                                                    kSubmission> {
  using Base =
      RpcReactor::Client::ActiveWriteReactor<routeguide::Point, routeguide::RouteSummary, kDelivery, kSubmission>;

 public:
  ObservedRecordRouteReactor(routeguide::RouteGuide::Stub& stub, std::unique_ptr<grpc::ClientContext> context,
                             routeguide::RecordRoute::Callbacks<kDelivery, kSubmission>&& cbs, std::promise<void>& done)
      : Base(std::move(context), std::move(cbs)), done_(done) {
    stub.async()->RecordRoute(this->context_.get(), this->response_.get(), this);
    this->StartCall();
  }

 protected:
  void OnDone(const grpc::Status& status) override {
    Base::OnDone(status);  // Runs with the done slot unbound, which is what the test is about
    done_.set_value();     // Last action: the reactor may be destroyed from here on
  }

 private:
  std::promise<void>& done_;
};

/// @test Validates an unbound done slot discards the response without breaking the RPC.
///
/// With no done callback the response has no accessor, so it stays in the reactor until destruction:
///
/// 1. Only write_done is bound, and it counts the completed writes
/// 2. The RPC runs to completion, and the summary is never handed anywhere
/// 3. The reactor is destroyed only once OnDone has run, which the observed reactor reports
TEST_F(ActiveWriteReactorTest, RecordRoute_NoDoneCallbackBound_RpcStillCompletes) {
  std::atomic<int> write_done_count{0};
  std::promise<void> done_promise;
  const std::future<void> done_future = done_promise.get_future();

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  // cbs.done deliberately left unbound.
  cbs.write_done = [&write_done_count](grpc::ClientWriteReactor<routeguide::Point>*, bool ok) {
    if (ok) ++write_done_count;
  };

  auto reactor =
      std::make_unique<ObservedRecordRouteReactor>(*stub_, CreateClientContext(), std::move(cbs), done_promise);

  reactor->SendRequest(rg_utils::MakePoint(407838351, -746143763));
  // The close must be accepted, not merely attempted: it is refused while the write is pending, and an
  // unclosed write side keeps the write-flow hold, so the RPC would never complete.
  for (int i = 0; i < 200 && !reactor->CloseRequestStream(); ++i) {
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "OnDone never ran with the done slot unbound";
  EXPECT_GE(write_done_count.load(), 1) << "the write path must run with no done callback bound";
}

// =============================================================================
// Write-flow hold
// =============================================================================
// SendRequest() issues StartWrite() from the application thread, outside any reaction, so the reactor
// holds the RPC for its whole write flow, as gRPC's AddHold() documentation prescribes. The hold is
// released when the write side ends: SendLastRequest(), CloseRequestStream(), a failed write, or
// TryCancel(). A client-streaming RPC has no read side, so nothing else can end it.

/// @test Validates OnDone waits for the write side when the server ends the RPC on its own.
///
/// 1. The server ends the RPC with an error as soon as it starts, while no write is in flight
/// 2. No reaction reports that to a client-streaming reactor, so OnDone stays back: the write-flow
///    hold keeps the call alive, and a write issued now could not reach a destroyed call
/// 3. Ending the write side releases the hold, and OnDone reports the server's status
TEST_F(ActiveWriteReactorTest, RecordRoute_ServerEndsWhileIdle_OnDoneWaitsForWriteSide) {
  test_service_.SetEndEarly(grpc::StatusCode::FAILED_PRECONDITION, "Server ends early");
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&done_promise](grpc::ClientWriteReactor<routeguide::Point>*, const grpc::Status& status,
                             std::unique_ptr<routeguide::RouteSummary>) { done_promise.set_value(status); };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  EXPECT_EQ(done_future.wait_for(std::chrono::milliseconds(300)), std::future_status::timeout)
      << "OnDone fired while the write side was still open";

  EXPECT_TRUE(reactor->CloseRequestStream()) << "the close must be issued on a call kept alive by the hold";
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  const grpc::Status status = done_future.get();
  EXPECT_EQ(status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(status.error_message(), "Server ends early");
}

/// @test Validates a failed write ends the write side by itself.
///
/// 1. The server ends the RPC as soon as it starts
/// 2. The client keeps writing, which is safe since the hold keeps the call alive, until a write fails
/// 3. The failed write ends the write flow, with no close and no cancel, and OnDone follows
TEST_F(ActiveWriteReactorTest, RecordRoute_ServerEndsWhileIdle_FailedWriteEndsWriteSide) {
  test_service_.SetEndEarly(grpc::StatusCode::FAILED_PRECONDITION, "Server ends early");
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();
  std::atomic<int> failed_writes{0};

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [&failed_writes](grpc::ClientWriteReactor<routeguide::Point>*, bool ok) {
    if (!ok) ++failed_writes;
  };
  cbs.done = [&done_promise](grpc::ClientWriteReactor<routeguide::Point>*, const grpc::Status& status,
                             std::unique_ptr<routeguide::RouteSummary>) { done_promise.set_value(status); };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  // A write may still succeed while the server's status is on its way, and a write is refused while
  // the previous one is pending, so the client simply keeps trying until OnDone arrives.
  for (int i = 0; i < 200 && done_future.wait_for(std::chrono::milliseconds(10)) != std::future_status::ready;
       ++i) {
    reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000));
  }

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  EXPECT_EQ(done_future.get().error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_GE(failed_writes.load(), 1) << "OnDone must follow from a failed write, the only action taken";
  EXPECT_FALSE(reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000)))
      << "a write was accepted after the write side ended";
}

/// @test Validates TryCancel completes an idle RPC with no other write-side action.
///
/// TryCancel's purpose is to provoke OnDone, so it must end the write flow itself: an idle write side
/// would otherwise keep the hold, and OnDone would never come.
TEST_F(ActiveWriteReactorTest, RecordRoute_TryCancelWhileIdle_CompletesWithoutWriteSideAction) {
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&done_promise](grpc::ClientWriteReactor<routeguide::Point>*, const grpc::Status& status,
                             std::unique_ptr<routeguide::RouteSummary>) { done_promise.set_value(status); };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  reactor->TryCancel();

  ASSERT_TRUE(AwaitRpc(done_future, [] {})) << "OnDone did not follow TryCancel on an idle write side";
  EXPECT_EQ(done_future.get().error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_FALSE(reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000)))
      << "a write was accepted after TryCancel";
}

/// @test Validates every write call is refused once the write side has ended.
///
/// SendLastRequest() ends the write flow, so no later call may write or close again, and the RPC
/// completes normally.
TEST_F(ActiveWriteReactorTest, RecordRoute_EndedWriteSide_RejectsEveryWriteCall) {
  std::promise<RecordRouteResult> result_promise;
  std::future<RecordRouteResult> result_future = result_promise.get_future();

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&result_promise](grpc::ClientWriteReactor<routeguide::Point>*, const grpc::Status& status,
                               std::unique_ptr<routeguide::RouteSummary> response) {
    RecordRouteResult result;
    result.status = status;
    result.summary = *response;
    result.completed = true;
    result_promise.set_value(std::move(result));
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  ASSERT_TRUE(reactor->SendLastRequest(rg_utils::MakePoint(407128000, -740060000)));
  EXPECT_FALSE(reactor->SendRequest(rg_utils::MakePoint(408000000, -740000000)));
  EXPECT_FALSE(reactor->SendLastRequest(rg_utils::MakePoint(408000000, -740000000)));
  EXPECT_FALSE(reactor->CloseRequestStream());

  ASSERT_TRUE(AwaitRpc(result_future, [&] { reactor->TryCancel(); }));
  const RecordRouteResult result = result_future.get();
  EXPECT_TRUE(result.status.ok()) << result.status.error_message();
  EXPECT_EQ(result.summary.point_count(), 1);
}

/// @test Validates TryCancel from another thread, racing a thread that keeps writing.
///
/// The write calls claim the write flow, so a cancel arriving mid-write cannot release the hold under
/// them: whichever side looks last ends the flow, and the hold is released exactly once.
/// 1. A writer thread calls SendRequest() in a tight loop, so calls are often mid-flight
/// 2. The test thread cancels the RPC
/// 3. OnDone fires exactly once, and the writer's later calls are refused
TEST_F(ActiveWriteReactorTest, RecordRoute_TryCancelRacingWrites_CompletesOnce) {
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();
  std::atomic<int> done_count{0};

  routeguide::RecordRoute::Callbacks<kDelivery, kSubmission> cbs;
  cbs.write_done = [](grpc::ClientWriteReactor<routeguide::Point>*, bool) {};
  cbs.done = [&done_promise, &done_count](grpc::ClientWriteReactor<routeguide::Point>*,
                                          const grpc::Status& status, std::unique_ptr<routeguide::RouteSummary>) {
    if (++done_count == 1) done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>>(
      *stub_, CreateClientContext(), std::move(cbs));

  std::atomic<bool> stop{false};
  std::atomic<int> accepted{0};
  std::thread writer([&] {
    while (!stop) {
      if (reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000))) ++accepted;
    }
  });
  // Lets some writes complete first, so the cancel lands on a stream that is busy writing.
  for (int i = 0; i < 200 && accepted.load() < 3; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  reactor->TryCancel();

  const bool completed = AwaitRpc(done_future, [&] { reactor->TryCancel(); });
  stop = true;
  writer.join();
  ASSERT_TRUE(completed);
  EXPECT_EQ(done_future.get().error_code(), grpc::StatusCode::CANCELLED);
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(done_count.load(), 1) << "OnDone fired more than once";
  EXPECT_GE(accepted.load(), 1) << "the writer never wrote, so nothing raced the cancel";
  EXPECT_FALSE(reactor->SendRequest(rg_utils::MakePoint(400000000, -740000000)));
}

}  // namespace
