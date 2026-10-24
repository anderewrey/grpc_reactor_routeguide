///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///
/// ActiveReadReactor Test Suite
///
/// Tests the server-side streaming (read) reactor pattern using ListFeatures RPC.
/// Uses std::promise/future for synchronization.
///
/// The ok callback is lent each received message in place, so these tests copy it out, or pull it
/// back from the reactor through GetResponse() when the pacing holds it.
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

/// Every reactor in this file is instantiated with Delivery::kPull; its push counterpart is
/// covered by active_read_reactor_push_test.cpp.
constexpr auto kDelivery = RpcReactor::Client::Delivery::kPull;

using routeguide::ListFeatures::ReadPacing;

// =============================================================================
// Compile-time API checks
// =============================================================================

/// GetResponse() pulls a held message out, and kContinuous holds none, so it does not exist there.
static_assert(!pacing_test::HasGetResponse<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kContinuous>,
                                           routeguide::Feature>);
static_assert(pacing_test::HasGetResponse<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kTurnByTurn>,
                                          routeguide::Feature>);
static_assert(pacing_test::HasGetResponse<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>,
                                          routeguide::Feature>);
/// ResumeRead() is a Delivery::kPush method, so no pull reactor offers it, whatever its pacing.
static_assert(!pacing_test::HasResumeRead<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kTurnByTurn>>);
static_assert(!pacing_test::HasResumeRead<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>>);
/// The read_ok slot is lent the message, and returns the hold decision only under kPerMessage.
static_assert(std::is_same_v<
              routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kPerMessage>::OnReadDoneOkCallback,
              std::function<bool(grpc::ClientReadReactor<routeguide::Feature>*, const routeguide::Feature&)>>);
static_assert(std::is_same_v<
              routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kContinuous>::OnReadDoneOkCallback,
              std::function<void(grpc::ClientReadReactor<routeguide::Feature>*, const routeguide::Feature&)>>);

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

/// The pacing of master's bool read_ok contract, which the ActiveReadReactorTest cases written for it
/// keep: every message is held, unless read_ok returns false.
using PerMessageReactor = routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kPerMessage>;
using PerMessageCallbacks = routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kPerMessage>;

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

  /// Builds a read_ok callback that gives a copy of every lent message to `keep`, then consumes it the
  /// way the pacing requires: nothing more under kContinuous, where the reactor re-arms by itself and
  /// the next read overwrites the lent message, and a GetResponse() under kTurnByTurn and kPerMessage,
  /// where the callback keeps the message held. The copy is handed to `keep` before the release,
  /// because GetResponse() arms the next read, and the next read_ok could then run before this one
  /// returns. The pulled message must match the lent one, which checks GetResponse() hands out the
  /// message it holds.
  template <class KeepFn>
  static typename Callbacks::OnReadDoneOkCallback ConsumeEach(KeepFn keep) {
    return [keep]([[maybe_unused]] grpc::ClientReadReactor<routeguide::Feature>* reactor,
                  const routeguide::Feature& lent) {
      if constexpr (kPacing == ReadPacing::kContinuous) {
        keep(routeguide::Feature(lent));
      } else {
        const std::string lent_bytes = lent.SerializeAsString();
        keep(routeguide::Feature(lent));
        routeguide::Feature pulled;
        EXPECT_TRUE(static_cast<Reactor*>(reactor)->GetResponse(pulled)) << "GetResponse() rejected a held message";
        EXPECT_EQ(pulled.SerializeAsString(), lent_bytes) << "GetResponse() did not hand out the held message";
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
/// The held pacings matter most here: with no callback bound there is nobody to call GetResponse(),
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

/// @test Validates the hold is taken before the ok callback hands the response off.
///
/// Forces the order that broke when OnReadDone() called AddHold() only after the ok callback
/// returned: the callback hands each response to another thread, which runs GetResponse() (its
/// StartRead() and RemoveHold()) to completion before the callback returns. The callback then
/// lingers, giving gRPC time to finish the RPC if that early RemoveHold() released the reactor's
/// last outstanding operation. On the last message it did: OnDone() fired while the reaction was
/// still running, and the late AddHold() then reached an already destroyed call.
///
/// Verifies that:
/// - OnDone never fires while an ok callback is still running
/// - All features are received through GetResponse(), and the final status is OK
TYPED_TEST(ActiveReadReactorHeldPacingTest, ListFeatures_ResponseConsumedBeforeCallbackReturns_HoldsUntilReturn) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  const std::vector<routeguide::Feature> features = MakeFeatures(3);
  this->test_service_.SetListFeaturesResponse(features);

  // GetResponse() starts the next read before it returns, so the consumer of the next message can
  // run while the previous one is still storing its feature: guard the vector, and compare it
  // without relying on arrival order.
  std::vector<routeguide::Feature> received_features;
  std::mutex received_mutex;
  std::atomic<bool> done_fired{false};
  std::atomic<bool> done_inside_callback{false};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&received_features, &received_mutex, &done_fired, &done_inside_callback](
               grpc::ClientReadReactor<routeguide::Feature>* reactor, const routeguide::Feature&) {
    auto* client_reactor = static_cast<Reactor*>(reactor);
    std::thread consumer([client_reactor, &received_features, &received_mutex] {
      routeguide::Feature feature;
      if (client_reactor->GetResponse(feature)) {
        const std::lock_guard<std::mutex> lock(received_mutex);
        received_features.push_back(std::move(feature));
      }
    });
    consumer.join();
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    if (done_fired) done_inside_callback = true;
    return pacing_test::KeepHeld<kPacing>();  // Held: the response was already consumed through GetResponse() above
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

  EXPECT_FALSE(done_inside_callback) << "OnDone fired while the ok callback was still running";
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
/// The ok callback keeps every message and never pulls it, so the test thread drives the stream one
/// message at a time:
///
/// 1. Server is configured to send 5 features
/// 2. Each OnReadDone(true) takes a hold and arms nothing, and read_ok keeps it
/// 3. While that hold stands, no further message can arrive and the RPC cannot terminate
/// 4. Each GetResponse() hands the held message out, releases the hold, and arms exactly one more read
///
/// Verifies that:
/// - The delivered count never runs ahead of the number of GetResponse() calls
/// - OnDone stays pending for as long as a hold is outstanding
/// - Each GetResponse() returns the message just delivered, in order
/// - The RPC completes with OK once the last read is armed
TYPED_TEST(ActiveReadReactorHeldPacingTest, ListFeatures_StreamStallsUntilGetResponse) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  constexpr int kFeatureCount = 5;
  const std::vector<routeguide::Feature> features = MakeFeatures(kFeatureCount);
  this->test_service_.SetListFeaturesResponse(features);

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received](grpc::ClientReadReactor<routeguide::Feature>*, const routeguide::Feature&) {
    // No GetResponse() here on purpose: the test thread owns the re-arm, which is what makes the
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
      EXPECT_EQ(received, expected) << "Held stream delivered ahead of GetResponse()";
    }
    routeguide::Feature feature;
    EXPECT_TRUE(reactor->GetResponse(feature)) << "GetResponse() rejected while a hold was outstanding";
    EXPECT_EQ(feature.name(), features[expected - 1].name()) << "GetResponse() handed out the wrong message";
  }

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }))
      << "Held stream never completed after the last GetResponse()";
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, kFeatureCount);
}

/// @test Validates a second GetResponse() is rejected instead of underflowing the hold count.
///
/// GetResponse() claims its flag by compare-exchange before it swaps or releases anything, so only
/// one caller takes the message and releases the hold:
///
/// 1. Server sends a single feature, so no further hold can ever be taken after the first call
/// 2. The first GetResponse() hands the message out, releases the hold, and arms the read that ends
///    the stream
/// 3. The second GetResponse() finds nothing to claim
///
/// Verifies that:
/// - The duplicate call returns false and leaves its argument untouched
/// - The RPC still terminates once, with OK
TYPED_TEST(ActiveReadReactorHeldPacingTest, ListFeatures_DuplicateGetResponseIsRejected) {
  using Reactor = typename TestFixture::Reactor;
  constexpr ReadPacing kPacing = TestFixture::kPacing;

  this->test_service_.SetListFeaturesResponse(MakeFeatures(1));

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  std::atomic_int done_count{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  typename TestFixture::Callbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received](grpc::ClientReadReactor<routeguide::Feature>*, const routeguide::Feature&) {
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

  routeguide::Feature first;
  EXPECT_TRUE(reactor->GetResponse(first)) << "First GetResponse() must hand out the held message";
  EXPECT_EQ(first.name(), "Feature 0");
  // Only one feature exists, so no new hold can appear between the two calls.
  routeguide::Feature second;
  second.set_name("untouched");
  EXPECT_FALSE(reactor->GetResponse(second)) << "Second GetResponse() must find nothing to claim";
  EXPECT_EQ(second.name(), "untouched") << "A rejected GetResponse() swapped a message out anyway";

  ASSERT_TRUE(this->AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, 1);
  EXPECT_EQ(done_count.load(), 1) << "Hold count underflowed into a repeated OnDone";
}

// =============================================================================
// ReadPacing::kContinuous specific tests
// =============================================================================

/// @test Validates the stream completes with no GetResponse() at all, the callback copying each message.
///
/// Under kContinuous the reactor re-arms its read as the last statement of the reaction, into the same
/// storage it lent the callback. The callback therefore copies what it keeps, and never pulls:
///
/// 1. Server sends 10 features
/// 2. read_ok copies each lent message and records where it was lent from
///
/// Verifies that:
/// - All 10 features arrive, in order, with no application call into the reactor
/// - Every message was lent from the same reactor storage, so a kept reference would be overwritten
/// - The nok and done events fire, and the final status is OK
TEST_F(ActiveReadReactorTest, ListFeatures_Continuous_CompletesWithoutGetResponse) {
  const std::vector<routeguide::Feature> features = MakeFeatures(10);
  test_service_.SetListFeaturesResponse(features);

  std::vector<routeguide::Feature> received_features;
  std::vector<const routeguide::Feature*> lent_addresses;
  bool nok_called = false;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  routeguide::ListFeatures::Callbacks<kDelivery, ReadPacing::kContinuous> cbs;
  cbs.read_ok = [&received_features, &lent_addresses](grpc::ClientReadReactor<routeguide::Feature>*,
                                                      const routeguide::Feature& lent) {
    received_features.push_back(lent);  // Copy: the next read overwrites the lent message
    lent_addresses.push_back(&lent);
  };
  cbs.read_nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) { nok_called = true; };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor<kDelivery, ReadPacing::kContinuous>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Stream stalled without GetResponse()";

  const grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok()) << "Status: " << status.error_message();
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
  ASSERT_EQ(received_features.size(), features.size());
  for (size_t i = 0; i < features.size(); ++i) {
    EXPECT_EQ(received_features[i].name(), features[i].name()) << "Message " << i << " arrived out of order";
  }
  for (const auto* address : lent_addresses) {
    EXPECT_EQ(address, lent_addresses.front()) << "The lent message did not come from the reactor's own storage";
  }
}

// =============================================================================
// ReadPacing::kPerMessage: master's bool read_ok contract, each message declined
// =============================================================================
//
// These cases predate the ReadPacing axis, when every pull reactor had a bool read_ok. kPerMessage is
// that contract unchanged, so they run on it as written: each read_ok copies the lent message and
// returns false, which makes the reactor claim its hold back and read on by itself.


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
  expected_features.reserve(5);
  for (int i = 0; i < 5; ++i) {
    expected_features.push_back(
        rg_utils::MakeFeature("Feature " + std::to_string(i), 400000000 + (i * 1000000), -740000000 + (i * 1000000)));
  }
  test_service_.SetListFeaturesResponse(expected_features);

  // Collect received features
  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  // Create request (bounding rectangle)
  const routeguide::Rectangle request = rg_utils::MakeRectangle(0, -800000000, 500000000, 0);

  // Create callbacks - use false to let reactor auto-continue reads
  PerMessageCallbacks cbs;
  cbs.read_ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                 const routeguide::Feature& response) {
    received_features.push_back(response);
    return false;  // Don't hold - let reactor auto-continue to next read
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Stream ended - no more reads
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  // Create reactor
  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait for completion
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); })) << "Timeout waiting for stream completion";

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
TEST_F(ActiveReadReactorTest, ListFeatures_EmptyStream_CompletesSuccessfully) {
  test_service_.SetListFeaturesResponse({});

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();
  int read_count = 0;

  const routeguide::Rectangle request = rg_utils::MakeRectangle(0, 0, 0, 0);

  PerMessageCallbacks cbs;
  cbs.read_ok = [&read_count](grpc::ClientReadReactor<routeguide::Feature>*,
                          const routeguide::Feature&) {
    ++read_count;  // Should never be called for empty stream
    return false;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {
    // Expected: stream ends immediately
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

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
TEST_F(ActiveReadReactorTest, ListFeatures_SingleFeature_ReceivesOne) {
  std::vector<routeguide::Feature> features;
  features.push_back(rg_utils::MakeFeature("Single Feature", 407128000, -740060000));
  test_service_.SetListFeaturesResponse(features);

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  PerMessageCallbacks cbs;
  cbs.read_ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                 const routeguide::Feature& response) {
    received_features.push_back(response);
    return false;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

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
TEST_F(ActiveReadReactorTest, ListFeatures_ServerErrorMidStream_PropagatesStatus) {
  // Configure server to return 2 features then error
  std::vector<routeguide::Feature> features;
  features.reserve(5);
  for (int i = 0; i < 5; ++i) {
    features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), i * 100, i * -100));
  }
  test_service_.SetListFeaturesResponse(features);
  test_service_.SetListFeaturesErrorAfter(2, grpc::StatusCode::INTERNAL, "Mid-stream error");

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  PerMessageCallbacks cbs;
  cbs.read_ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                 const routeguide::Feature& response) {
    received_features.push_back(response);
    return false;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

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
TEST_F(ActiveReadReactorTest, ListFeatures_ImmediateError_PropagatesStatus) {
  std::vector<routeguide::Feature> features;
  features.push_back(rg_utils::MakeFeature("Feature 0", 100, -100));
  test_service_.SetListFeaturesResponse(features);
  test_service_.SetListFeaturesErrorAfter(0, grpc::StatusCode::UNAVAILABLE, "Service unavailable");

  std::vector<routeguide::Feature> received_features;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  PerMessageCallbacks cbs;
  cbs.read_ok = [&received_features](grpc::ClientReadReactor<routeguide::Feature>*,
                                 const routeguide::Feature& response) {
    received_features.push_back(response);
    return false;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

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
TEST_F(ActiveReadReactorTest, ListFeatures_TryCancel_TerminatesStream) {
  // Configure server to return many features
  std::vector<routeguide::Feature> features;
  features.reserve(100);
  for (int i = 0; i < 100; ++i) {
    features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), i * 100, i * -100));
  }
  test_service_.SetListFeaturesResponse(features);

  std::atomic<int> read_count{0};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  PerMessageCallbacks cbs;
  cbs.read_ok = [&read_count](grpc::ClientReadReactor<routeguide::Feature>*,
                          const routeguide::Feature&) {
    ++read_count;
    return false;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Cancel after a brief moment
  std::this_thread::sleep_for(std::chrono::milliseconds(1));
  reactor->TryCancel();

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

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
TEST_F(ActiveReadReactorTest, ListFeatures_DeadlineExceeded_PropagatesStatus) {
  std::vector<routeguide::Feature> features;
  features.push_back(rg_utils::MakeFeature("Feature", 100, -100));
  test_service_.SetListFeaturesResponse(features);

  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  PerMessageCallbacks cbs;
  cbs.read_ok = [](grpc::ClientReadReactor<routeguide::Feature>*, const routeguide::Feature&) {
    return false;
  };
  cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto context = std::make_unique<grpc::ClientContext>();
  context->set_deadline(std::chrono::system_clock::now() - std::chrono::milliseconds(100));

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, std::move(context), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

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
  features.reserve(10);
  for (int i = 0; i < 10; ++i) {
    features.push_back(rg_utils::MakeFeature("Feature " + std::to_string(i), i * 100, i * -100));
  }
  test_service_.SetListFeaturesResponse(features);

  constexpr int kNumConcurrentStreams = 5;

  std::atomic<int> completed_count{0};
  std::promise<void> all_done_promise;
  const std::future<void> all_done_future = all_done_promise.get_future();

  std::vector<std::unique_ptr<PerMessageReactor>> reactors;
  reactors.reserve(kNumConcurrentStreams);
  std::vector<grpc::Status> statuses(kNumConcurrentStreams);
  std::vector<int> feature_counts(kNumConcurrentStreams, 0);

  for (int i = 0; i < kNumConcurrentStreams; ++i) {
    const routeguide::Rectangle request;

    PerMessageCallbacks cbs;
    cbs.read_ok = [&feature_counts, i](grpc::ClientReadReactor<routeguide::Feature>*,
                                   const routeguide::Feature&) {
      ++feature_counts[i];
      return false;
    };
    cbs.read_nok = [](grpc::ClientReadReactor<routeguide::Feature>*) {};
    cbs.done = [&completed_count, &all_done_promise, &statuses, i, kNumConcurrentStreams](
                   grpc::ClientReadReactor<routeguide::Feature>*, const grpc::Status& status) {
      statuses[i] = status;
      if (++completed_count == kNumConcurrentStreams) {
        all_done_promise.set_value();
      }
    };

    reactors.push_back(std::make_unique<PerMessageReactor>(
        *stub_, CreateClientContext(), request, std::move(cbs)));
  }

  const auto cancel_all = [&] {
    for (auto& r : reactors) {
      r->TryCancel();
    }
  };
  ASSERT_TRUE(AwaitRpc(all_done_future, cancel_all, std::chrono::seconds(10)))
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
TEST_F(ActiveReadReactorTest, ListFeatures_NokCallback_FiresOnStreamEnd) {
  std::vector<routeguide::Feature> features;
  features.push_back(rg_utils::MakeFeature("Feature 1", 100, -100));
  features.push_back(rg_utils::MakeFeature("Feature 2", 200, -200));
  test_service_.SetListFeaturesResponse(features);

  bool nok_called = false;
  int ok_count = 0;
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  const routeguide::Rectangle request;

  PerMessageCallbacks cbs;
  cbs.read_ok = [&ok_count](grpc::ClientReadReactor<routeguide::Feature>*,
                        const routeguide::Feature&) {
    ++ok_count;
    return false;
  };
  cbs.read_nok = [&nok_called](grpc::ClientReadReactor<routeguide::Feature>*) {
    nok_called = true;
  };
  cbs.done = [&done_promise](grpc::ClientReadReactor<routeguide::Feature>*,
                              const grpc::Status& status) {
    done_promise.set_value(status);
  };

  auto reactor = std::make_unique<PerMessageReactor>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const grpc::Status status = done_future.get();

  EXPECT_TRUE(status.ok());
  EXPECT_EQ(ok_count, 2);
  EXPECT_TRUE(nok_called) << "nok callback should fire when stream ends";
}

// =============================================================================
// ReadPacing::kPerMessage specific tests: the callback's runtime hold decision
// =============================================================================

/// @test Validates a late decline cannot release the hold of the message that follows it.
///
/// Regression test for a claim that once identified a hold by a shared bool, as in the push file's test
/// of the same name. Once the application pulls message 0 itself, GetResponse() arms the read of message
/// 1, which can arrive and publish its own hold while message 0's callback still runs. When that callback
/// then returns false, the reactor's claim must leave message 1's hold alone. The shared flag let it take
/// message 1's hold, and re-arm a read into the storage still lent to message 1's callback. This test
/// orders the two callbacks explicitly:
///
/// 1. Message 0's consumer thread calls GetResponse(), which arms the read of message 1
/// 2. Message 0's callback waits until message 1's callback has started, then returns false
/// 3. Message 1's callback waits until message 0's has returned, then keeps its message by returning true
/// 4. The test thread checks the stream is held, then pulls message 1 with GetResponse()
///
/// Verifies that:
/// - Message 1 stays held after message 0's decline: OnDone stays pending and no further message arrives
/// - The application's GetResponse() for message 1 is accepted and hands out message 1
/// - All 3 features arrive and the RPC completes with OK
TEST_F(ActiveReadReactorTest, ListFeatures_PerMessage_DeclinedAfterGetResponse_LeavesNextHoldAlone) {
  test_service_.SetListFeaturesResponse(MakeFeatures(3));

  std::mutex mutex;
  std::condition_variable cv;
  int received = 0;
  bool first_returning = false;
  bool second_kept = false;
  std::atomic<bool> first_pulled{false};
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  PerMessageCallbacks cbs;
  cbs.read_ok = [&mutex, &cv, &received, &first_returning, &second_kept, &first_pulled](
                    grpc::ClientReadReactor<routeguide::Feature>* reactor, const routeguide::Feature&) {
    auto* client_reactor = static_cast<PerMessageReactor*>(reactor);
    std::unique_lock<std::mutex> lock(mutex);
    const int index = received++;
    cv.notify_all();
    if (index == 0) {
      lock.unlock();
      std::thread consumer([client_reactor, &first_pulled] {
        routeguide::Feature feature;
        first_pulled = client_reactor->GetResponse(feature) && feature.name() == "Feature 0";
      });
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

  auto reactor = std::make_unique<PerMessageReactor>(*stub_, CreateClientContext(), routeguide::Rectangle(),
                                                     std::move(cbs));

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
  EXPECT_TRUE(first_pulled) << "Message 0's own GetResponse() was rejected or handed out the wrong message";
  routeguide::Feature second;
  EXPECT_TRUE(reactor->GetResponse(second)) << "Message 0's decline released message 1's hold";
  EXPECT_EQ(second.name(), "Feature 1");

  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(received, 3);
}

}  // namespace
