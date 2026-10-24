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

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
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

/// Every reactor in this file is instantiated with Delivery::kPush; its pull counterpart is
/// covered by active_read_reactor_pull_test.cpp.
constexpr auto kDelivery = RpcReactor::Client::Delivery::kPush;

using routeguide::ListFeatures::ReadPacing;

// =============================================================================
// Compile-time API checks
// =============================================================================

/// GetResponse() is a Delivery::kPull method, so no push reactor offers it, whatever its pacing.
static_assert(!pacing_test::HasGetResponse<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kTurnByTurn>,
                                           routeguide::Feature>);
static_assert(!pacing_test::HasGetResponse<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>,
                                           routeguide::Feature>);
/// ResumeRead() releases a hold, and kContinuous never takes one, so it does not exist there.
static_assert(!pacing_test::HasResumeRead<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kContinuous>>);
static_assert(pacing_test::HasResumeRead<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kTurnByTurn>>);
static_assert(pacing_test::HasResumeRead<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>>);
/// The read_ok slot takes the message as owned, and returns the hold decision only under kPerMessage.
static_assert(std::is_same_v<
              routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kPerMessage>::OnReadDoneOkCallback,
              std::function<bool(grpc::ClientReadReactor<routeguide::Feature>*,
                                 std::unique_ptr<routeguide::Feature>)>>);
static_assert(std::is_same_v<
              routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kTurnByTurn>::OnReadDoneOkCallback,
              std::function<void(grpc::ClientReadReactor<routeguide::Feature>*,
                                 std::unique_ptr<routeguide::Feature>)>>);

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
      grpc::CallbackServerContext* /*context*/,
      const routeguide::Rectangle* /*request*/) override {
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

/// Test fixture with in-process server, for the tests pinned to one pacing
class ActiveReadReactorTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {};

/// Fixture for the behaviours that must hold identically under every read pacing. The pacing changes
/// only who arms the next read, so everything an application observes (which messages arrive, in what
/// order, and which events fire) is expected to be the same under all of them.
/// @tparam PacingTagT a pacing_test::PacingTag, selecting the reactor's ReadPacing
template <class PacingTagT>
class ActiveReadReactorPacingTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {
 protected:
  static constexpr ReadPacing kPacing = PacingTagT::value;
  using Reactor = routeguide::ListFeatures::ClientReactor<kDelivery, kPacing>;
  using Callbacks = routeguide::ListFeatures::Callbacks<kDelivery, kPacing>;

  /// Builds a read_ok callback that gives every owned message to `keep`, then consumes it the way the
  /// pacing requires: nothing more under kContinuous, where the reactor re-arms by itself, and a
  /// ResumeRead() under kTurnByTurn and kPerMessage, where the callback keeps the message held. The
  /// message is handed to `keep` before the release, because ResumeRead() arms the next read, and the
  /// next read_ok could then run before this one returns.
  template <class KeepFn>
  static typename Callbacks::OnReadDoneOkCallback ConsumeEach(KeepFn keep) {
    return [keep]([[maybe_unused]] grpc::ClientReadReactor<routeguide::Feature>* reactor,
                  std::unique_ptr<routeguide::Feature> message) {
      keep(std::move(*message));
      if constexpr (kPacing != ReadPacing::kContinuous) {
        // A read stream cannot end while the hold stands, so the release always arms the next read.
        EXPECT_TRUE(static_cast<Reactor*>(reactor)->ResumeRead()) << "ResumeRead() rejected a held message";
        return pacing_test::KeepHeld<kPacing>();
      }
    };
  }
};

TYPED_TEST_SUITE(ActiveReadReactorPacingTest, pacing_test::AllPacings);

/// Fixture for the behaviours of a held message, under the two pacings that hold one: kTurnByTurn
/// always, and kPerMessage when read_ok returns true.
/// @tparam PacingTagT a pacing_test::PacingTag, selecting kTurnByTurn or kPerMessage
template <class PacingTagT>
class ActiveReadReactorHeldPacingTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {
 protected:
  static constexpr ReadPacing kPacing = PacingTagT::value;
  using Reactor = routeguide::ListFeatures::ClientReactor<kDelivery, kPacing>;
  using Callbacks = routeguide::ListFeatures::Callbacks<kDelivery, kPacing>;
};

TYPED_TEST_SUITE(ActiveReadReactorHeldPacingTest, pacing_test::HeldPacings);

/// Builds the features the tests stream, named "Feature 0", "Feature 1", and so on.
std::vector<routeguide::Feature> MakeFeatures(int count) {
  std::vector<routeguide::Feature> features;
  features.reserve(count);
  for (int i = 0; i < count; ++i) {
    features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), 100 + i, -100 - i));
  }
  return features;
}

// =============================================================================
// Every pacing: ListFeatures server-side streaming behaviours
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
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_MultipleResponses_ReceivesAll) {
  using Reactor = typename TestFixture::Reactor;

  // Configure server to return multiple features
  std::vector<routeguide::Feature> expected_features;
  expected_features.reserve(5);
  for (int i = 0; i < 5; ++i) {
    expected_features.push_back(
        rg_utils::MakeFeature("Feature " + std::to_string(i), 400000000 + (i * 1000000), -740000000 + (i * 1000000)));
  }
  this->test_service_.SetListFeaturesResponse(expected_features);

  // Collect received features
  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  // Create request (bounding rectangle)
  const routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  // Create callbacks - each message arrives owned by the ok callback
  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&received_features](routeguide::Feature&& feature) {
    received_features.push_back(std::move(feature));
  });
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Stream ended - no more reads
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  // Create reactor
  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  // Wait for completion
  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for stream completion";

  const grpc::Status status = done_future.get();

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
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_EmptyStream_CompletesSuccessfully) {
  using Reactor = typename TestFixture::Reactor;

  this->test_service_.SetListFeaturesResponse({});

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();
  int read_count = 0;

  const routeguide::Rectangle request = rg_utils::MakeRectangle(0, 0, 0, 0);

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&read_count](routeguide::Feature&&) {
    ++read_count;  // Should never be called for empty stream
  });
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Expected: stream ends immediately
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

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
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_SingleFeature_ReceivesOne) {
  using Reactor = typename TestFixture::Reactor;

  std::vector<routeguide::Feature> features;
  features.push_back(rg_utils::MakeFeature("Single Feature", 407128000, -740060000));
  this->test_service_.SetListFeaturesResponse(features);

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&received_features](routeguide::Feature&& feature) {
    received_features.push_back(std::move(feature));
  });
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  ASSERT_EQ(received_features.size(), 1U);
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
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_ServerErrorMidStream_PropagatesStatus) {
  using Reactor = typename TestFixture::Reactor;

  // Configure server to return 2 features then error
  this->test_service_.SetListFeaturesResponse(MakeFeatures(5));
  this->test_service_.SetListFeaturesErrorAfter(2, grpc::StatusCode::INTERNAL, "Mid-stream error");

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&received_features](routeguide::Feature&& feature) {
    received_features.push_back(std::move(feature));
  });
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

  // Should have received 2 features before error
  EXPECT_EQ(received_features.size(), 2U);
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
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_ImmediateError_PropagatesStatus) {
  using Reactor = typename TestFixture::Reactor;

  this->test_service_.SetListFeaturesResponse(MakeFeatures(1));
  this->test_service_.SetListFeaturesErrorAfter(0, grpc::StatusCode::UNAVAILABLE, "Service unavailable");

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&received_features](routeguide::Feature&& feature) {
    received_features.push_back(std::move(feature));
  });
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

  EXPECT_EQ(received_features.size(), 0U);
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
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_TryCancel_TerminatesStream) {
  using Reactor = typename TestFixture::Reactor;

  // Configure server to return many features
  this->test_service_.SetListFeaturesResponse(MakeFeatures(100));

  std::atomic<int> read_count{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&read_count](routeguide::Feature&&) { ++read_count; });
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  // Cancel after a brief moment
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  reactor->TryCancel();

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

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
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_DeadlineExceeded_PropagatesStatus) {
  using Reactor = typename TestFixture::Reactor;

  this->test_service_.SetListFeaturesResponse(MakeFeatures(1));

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([](routeguide::Feature&&) {});
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto context = std::make_unique<grpc::ClientContext>();
  context->set_deadline(std::chrono::system_clock::now() - std::chrono::milliseconds(100));

  auto reactor = std::make_unique<Reactor>(*this->stub_, std::move(context), request, std::move(cbs));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

  EXPECT_EQ(status.error_code(), grpc::StatusCode::DEADLINE_EXCEEDED)
      << "Expected DEADLINE_EXCEEDED, got: " << status.error_code();
}

/// @test Validates concurrent streaming RPCs all complete.
///
/// Tests that multiple simultaneous streams work correctly:
/// 1. Start 5 concurrent ListFeatures streams
/// 2. Each receives the same 10 features
/// 3. All complete successfully
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_MultipleConcurrent_AllComplete) {
  using Reactor = typename TestFixture::Reactor;

  // Configure server with features
  this->test_service_.SetListFeaturesResponse(MakeFeatures(10));

  constexpr int kNumConcurrentStreams = 5;

  std::atomic<int> completed_count{0};
  std::promise<void> all_done_promise;
  const std::future<void> all_done_future = all_done_promise.get_future();

  std::vector<std::unique_ptr<Reactor>> reactors;
  reactors.reserve(kNumConcurrentStreams);
  std::vector<grpc::Status> statuses(kNumConcurrentStreams);
  std::vector<int> feature_counts(kNumConcurrentStreams, 0);

  for (int i = 0; i < kNumConcurrentStreams; ++i) {
    const routeguide::Rectangle request;

    typename TestFixture::Callbacks cbs;
    cbs.read_ok = TestFixture::ConsumeEach([&feature_counts, i](routeguide::Feature&&) { ++feature_counts[i]; });
    cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
    cbs.done = [&completed_count, &all_done_promise, &statuses, i](
                   grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
      statuses[i] = status;
      if (++completed_count == kNumConcurrentStreams) {
        all_done_promise.set_value();
      }
    };

    reactors.push_back(std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs)));
  }

  const auto cancel_all = [&] {
    for (auto& r : reactors) {
      r->TryCancel();
    }
  };
  ASSERT_TRUE(this->AwaitRpc(all_done_future, cancel_all, std::chrono::seconds(10)))
      << "Timeout waiting for concurrent streams";

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
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_NokCallback_FiresOnStreamEnd) {
  using Reactor = typename TestFixture::Reactor;

  this->test_service_.SetListFeaturesResponse(MakeFeatures(2));

  bool nok_called = false;
  int ok_count = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = TestFixture::ConsumeEach([&ok_count](routeguide::Feature&&) { ++ok_count; });
  cbs.read_nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) {
    nok_called = true;
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(ok_count, 2);
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
}

/// @test Validates an unbound ok slot discards messages without stalling the stream.
///
/// The reactor tests the slot before it takes any hold, so a stream with no read_ok callback still drains:
///
/// 1. Server sends 5 features, none of which is handed anywhere
/// 2. OnReadDone(false) ends the stream, then OnDone fires with OK
///
/// The held pacings matter most here: with no callback bound there is nobody to call ResumeRead(),
/// so the reactor must arm the read itself rather than take a hold that nothing would ever release.
TYPED_TEST(ActiveReadReactorPacingTest, ListFeatures_NoReadOkCallbackBound_StreamStillDrains) {
  using Reactor = typename TestFixture::Reactor;

  this->test_service_.SetListFeaturesResponse(MakeFeatures(5));

  bool nok_called = false;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  // cbs.read_ok deliberately left unbound.
  cbs.read_nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) {
    nok_called = true;
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                             const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Stream stalled with no ok callback bound";

  const grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
}

// =============================================================================
// Held pacings: kTurnByTurn, and kPerMessage with read_ok returning true
// =============================================================================

/// @test Validates the hold is taken before the read_ok callback hands the message off.
///
/// Forces the order that breaks if OnReadDone() calls AddHold() only after the read_ok callback
/// returns: the callback hands each message to another thread, which runs ResumeRead() (its
/// StartRead() and RemoveHold()) to completion before the callback returns. The callback then
/// lingers, giving gRPC time to finish the RPC if that early RemoveHold() released the reactor's last
/// outstanding operation. On the last message it would: OnDone() would fire while the reaction was
/// still running, and the late AddHold() would then reach an already destroyed call. Under kPerMessage
/// the callback then returns true, which leaves the release it already made as the only one.
///
/// Verifies that:
/// - OnDone never fires while a read_ok callback is still running
/// - Every ResumeRead() from the consumer thread is accepted
/// - All features are received, and the final status is OK
TYPED_TEST(ActiveReadReactorHeldPacingTest, ListFeatures_ResumedBeforeCallbackReturns_HoldsUntilReturn) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  const std::vector<routeguide::Feature> features = MakeFeatures(3);
  this->test_service_.SetListFeaturesResponse(features);

  // ResumeRead() arms the next read before it returns, so the consumer of the next message can run
  // while the previous one is still storing its feature: guard the vector, and compare it without
  // relying on arrival order.
  std::vector<routeguide::Feature> received_features;
  std::mutex received_mutex;
  std::atomic<bool> done_fired{false};
  std::atomic<bool> done_inside_callback{false};
  std::atomic<bool> resume_rejected{false};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&received_features, &received_mutex, &done_fired, &done_inside_callback, &resume_rejected](
                    grpc::ClientReadReactor<routeguide::Feature>* reactor,
                    std::unique_ptr<routeguide::Feature> response) {
    auto* client_reactor = static_cast<Reactor*>(reactor);
    std::thread consumer([client_reactor, &response, &received_features, &received_mutex, &resume_rejected] {
      {
        const std::lock_guard<std::mutex> lock(received_mutex);
        received_features.push_back(std::move(*response));
      }
      // The hold is outstanding and no read is armed, so the stream cannot have ended: the resume
      // must find the hold and arm the next read.
      if (!client_reactor->ResumeRead()) resume_rejected = true;
    });
    consumer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (done_fired) done_inside_callback = true;
    return pacing_test::KeepHeld<kPacing>();
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise, &done_fired](grpc::ClientReadReactor<routeguide::Feature>*,
                                          const grpc::Status& status) {
    done_fired = true;
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for stream completion";

  const grpc::Status status = done_future.get();

  EXPECT_FALSE(done_inside_callback) << "OnDone fired while the read_ok callback was still running";
  EXPECT_FALSE(resume_rejected) << "ResumeRead() rejected while a hold was outstanding";
  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  const std::lock_guard<std::mutex> lock(received_mutex);
  std::vector<std::string> received_names;
  received_names.reserve(received_features.size());
  for (const auto& feature : received_features) received_names.push_back(feature.name());
  std::ranges::sort(received_names);
  ASSERT_EQ(received_names.size(), features.size());
  for (size_t i = 0; i < features.size(); ++i) {
    EXPECT_EQ(received_names[i], features[i].name());
  }
}

/// @test Validates that a held message bounds the stream to one message in flight.
///
/// This is the backpressure kContinuous deliberately gives up. The consumer never resumes from the
/// callback, so the test thread drives the stream one message at a time:
///
/// 1. Server is configured to send 5 features
/// 2. Each OnReadDone(true) takes a hold and arms nothing, and read_ok keeps it
/// 3. While that hold stands, no further message can arrive and the RPC cannot terminate
/// 4. Each ResumeRead() releases the hold and arms exactly one more read
///
/// Verifies that:
/// - The delivered count never runs ahead of the number of resumes
/// - OnDone stays pending for as long as a hold is outstanding
/// - The RPC completes with OK once the last read is armed
TYPED_TEST(ActiveReadReactorHeldPacingTest, ListFeatures_StreamStallsUntilResumed) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  constexpr int kFeatureCount = 5;
  this->test_service_.SetListFeaturesResponse(MakeFeatures(kFeatureCount));

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received](grpc::ClientReadReactor<routeguide::Feature>*,
                                         std::unique_ptr<routeguide::Feature>) {
    // No ResumeRead() here on purpose: the test thread owns the re-arm, which is what makes the
    // stall observable.
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++received;
    }
    cv.notify_one();
    return pacing_test::KeepHeld<kPacing>();
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

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

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }))
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
TYPED_TEST(ActiveReadReactorHeldPacingTest, ListFeatures_DuplicateResumeIsRejected) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  this->test_service_.SetListFeaturesResponse(MakeFeatures(1));

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  std::atomic_int done_count{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received](grpc::ClientReadReactor<routeguide::Feature>*,
                                         std::unique_ptr<routeguide::Feature>) {
    {
      std::lock_guard<std::mutex> lock(mutex);
      ++received;
    }
    cv.notify_one();
    return pacing_test::KeepHeld<kPacing>();
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise, &done_count](grpc::ClientReadReactor<routeguide::Feature>*,
                                          const grpc::Status& status) {
    if (++done_count == 1) done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*this->stub_, this->CreateClientContext(), request, std::move(cbs));

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&received] { return received == 1; }));
  }

  EXPECT_TRUE(reactor->ResumeRead()) << "First resume must release the hold";
  // Only one feature exists, so no new hold can appear between the two calls.
  EXPECT_FALSE(reactor->ResumeRead()) << "Second resume must find no hold to release";

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, 1);
  EXPECT_EQ(done_count.load(), 1) << "Hold count underflowed into a repeated OnDone";
}

// =============================================================================
// ReadPacing::kContinuous specific tests
// =============================================================================

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

  const routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kContinuous> cbs;
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

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kContinuous>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }))
      << "Stream stalled while the consumer deferred processing";

  const grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
  ASSERT_EQ(parked.size(), static_cast<size_t>(kFeatureCount));
  for (int i = 0; i < kFeatureCount; ++i) {
    EXPECT_EQ(parked[i]->name(), expected_features[i].name()) << "Parked message " << i << " was overwritten";
  }
}

// =============================================================================
// ReadPacing::kPerMessage specific tests: the callback's runtime hold decision
// =============================================================================

/// @test Validates a declined message lets the stream read on with no ResumeRead().
///
/// Under kPerMessage the reactor takes a hold for every message, since it cannot know the answer
/// before calling the callback. A callback returning false declines it, and the reactor then claims
/// that hold back itself, re-arms, and releases it:
///
/// 1. Server sends 5 features
/// 2. read_ok keeps each owned message and returns false, never calling ResumeRead()
///
/// Verifies that:
/// - All 5 features arrive, in order, with no application call into the reactor
/// - The nok event fires and the RPC completes with OK, so no hold was left outstanding
TEST_F(ActiveReadReactorTest, ListFeatures_PerMessage_DeclinedMessagesReadOnWithoutResume) {
  const std::vector<routeguide::Feature> features = MakeFeatures(5);
  test_service_.SetListFeaturesResponse(features);

  std::vector<routeguide::Feature> received_features;
  bool nok_called = false;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kPerMessage> cbs;
  cbs.read_ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                     std::unique_ptr<routeguide::Feature> response) {
    received_features.push_back(std::move(*response));
    return false;  // Decline: the reactor reads on by itself
  };
  cbs.read_nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) { nok_called = true; };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Declined messages stalled the stream";

  const grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
  ASSERT_EQ(received_features.size(), features.size());
  for (size_t i = 0; i < features.size(); ++i) {
    EXPECT_EQ(received_features[i].name(), features[i].name());
  }
}

/// @test Validates a mix of kept and declined messages, decided per message, completes.
///
/// The decision is the callback's own, message by message. Here every third message is kept and the
/// others are declined, so the stream alternates between running on its own and waiting for the
/// application:
///
/// 1. Server sends 7 features, so messages 0, 3 and 6 are kept and the other four declined
/// 2. On each kept message the test thread checks the stream stalls, then calls ResumeRead()
/// 3. Declined messages flow through without any call from the test thread
///
/// Verifies that:
/// - A kept message stalls the stream and suppresses OnDone until ResumeRead()
/// - The stream runs on through declined messages until the next kept one
/// - All 7 features arrive and the RPC completes with OK
TEST_F(ActiveReadReactorTest, ListFeatures_PerMessage_MixedDecisions_HoldOnlyKeptMessages) {
  constexpr int kFeatureCount = 7;
  test_service_.SetListFeaturesResponse(MakeFeatures(kFeatureCount));

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kPerMessage> cbs;
  cbs.read_ok = [&mutex, &cv, &received](grpc::ClientReadReactor<routeguide::Feature>*,
                                         std::unique_ptr<routeguide::Feature>) {
    bool keep = false;
    {
      std::lock_guard<std::mutex> lock(mutex);
      keep = (received % 3) == 0;
      ++received;
    }
    cv.notify_one();
    return keep;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  for (const int kept_index : {0, 3, 6}) {
    const int expected = kept_index + 1;
    {
      std::unique_lock<std::mutex> lock(mutex);
      ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&received, expected] { return received == expected; }))
          << "Stream did not reach kept message " << kept_index;
    }
    EXPECT_EQ(done_future.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout)
        << "RPC terminated while kept message " << kept_index << " was held";
    {
      std::lock_guard<std::mutex> lock(mutex);
      EXPECT_EQ(received, expected) << "Stream delivered past kept message " << kept_index;
    }
    EXPECT_TRUE(reactor->ResumeRead()) << "ResumeRead() rejected kept message " << kept_index;
  }

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Stream never completed";
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, kFeatureCount);
}

/// @test Validates a declined message already resumed by the application is released only once.
///
/// The application may release the hold itself before its callback returns false, against its own
/// answer: here a consumer thread calls ResumeRead() and finishes before the callback declines. The
/// reactor's own claim then finds the hold already taken and must do nothing, since a second
/// StartRead() would break gRPC's one-read-in-flight rule and a second RemoveHold() would underflow
/// the hold count into an early OnDone.
///
/// Verifies that:
/// - Every ResumeRead() from the consumer thread is accepted
/// - OnDone fires exactly once, never while a read_ok callback is still running
/// - All features are received and the final status is OK, with nothing aborting
TEST_F(ActiveReadReactorTest, ListFeatures_PerMessage_DeclinedAfterResumeRead_ReleasesOnce) {
  const std::vector<routeguide::Feature> features = MakeFeatures(3);
  test_service_.SetListFeaturesResponse(features);

  // As in the hold-before-hand-off test, the resume lets the next message in while this callback still
  // runs: guard the vector, and compare it without relying on arrival order.
  std::vector<routeguide::Feature> received_features;
  std::mutex received_mutex;
  std::atomic<bool> done_fired{false};
  std::atomic<bool> done_inside_callback{false};
  std::atomic<bool> resume_rejected{false};
  std::atomic_int done_count{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  using Reactor = routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>;
  routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kPerMessage> cbs;
  cbs.read_ok = [&received_features, &received_mutex, &done_fired, &done_inside_callback, &resume_rejected](
                    grpc::ClientReadReactor<routeguide::Feature>* reactor,
                    std::unique_ptr<routeguide::Feature> response) {
    auto* client_reactor = static_cast<Reactor*>(reactor);
    std::thread consumer([client_reactor, &response, &received_features, &received_mutex, &resume_rejected] {
      {
        const std::lock_guard<std::mutex> lock(received_mutex);
        received_features.push_back(std::move(*response));
      }
      if (!client_reactor->ResumeRead()) resume_rejected = true;
    });
    // Joined before declining, so the application's release always comes first.
    consumer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    if (done_fired) done_inside_callback = true;
    return false;  // Declined, although the hold is already released: the reactor must not release it again
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise, &done_fired, &done_count](grpc::ClientReadReactor<routeguide::Feature>*,
                                                       const grpc::Status& status) {
    done_fired = true;
    if (++done_count == 1) done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for stream completion";

  const grpc::Status status = done_future.get();

  EXPECT_FALSE(done_inside_callback) << "OnDone fired while the read_ok callback was still running";
  EXPECT_FALSE(resume_rejected) << "ResumeRead() rejected while a hold was outstanding";
  EXPECT_EQ(done_count.load(), 1) << "Hold count underflowed into a repeated OnDone";
  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  const std::lock_guard<std::mutex> lock(received_mutex);
  std::vector<std::string> received_names;
  received_names.reserve(received_features.size());
  for (const auto& feature : received_features) received_names.push_back(feature.name());
  std::ranges::sort(received_names);
  ASSERT_EQ(received_names.size(), features.size());
  for (size_t i = 0; i < features.size(); ++i) {
    EXPECT_EQ(received_names[i], features[i].name());
  }
}

/// @test Validates a late decline cannot release the hold of the message that follows it.
///
/// Regression test for a claim that once identified a hold by a shared bool. Once the application
/// releases message 0 itself, message 1 can arrive and publish its own hold while message 0's callback
/// still runs. When that callback then returns false, the reactor's claim must find its own hold already
/// gone, and leave message 1's alone. The shared flag let it take message 1's hold instead, re-arming the
/// read before message 1's callback had decided anything. This test orders the two callbacks explicitly,
/// so the old defect reproduced on every run:
///
/// 1. Message 0's consumer thread calls ResumeRead(), which arms the read of message 1
/// 2. Message 0's callback waits until message 1's callback has started, then returns false
/// 3. Message 1's callback waits until message 0's has returned, then keeps its message by returning true
/// 4. The test thread checks the stream is held, then releases message 1 with ResumeRead()
///
/// Verifies that:
/// - Message 1 stays held after message 0's decline: OnDone stays pending and no further message arrives
/// - The application's ResumeRead() for message 1 finds the hold and is accepted
/// - All 3 features arrive and the RPC completes with OK
TEST_F(ActiveReadReactorTest, ListFeatures_PerMessage_DeclinedAfterResumeRead_LeavesNextHoldAlone) {
  test_service_.SetListFeaturesResponse(MakeFeatures(3));

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  bool first_returning = false;
  bool second_kept = false;
  std::atomic<bool> first_resumed{false};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  using Reactor = routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>;
  routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kPerMessage> cbs;
  cbs.read_ok = [&mutex, &cv, &received, &first_returning, &second_kept, &first_resumed](
                    grpc::ClientReadReactor<routeguide::Feature>* reactor, std::unique_ptr<routeguide::Feature>) {
    auto* client_reactor = static_cast<Reactor*>(reactor);
    std::unique_lock<std::mutex> lock(mutex);
    const int index = received++;
    cv.notify_all();
    if (index == 0) {
      lock.unlock();
      std::thread consumer([client_reactor, &first_resumed] { first_resumed = client_reactor->ResumeRead(); });
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
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), routeguide::Rectangle(), std::move(cbs));

  {
    std::unique_lock<std::mutex> lock(mutex);
    ASSERT_TRUE(cv.wait_for(lock, std::chrono::seconds(5), [&second_kept] { return second_kept; }))
        << "Message 1 never reached its callback";
  }
  EXPECT_EQ(done_future.wait_for(std::chrono::milliseconds(100)), std::future_status::timeout)
      << "RPC terminated while message 1 was kept";
  {
    std::lock_guard<std::mutex> lock(mutex);
    EXPECT_EQ(received, 2) << "Message 0's decline released message 1's hold, and the stream read on";
  }
  EXPECT_TRUE(first_resumed) << "Message 0's own ResumeRead() was rejected";
  EXPECT_TRUE(reactor->ResumeRead()) << "Message 0's decline released message 1's hold";

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, 3);
}
}  // namespace
