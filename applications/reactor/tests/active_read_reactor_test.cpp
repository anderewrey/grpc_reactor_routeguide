///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///
/// ActiveReadReactor Test Suite
///
/// Tests the server-side streaming (read) reactor pattern using ListFeatures RPC.
/// Uses std::promise/future for synchronization.
///
/// The ok callback owns each received message, so these tests take ownership
/// there instead of pulling a response back from the reactor.
///
/// The test fixture creates:
/// - An in-process gRPC server with controllable ListFeatures responses
/// - Client reactors with callbacks that complete promises directly
/// - Configurable error injection for error path testing
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <future>
#include <memory>
#include <mutex>
#include <ostream>
#include <string>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace RpcReactor::Client {
/// Found by argument-dependent lookup, so GoogleTest names the pacing mode in the registered test
/// names and in failure messages instead of dumping the enum's bytes.
void PrintTo(const ReadPacing& pacing, std::ostream* os) {
  *os << (pacing == ReadPacing::kContinuous ? "Continuous" : "TurnByTurn");
}
}  // namespace RpcReactor::Client

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
class ActiveReadReactorTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {};

using routeguide::ListFeatures::ReadPacing;

/// Fixture for the behaviours that must hold identically under either read pacing mode. The mode
/// changes only who arms the next read, so everything an application observes (which messages
/// arrive, in what order, and which events fire) is expected to be the same under both.
class ActiveReadReactorPacingTest : public RouteGuideTestFixtureBase<TestRouteGuideService>,
                                    public testing::WithParamInterface<ReadPacing> {
 protected:
  /// Under kTurnByTurn the application owns the re-arm, so a consumer must resume once it is done with
  /// the message or the stream stays stalled. Under kContinuous the reactor has already re-armed and the
  /// call is a rejected no-op, which is what lets one callback body serve both modes.
  static void ResumeIfTurnByTurn(grpc::ClientReadReactor<routeguide::Feature>* reactor, const ReadPacing pacing) {
    if (pacing == ReadPacing::kTurnByTurn) {
      static_cast<routeguide::ListFeatures::ClientReactor*>(reactor)->ResumeRead();
    }
  }
};

/// Names the parameterized cases after the mode, so a failure report says which one broke.
std::string PacingName(const testing::TestParamInfo<ReadPacing>& info) {
  return info.param == ReadPacing::kContinuous ? "Continuous" : "TurnByTurn";
}

INSTANTIATE_TEST_SUITE_P(Pacing, ActiveReadReactorPacingTest,
                         testing::Values(ReadPacing::kContinuous, ReadPacing::kTurnByTurn),
                         PacingName);

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
/// - The above holds under both read pacing modes
TEST_P(ActiveReadReactorPacingTest, ListFeatures_MultipleResponses_ReceivesAll) {
  const ReadPacing pacing = GetParam();

  // Configure server to return multiple features
  std::vector<routeguide::Feature> expected_features;
  for (int i = 0; i < 5; ++i) {
    expected_features.push_back(
        rg_utils::MakeFeature("Feature " + std::to_string(i), 400000000 + i * 1000000, -740000000 + i * 1000000));
  }
  test_service_.SetListFeaturesResponse(expected_features);

  // Collect received features
  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  // Create request (bounding rectangle)
  routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  // Create callbacks - each message arrives owned by the ok callback
  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&received_features, pacing](grpc::ClientReadReactor<routeguide::Feature>* reactor,
                                std::unique_ptr<routeguide::Feature> response) {
    received_features.push_back(std::move(*response));
    ResumeIfTurnByTurn(reactor, pacing);
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Stream ended - no more reads
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  // Create reactor
  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs), pacing);

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

  routeguide::Rectangle request = rg_utils::MakeRectangle(0, 0, 0, 0);

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&read_count](grpc::ClientReadReactor<routeguide::Feature>*,
                         std::unique_ptr<routeguide::Feature>) {
    ++read_count;  // Should never be called for empty stream
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
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
  features.push_back(rg_utils::MakeFeature("Single Feature", 407128000, -740060000));
  test_service_.SetListFeaturesResponse(features);

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                std::unique_ptr<routeguide::Feature> response) {
    received_features.push_back(std::move(*response));
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
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
    features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), i * 100, i * -100));
  }
  test_service_.SetListFeaturesResponse(features);
  test_service_.SetListFeaturesErrorAfter(2, grpc::StatusCode::INTERNAL, "Mid-stream error");

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                std::unique_ptr<routeguide::Feature> response) {
    received_features.push_back(std::move(*response));
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
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
  features.push_back(rg_utils::MakeFeature("Feature 0", 100, -100));
  test_service_.SetListFeaturesResponse(features);
  test_service_.SetListFeaturesErrorAfter(0, grpc::StatusCode::UNAVAILABLE, "Service unavailable");

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                std::unique_ptr<routeguide::Feature> response) {
    received_features.push_back(std::move(*response));
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
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
    features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), i * 100, i * -100));
  }
  test_service_.SetListFeaturesResponse(features);

  std::atomic<int> read_count{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&read_count](grpc::ClientReadReactor<routeguide::Feature>*,
                         std::unique_ptr<routeguide::Feature>) {
    ++read_count;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
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

  EXPECT_TRUE(status.error_code() == grpc::StatusCode::CANCELLED ||
              status.error_code() == grpc::StatusCode::OK)
      << "Expected CANCELLED or OK, got: " << status.error_code();

  if (status.error_code() == grpc::StatusCode::CANCELLED) {
    // Cancel arrived before the stream finished: only part of it should have been received.
    EXPECT_LT(read_count.load(), 100);
  } else {
    // Cancel lost the race: the stream had already delivered everything.
    EXPECT_EQ(read_count.load(), 100);
  }
}

/// @test Validates deadline exceeded during streaming.
///
/// Tests that expired deadline terminates stream:
/// 1. Client creates context with already-expired deadline
/// 2. OnDone fires with DEADLINE_EXCEEDED status
TEST_F(ActiveReadReactorTest, ListFeatures_DeadlineExceeded_PropagatesStatus) {
  std::vector<routeguide::Feature> features;
  features.push_back(rg_utils::MakeFeature("Feature", 100, -100));
  test_service_.SetListFeaturesResponse(features);

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [](grpc::ClientReadReactor<routeguide::Feature>*, std::unique_ptr<routeguide::Feature>) {};
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
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
    features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), i * 100, i * -100));
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
    cbs.read_ok = [&feature_counts, i](grpc::ClientReadReactor<routeguide::Feature>*,
                                  std::unique_ptr<routeguide::Feature>) {
      ++feature_counts[i];
    };
    cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
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
  features.push_back(rg_utils::MakeFeature("Feature 1", 100, -100));
  features.push_back(rg_utils::MakeFeature("Feature 2", 200, -200));
  test_service_.SetListFeaturesResponse(features);

  bool nok_called = false;
  int ok_count = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&ok_count](grpc::ClientReadReactor<routeguide::Feature>*,
                       std::unique_ptr<routeguide::Feature>) {
    ++ok_count;
  };
  cbs.read_nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) {
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

/// @test Validates an unbound ok slot discards messages without stalling the stream.
///
/// The re-arm sits outside the `if (cbs_.read_ok)` guard, so a stream with no read_ok callback still drains:
///
/// 1. Server sends 5 features, none of which is handed anywhere
/// 2. OnReadDone(false) ends the stream, then OnDone fires with OK
///
/// The kTurnByTurn case matters most here: with no callback bound there is nobody to call ResumeRead(),
/// so the reactor must arm the read itself rather than take a hold that nothing would ever release.
TEST_P(ActiveReadReactorPacingTest, ListFeatures_NoReadOkCallbackBound_StreamStillDrains) {
  const ReadPacing pacing = GetParam();

  std::vector<routeguide::Feature> features;
  for (int i = 0; i < 5; ++i) {
    features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), 100 + i, -100 - i));
  }
  test_service_.SetListFeaturesResponse(features);

  bool nok_called = false;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks cbs;
  // cbs.read_ok deliberately left unbound.
  cbs.read_nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) {
    nok_called = true;
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                             const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs), pacing);

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready) << "Stream stalled with no ok callback bound";

  grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
}

/// @test Validates the stream completes while the consumer defers all processing.
///
/// The ok callback parks each owned message and calls nothing back into the reactor. This deadlocked
/// under the previous contract, where only GetResponse() issued the next read.
///
/// Verifies that:
/// - All 25 messages arrive with no consumer-side action at all
/// - The nok and done events still fire
/// - Every parked message still holds its own value, so none of them alias a shared read buffer
TEST_F(ActiveReadReactorTest, ListFeatures_DeferredConsumer_StreamCompletesWithoutConsumerAction) {
  constexpr int kFeatureCount = 25;
  std::vector<routeguide::Feature> expected_features;
  for (int i = 0; i < kFeatureCount; ++i) {
    expected_features.push_back(
        rg_utils::MakeFeature("Feature " + std::to_string(i), 400000000 + i * 1000, -740000000 + i * 1000));
  }
  test_service_.SetListFeaturesResponse(expected_features);

  // Parked messages: kept owned and untouched until the RPC is over.
  std::vector<std::unique_ptr<routeguide::Feature>> parked;
  bool nok_called = false;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&parked](grpc::ClientReadReactor<routeguide::Feature>*,
                     std::unique_ptr<routeguide::Feature> response) {
    parked.push_back(std::move(response));  // Defer everything: no processing, no reactor call
  };
  cbs.read_nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) {
    nok_called = true;
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                             const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  auto wait_result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(wait_result, std::future_status::ready)
      << "Stream stalled while the consumer deferred processing";

  grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
  ASSERT_EQ(parked.size(), static_cast<size_t>(kFeatureCount));
  for (int i = 0; i < kFeatureCount; ++i) {
    EXPECT_EQ(parked[i]->name(), expected_features[i].name()) << "Parked message " << i << " was overwritten";
  }
}

// =============================================================================
// ReadPacing::kTurnByTurn Specific Tests
// =============================================================================

/// @test Validates that kTurnByTurn bounds the stream to one message in flight.
///
/// This is the backpressure kContinuous deliberately gives up. The consumer never resumes from the
/// callback, so the test thread drives the stream one message at a time:
///
/// 1. Server is configured to send 5 features
/// 2. Each OnReadDone(true) takes a hold and arms nothing
/// 3. While that hold stands, no further message can arrive and the RPC cannot terminate
/// 4. Each ResumeRead() releases the hold and arms exactly one more read
///
/// Verifies that:
/// - The delivered count never runs ahead of the number of resumes
/// - OnDone stays pending for as long as a hold is outstanding
/// - The RPC completes with OK once the last read is armed
TEST_F(ActiveReadReactorTest, ListFeatures_TurnByTurn_StreamStallsUntilResumed) {
  constexpr int kFeatureCount = 5;
  std::vector<routeguide::Feature> expected_features;
  for (int i = 0; i < kFeatureCount; ++i) {
    expected_features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), 100 + i, -100 - i));
  }
  test_service_.SetListFeaturesResponse(expected_features);

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received](grpc::ClientReadReactor<routeguide::Feature>*,
                                         std::unique_ptr<routeguide::Feature>) {
    // No ResumeRead() here on purpose: the test thread owns the re-arm, which is what makes the
    // stall observable.
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++received;
    }
    cv.notify_one();
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs), ReadPacing::kTurnByTurn);

  for (int expected = 1; expected <= kFeatureCount; ++expected) {
    {
      std::unique_lock<std::mutex> lock(mutex);
      ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&received, expected] { return received == expected; }))
          << "Held stream did not deliver message " << expected;
    }
    // The hold is outstanding: gRPC has no read armed and OnDone is suppressed.
    EXPECT_EQ(done_future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout)
        << "RPC terminated while a read hold was outstanding";
    {
      std::lock_guard<std::mutex> lock(mutex);
      EXPECT_EQ(received, expected) << "Held stream delivered ahead of ResumeRead()";
    }
    EXPECT_TRUE(reactor->ResumeRead()) << "ResumeRead() rejected while a hold was outstanding";
  }

  ASSERT_EQ(done_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "Held stream never completed after the last resume";
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, kFeatureCount);
}

/// @test Validates a second ResumeRead() is rejected instead of underflowing the hold count.
///
/// gRPC counts holds and outstanding operations together, and fires OnDone when that count reaches
/// zero. A RemoveHold() with no matching AddHold() would drop the count early, so ResumeRead()
/// claims its flag before releasing anything:
///
/// 1. Server sends a single feature, so no further hold can ever be taken after the first resume
/// 2. The first ResumeRead() releases the hold and arms the read that ends the stream
/// 3. The second ResumeRead() finds no hold to release
///
/// Verifies that:
/// - The duplicate call reports rejection rather than acting
/// - The RPC still terminates once, with OK
TEST_F(ActiveReadReactorTest, ListFeatures_TurnByTurn_DuplicateResumeIsRejected) {
  std::vector<routeguide::Feature> features;
  features.push_back(rg_utils::MakeFeature("Only feature", 100, -100));
  test_service_.SetListFeaturesResponse(features);

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  std::atomic_int done_count{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received](grpc::ClientReadReactor<routeguide::Feature>*,
                                         std::unique_ptr<routeguide::Feature>) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++received;
    }
    cv.notify_one();
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise, &done_count](grpc::ClientReadReactor<routeguide::Feature>*,
                                          const grpc::Status& status) {
    if (++done_count == 1) done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs), ReadPacing::kTurnByTurn);

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&received] { return received == 1; }));
  }

  EXPECT_TRUE(reactor->ResumeRead()) << "First resume must release the hold";
  // Only one feature exists, so no new hold can appear between the two calls.
  EXPECT_FALSE(reactor->ResumeRead()) << "Second resume must find no hold to release";

  ASSERT_EQ(done_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, 1);
  EXPECT_EQ(done_count.load(), 1) << "Hold count underflowed into a repeated OnDone";
}

/// @test Validates ResumeRead() is a rejected no-op under kContinuous.
///
/// The mode is fixed at construction and kContinuous takes no hold, so the flag ResumeRead() claims is
/// never set and the call cannot disturb a stream the reactor is already driving itself.
///
/// Verifies that:
/// - Every ResumeRead() call reports rejection
/// - All messages still arrive exactly once and the RPC completes with OK
TEST_F(ActiveReadReactorTest, ListFeatures_Continuous_ResumeReadIsRejected) {
  constexpr int kFeatureCount = 5;
  std::vector<routeguide::Feature> expected_features;
  for (int i = 0; i < kFeatureCount; ++i) {
    expected_features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), 100 + i, -100 - i));
  }
  test_service_.SetListFeaturesResponse(expected_features);

  int received = 0;
  int resume_accepted = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  routeguide::ListFeatures::Callbacks cbs;
  cbs.read_ok = [&received, &resume_accepted](grpc::ClientReadReactor<routeguide::Feature>* reactor,
                                              std::unique_ptr<routeguide::Feature>) {
    ++received;
    if (static_cast<routeguide::ListFeatures::ClientReactor*>(reactor)->ResumeRead()) ++resume_accepted;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs), ReadPacing::kContinuous);

  ASSERT_EQ(done_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);

  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, kFeatureCount);
  EXPECT_EQ(resume_accepted, 0) << "ResumeRead() acted on a reactor that holds nothing";
}

}  // namespace
