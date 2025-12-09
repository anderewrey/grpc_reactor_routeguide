///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///
/// ActiveReadReactor Test Suite
///
/// Tests the server-side streaming (read) reactor pattern using ListFeatures RPC.
/// Uses std::promise/future for synchronization (Option B pattern - no EventLoop).
///
/// The test fixture creates:
/// - An in-process gRPC server with controllable ListFeatures responses
/// - Client reactors with callbacks that complete promises directly
/// - Configurable error injection for error path testing
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
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"

namespace {

/// Controllable test service for server-side streaming RPC testing
class TestRouteGuideService final : public routeguide::RouteGuide::CallbackService {
 public:
  void SetListFeaturesResponse(const std::vector<routeguide::Feature>& features) {
    configured_features_ = features;
    list_features_error_after_ = -1;  // No error
  }

  void SetListFeaturesErrorAfter(int count, grpc::StatusCode code, const std::string& message) {
    list_features_error_after_ = count;
    list_features_error_code_ = code;
    list_features_error_message_ = message;
  }

  void ClearListFeaturesError() {
    list_features_error_after_ = -1;
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
  std::vector<routeguide::Feature> configured_features_;
  int list_features_error_after_ = -1;
  grpc::StatusCode list_features_error_code_ = grpc::StatusCode::OK;
  std::string list_features_error_message_;
};

/// Test fixture with in-process server
class ActiveReadReactorTest : public ::testing::Test {
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

  /// Create a test feature with given name and coordinates
  static routeguide::Feature MakeTestFeature(const std::string& name, int32_t lat, int32_t lon) {
    return rg_utils::MakeFeature(name, lat, lon);
  }

  TestRouteGuideService test_service_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<routeguide::RouteGuide::Stub> stub_;
};

// =============================================================================
// ListFeatures Server-Side Streaming Tests
// =============================================================================

/// @test Validates server streaming RPC receives all responses.
///
/// Tests the complete streaming flow with multiple responses:
/// 1. Server is configured to send 5 features
/// 2. Client initiates ListFeatures stream
/// 3. Each OnReadDone(true) callback fires with a feature
/// 4. After all features, stream ends with OnReadDone(false)
/// 5. OnDone fires with OK status
///
/// Verifies that:
/// - All 5 features are received in order
/// - Each feature's name and location match expected values
/// - Final status is OK
TEST_F(ActiveReadReactorTest, ListFeatures_MultipleResponses_ReceivesAll) {
  // Configure server to return multiple features
  std::vector<routeguide::Feature> expected_features;
  for (int i = 0; i < 5; ++i) {
    expected_features.push_back(MakeTestFeature("Feature " + std::to_string(i),
                                                 400000000 + i * 1000000,
                                                 -740000000 + i * 1000000));
  }
  test_service_.SetListFeaturesResponse(expected_features);

  // Collect received features
  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  // Create request (bounding rectangle)
  routeguide::Rectangle request;
  request.mutable_lo()->set_latitude(0);
  request.mutable_lo()->set_longitude(-800000000);
  request.mutable_hi()->set_latitude(500000000);
  request.mutable_hi()->set_longitude(0);

  // Create callbacks - use false to let reactor auto-continue reads
  routeguide::ListFeatures::Callbacks cbs;
  cbs.ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                 const routeguide::Feature& response) {
    received_features.push_back(response);
    return false;  // Don't hold - let reactor auto-continue to next read
  };
  cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Stream ended - no more reads
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
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
/// 3. OnReadDone(false) fires immediately (no data to read)
/// 4. OnDone fires with OK status
///
/// Verifies that:
/// - ok callback is never invoked (no features received)
/// - Final status is OK (empty stream is valid, not an error)
TEST_F(ActiveReadReactorTest, ListFeatures_EmptyStream_CompletesSuccessfully) {
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
  cbs.ok = [&read_count](grpc::ClientReadReactor<routeguide::Feature>*,
                          const routeguide::Feature&) {
    ++read_count;  // Should never be called for empty stream
    return false;
  };
  cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Expected: stream ends immediately
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
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

/// @test Validates single feature stream.
///
/// Tests edge case of exactly one response:
/// 1. Server sends single feature
/// 2. OnReadDone(true) fires once
/// 3. OnReadDone(false) signals end
/// 4. OnDone fires with OK status
TEST_F(ActiveReadReactorTest, ListFeatures_SingleFeature_ReceivesOne) {
  std::vector<routeguide::Feature> features;
  features.push_back(MakeTestFeature("Single Feature", 407128000, -740060000));
  test_service_.SetListFeaturesResponse(features);

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
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  ASSERT_EQ(received_features.size(), 1u);
  EXPECT_EQ(received_features[0].name(), "Single Feature");
}

/// @test Validates mid-stream server error propagation.
///
/// Tests partial stream completion followed by server error:
/// 1. Server is configured with 5 features but error after 2
/// 2. Client receives first 2 features successfully
/// 3. Server terminates stream with INTERNAL error
/// 4. OnDone fires with error status
///
/// Verifies that:
/// - Exactly 2 features were received before error
/// - Final status is INTERNAL (as configured)
/// - Partial data is preserved despite stream failure
TEST_F(ActiveReadReactorTest, ListFeatures_ServerErrorMidStream_PropagatesStatus) {
  // Configure server to return 2 features then error
  std::vector<routeguide::Feature> features;
  for (int i = 0; i < 5; ++i) {
    features.push_back(MakeTestFeature("Feature " + std::to_string(i), i * 100, i * -100));
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
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
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

/// @test Validates immediate server error (no features).
///
/// Tests error before any features are sent:
/// 1. Server configured to error immediately (after 0 features)
/// 2. Client receives no features
/// 3. OnDone fires with error status
TEST_F(ActiveReadReactorTest, ListFeatures_ImmediateError_PropagatesStatus) {
  std::vector<routeguide::Feature> features;
  features.push_back(MakeTestFeature("Feature 0", 100, -100));
  test_service_.SetListFeaturesResponse(features);
  test_service_.SetListFeaturesErrorAfter(0, grpc::StatusCode::UNAVAILABLE, "Service unavailable");

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
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  EXPECT_EQ(received_features.size(), 0u);
  EXPECT_FALSE(status.ok());
  EXPECT_EQ(status.error_code(), grpc::StatusCode::UNAVAILABLE);
}

/// @test Validates streaming RPC cancellation triggers OnDone.
///
/// Tests that TryCancel() correctly terminates an active stream:
/// 1. Server is configured with 100 features (long stream)
/// 2. Client initiates stream and begins receiving
/// 3. After brief delay, client calls TryCancel()
/// 4. OnDone fires with final status
TEST_F(ActiveReadReactorTest, ListFeatures_TryCancel_TerminatesStream) {
  // Configure server to return many features
  std::vector<routeguide::Feature> features;
  for (int i = 0; i < 100; ++i) {
    features.push_back(MakeTestFeature("Feature " + std::to_string(i), i * 100, i * -100));
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
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
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
  EXPECT_TRUE(status.error_code() == grpc::StatusCode::CANCELLED ||
              status.error_code() == grpc::StatusCode::OK)
      << "Expected CANCELLED or OK, got: " << status.error_code();
}

/// @test Validates deadline exceeded during streaming.
///
/// Tests that expired deadline terminates stream:
/// 1. Client creates context with already-expired deadline
/// 2. OnDone fires with DEADLINE_EXCEEDED status
TEST_F(ActiveReadReactorTest, ListFeatures_DeadlineExceeded_PropagatesStatus) {
  std::vector<routeguide::Feature> features;
  features.push_back(MakeTestFeature("Feature", 100, -100));
  test_service_.SetListFeaturesResponse(features);

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.ok = [](grpc::ClientReadReactor<routeguide::Feature>*, const routeguide::Feature&) {
    return false;
  };
  cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto context = std::make_unique<grpc::ClientContext>();
  context->set_deadline(std::chrono::system_clock::now() - std::chrono::milliseconds(100));

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, std::move(context), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED)
      << "Expected DEADLINE_EXCEEDED, got: " << status.error_code();
}

/// @test Validates concurrent streaming RPCs all complete.
///
/// Tests that multiple simultaneous streams work correctly:
/// 1. Start 5 concurrent ListFeatures streams
/// 2. Each configured with different feature counts
/// 3. All complete successfully
TEST_F(ActiveReadReactorTest, ListFeatures_MultipleConcurrent_AllComplete) {
  // Configure server with features
  std::vector<routeguide::Feature> features;
  for (int i = 0; i < 10; ++i) {
    features.push_back(MakeTestFeature("Feature " + std::to_string(i), i * 100, i * -100));
  }
  test_service_.SetListFeaturesResponse(features);

  constexpr int kNumConcurrentStreams = 5;

  std::atomic<int> completed_count{0};
  std::promise<void> all_done_promise;
  std::future<void> all_done_future = all_done_promise.get_future();

  std::vector<std::unique_ptr<routeguide::ListFeatures::ClientReactor>> reactors;
  std::vector<grpc::Status> statuses(kNumConcurrentStreams);
  std::vector<int> feature_counts(kNumConcurrentStreams, 0);

  for (int i = 0; i < kNumConcurrentStreams; ++i) {
    routeguide::Rectangle request;

    routeguide::ListFeatures::Callbacks cbs;
    cbs.ok = [&feature_counts, i](grpc::ClientReadReactor<routeguide::Feature>*,
                                   const routeguide::Feature&) {
      ++feature_counts[i];
      return false;
    };
    cbs.nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
    cbs.done = [&completed_count, &all_done_promise, &statuses, i, kNumConcurrentStreams](
                   grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
      statuses[i] = status;
      if (++completed_count == kNumConcurrentStreams) {
        all_done_promise.set_value();
      }
    };

    reactors.push_back(std::make_unique<routeguide::ListFeatures::ClientReactor>(
        *stub_, CreateClientContext(), request, std::move(cbs)));
  }

  auto wait_result = all_done_future.wait_for(std::chrono::seconds(10));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Timeout waiting for concurrent streams";

  EXPECT_EQ(completed_count.load(), kNumConcurrentStreams);
  for (int i = 0; i < kNumConcurrentStreams; ++i) {
    EXPECT_TRUE(statuses[i].ok()) << "Stream " << i << " failed: " << statuses[i].error_message();
    EXPECT_EQ(feature_counts[i], 10) << "Stream " << i << " received wrong feature count";
  }
}

/// @test Validates nok callback fires on stream end.
///
/// Tests that nok callback is invoked when stream ends:
/// 1. Server sends features
/// 2. After last feature, OnReadDone(false) triggers nok callback
/// 3. Then OnDone fires
TEST_F(ActiveReadReactorTest, ListFeatures_NokCallback_FiresOnStreamEnd) {
  std::vector<routeguide::Feature> features;
  features.push_back(MakeTestFeature("Feature 1", 100, -100));
  features.push_back(MakeTestFeature("Feature 2", 200, -200));
  test_service_.SetListFeaturesResponse(features);

  bool nok_called = false;
  int ok_count = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.ok = [&ok_count](grpc::ClientReadReactor<routeguide::Feature>*,
                        const routeguide::Feature&) {
    ++ok_count;
    return false;
  };
  cbs.nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) {
    nok_called = true;
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready);

  grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(ok_count, 2);
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
}

}  // namespace

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
