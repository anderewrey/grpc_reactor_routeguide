///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///
/// ActiveWriteReactor Test Suite, Submission::kReactorPulls
///
/// Tests the client-streaming (write) reactor when the reactor pulls each request from the application's
/// next_request callback, using the RecordRoute RPC. The application makes no write call at all: the
/// reactor pulls the first request while it starts the RPC, then one more after each successful write,
/// and closes the request stream when the callback has none. It takes no hold, so the RPC must end on its
/// own however it ends, which is what most tests here check.
///
/// Submission is independent of Delivery, which the kApplicationPushes suites already cover both ways, so
/// this file uses Delivery::kPush only.
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <future>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace {

/// Every reactor in this file is instantiated with Submission::kReactorPulls; the application-pushes
/// counterparts are covered by active_write_reactor_pull_test.cpp and active_write_reactor_push_test.cpp.
constexpr auto kDelivery = RpcReactor::Client::Delivery::kPush;
constexpr auto kSubmission = RpcReactor::Client::Submission::kReactorPulls;

using Reactor = routeguide::RecordRoute::ClientReactor<kDelivery, kSubmission>;
using Callbacks = routeguide::RecordRoute::Callbacks<kDelivery, kSubmission>;
using PushingReactor =
    routeguide::RecordRoute::ClientReactor<kDelivery, RpcReactor::Client::Submission::kApplicationPushes>;
using PushingCallbacks =
    routeguide::RecordRoute::Callbacks<kDelivery, RpcReactor::Client::Submission::kApplicationPushes>;

// =============================================================================
// Compile-time API checks
// =============================================================================

/// Whether `ReactorT` offers the application's write calls.
template <class ReactorT>
concept HasWriteCalls = requires(ReactorT& reactor, routeguide::Point point) {
  { reactor.SendRequest(std::move(point)) } -> std::same_as<bool>;
  { reactor.SendLastRequest(std::move(point)) } -> std::same_as<bool>;
  { reactor.CloseRequestStream() } -> std::same_as<bool>;
};

/// The reactor issues every write itself, so the application has no write call to make.
static_assert(!HasWriteCalls<Reactor>);
static_assert(HasWriteCalls<PushingReactor>);
/// The next_request slot is a callback under kReactorPulls, and an empty placeholder under
/// kApplicationPushes, which has nobody to call it.
static_assert(std::is_same_v<decltype(Callbacks::next_request),
                             std::function<std::optional<routeguide::Point>(
                                 grpc::ClientWriteReactor<routeguide::Point>*)>>);
static_assert(std::is_same_v<decltype(PushingCallbacks::next_request), std::monostate>);

/// Test service implementing RecordRoute: counts the points it reads, and can end every RPC as soon as
/// it starts, without reading anything.
class TestRouteGuideService final : public routeguide::RouteGuide::CallbackService {
 public:
  /// Makes the server end every RecordRoute RPC with this status as soon as it starts.
  void SetEndEarly(grpc::StatusCode code, const std::string& message) {
    end_early_status_ = grpc::Status(code, message);
    end_early_ = true;
  }

  grpc::ServerReadReactor<routeguide::Point>* RecordRoute(grpc::CallbackServerContext* /*context*/,
                                                          routeguide::RouteSummary* summary) override {
    class RecordRouteReactor : public grpc::ServerReadReactor<routeguide::Point> {
     public:
      RecordRouteReactor(routeguide::RouteSummary* summary, bool end_early, const grpc::Status& end_early_status)
          : summary_(summary) {
        if (end_early) {
          Finish(end_early_status);
          return;
        }
        StartRead(&point_);
      }

      void OnReadDone(bool ok) override {
        if (ok) {
          ++point_count_;
          StartRead(&point_);
          return;
        }
        summary_->set_point_count(point_count_);
        Finish(grpc::Status::OK);
      }

      void OnDone() override { delete this; }

     private:
      routeguide::RouteSummary* summary_;
      routeguide::Point point_;
      int point_count_ = 0;
    };

    return new RecordRouteReactor(summary, end_early_, end_early_status_);
  }

 private:
  bool end_early_ = false;
  grpc::Status end_early_status_;
};

/// What a test's callbacks observed, read once the done promise is ready.
struct ProducerResult {
  grpc::Status status;
  routeguide::RouteSummary summary;
};

class ActiveWriteReactorProducerTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {
 protected:
  /// Binds a done slot that completes `promise` with what OnDone() received.
  static void BindDone(Callbacks& cbs, std::promise<ProducerResult>& promise) {
    cbs.done = [&promise](grpc::ClientWriteReactor<routeguide::Point>*, const grpc::Status& status,
                          std::unique_ptr<routeguide::RouteSummary> summary) {
      promise.set_value(ProducerResult{status, std::move(*summary)});
    };
  }
};

// =============================================================================
// Tests
// =============================================================================

/// @test The reactor pulls every request, then closes the request stream when next_request has none.
///
/// Verifies that:
/// - the server reads every point, and the RPC ends with OK, with no write call from the application
/// - next_request is called once per point, plus once for the std::nullopt that closes the stream
/// - write_done fires once per point, each before the pull that follows it
/// - the first pull runs on the thread that constructs the reactor
TEST_F(ActiveWriteReactorProducerTest, PullsEveryRequest_ThenClosesStream) {
  std::promise<ProducerResult> done_promise;
  auto done_future = done_promise.get_future();

  std::vector<routeguide::Point> source{rg_utils::MakePoint(407128000, -740060000),
                                        rg_utils::MakePoint(408000000, -740000000),
                                        rg_utils::MakePoint(409000000, -740000000)};
  std::size_t next = 0;
  int pull_count = 0;
  std::atomic<int> write_done_count{0};
  bool pulled_after_its_write_done = true;
  std::thread::id first_pull_thread;

  Callbacks cbs;
  cbs.next_request = [&](grpc::ClientWriteReactor<routeguide::Point>*) -> std::optional<routeguide::Point> {
    if (pull_count == 0) first_pull_thread = std::this_thread::get_id();
    // Every pull but the first follows the write_done of the write before it.
    if (write_done_count.load() != pull_count) pulled_after_its_write_done = false;
    ++pull_count;
    if (next == source.size()) return std::nullopt;
    return std::move(source[next++]);
  };
  cbs.write_done = [&](grpc::ClientWriteReactor<routeguide::Point>*, bool ok) {
    EXPECT_TRUE(ok);
    ++write_done_count;
  };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  const auto result = done_future.get();

  EXPECT_TRUE(result.status.ok()) << result.status.error_message();
  EXPECT_EQ(result.summary.point_count(), 3);
  EXPECT_EQ(pull_count, 4);
  EXPECT_EQ(write_done_count.load(), 3);
  EXPECT_TRUE(pulled_after_its_write_done);
  EXPECT_EQ(first_pull_thread, std::this_thread::get_id());
}

/// @test A next_request with nothing to send closes the request stream before the RPC starts.
///
/// Verifies that the RPC ends with OK and an empty summary, after a single pull and no write.
TEST_F(ActiveWriteReactorProducerTest, EmptySource_ClosesBeforeAnyWrite) {
  std::promise<ProducerResult> done_promise;
  auto done_future = done_promise.get_future();
  int pull_count = 0;
  std::atomic<int> write_done_count{0};

  Callbacks cbs;
  cbs.next_request = [&](grpc::ClientWriteReactor<routeguide::Point>*) -> std::optional<routeguide::Point> {
    ++pull_count;
    return std::nullopt;
  };
  cbs.write_done = [&](grpc::ClientWriteReactor<routeguide::Point>*, bool) { ++write_done_count; };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  const auto result = done_future.get();

  EXPECT_TRUE(result.status.ok()) << result.status.error_message();
  EXPECT_EQ(result.summary.point_count(), 0);
  EXPECT_EQ(pull_count, 1);
  EXPECT_EQ(write_done_count.load(), 0);
}

/// @test An unbound next_request slot counts as a source with nothing to send.
TEST_F(ActiveWriteReactorProducerTest, UnboundSlot_ClosesAtOnce) {
  std::promise<ProducerResult> done_promise;
  auto done_future = done_promise.get_future();

  Callbacks cbs;
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  const auto result = done_future.get();

  EXPECT_TRUE(result.status.ok()) << result.status.error_message();
  EXPECT_EQ(result.summary.point_count(), 0);
}

/// @test The server ends the RPC while the reactor is still pulling: OnDone() follows with no
/// application action, since the reactor holds nothing.
///
/// next_request never runs dry here, so only the failed write can stop the pulls. Verifies that:
/// - OnDone() reports the server's status
/// - exactly one write fails, and no pull follows it: one pull to start, and one per successful write
TEST_F(ActiveWriteReactorProducerTest, ServerEndsEarly_CompletesWithoutApplicationAction) {
  test_service_.SetEndEarly(grpc::StatusCode::FAILED_PRECONDITION, "Ended early");
  std::promise<ProducerResult> done_promise;
  auto done_future = done_promise.get_future();
  std::atomic<int> pull_count{0};
  std::atomic<int> ok_writes{0};
  std::atomic<int> failed_writes{0};

  Callbacks cbs;
  cbs.next_request = [&](grpc::ClientWriteReactor<routeguide::Point>*) -> std::optional<routeguide::Point> {
    ++pull_count;
    return rg_utils::MakePoint(407128000, -740060000);
  };
  cbs.write_done = [&](grpc::ClientWriteReactor<routeguide::Point>*, bool ok) { ++(ok ? ok_writes : failed_writes); };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  const auto result = done_future.get();

  EXPECT_EQ(result.status.error_code(), grpc::StatusCode::FAILED_PRECONDITION);
  EXPECT_EQ(failed_writes.load(), 1);
  EXPECT_EQ(pull_count.load(), ok_writes.load() + 1);
}

/// @test TryCancel() from write_done stops the pulls at once: the pull that would follow that write
/// never runs, and the RPC ends cancelled.
TEST_F(ActiveWriteReactorProducerTest, TryCancelFromWriteDone_StopsPulling) {
  std::promise<ProducerResult> done_promise;
  auto done_future = done_promise.get_future();
  std::atomic<int> pull_count{0};
  std::atomic<int> write_done_count{0};

  Callbacks cbs;
  cbs.next_request = [&](grpc::ClientWriteReactor<routeguide::Point>*) -> std::optional<routeguide::Point> {
    ++pull_count;
    return rg_utils::MakePoint(407128000, -740060000);
  };
  cbs.write_done = [&](grpc::ClientWriteReactor<routeguide::Point>* base_reactor, bool) {
    // The second write's completion cancels, through the reactor the callback was given.
    if (++write_done_count == 2) static_cast<Reactor*>(base_reactor)->TryCancel();
  };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));
  const auto result = done_future.get();

  EXPECT_EQ(result.status.error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(write_done_count.load(), 2);
  EXPECT_EQ(pull_count.load(), 2);
}

/// @test TryCancel() from the application thread ends an RPC whose source never runs dry.
TEST_F(ActiveWriteReactorProducerTest, TryCancelFromApplication_Completes) {
  std::promise<ProducerResult> done_promise;
  auto done_future = done_promise.get_future();
  std::promise<void> writing_promise;
  auto writing_future = writing_promise.get_future();
  std::atomic<int> write_done_count{0};

  Callbacks cbs;
  cbs.next_request = [](grpc::ClientWriteReactor<routeguide::Point>*) -> std::optional<routeguide::Point> {
    return rg_utils::MakePoint(407128000, -740060000);
  };
  cbs.write_done = [&](grpc::ClientWriteReactor<routeguide::Point>*, bool) {
    if (++write_done_count == 3) writing_promise.set_value();
  };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_EQ(writing_future.wait_for(std::chrono::seconds(5)), std::future_status::ready);
  reactor->TryCancel();
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  EXPECT_EQ(done_future.get().status.error_code(), grpc::StatusCode::CANCELLED);
}

}  // namespace
