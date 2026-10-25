///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///
/// CancelAndWait() Test Suite
///
/// Tests tearing a reactor down while its RPC is still in flight. Destroying a reactor before its OnDone()
/// has run is a use-after-free: gRPC still calls OnDone(), and still reads the reactor's ClientContext.
/// CancelAndWait() cancels the RPC, releases every hold nobody is left to release, and waits for OnDone(),
/// after which the reactor may be destroyed. Each test leaves one kind of hold outstanding, the ones an
/// application tearing down would never release, and checks that CancelAndWait() still returns true.
///
/// The use-after-free itself is visible only under AddressSanitizer, which the sanitizer builds run.
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <utility>
#include <vector>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"
#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/tests/route_guide_test_fixture.h"

namespace {

using Delivery = RpcReactor::Client::Delivery;
using ReadPacing = RpcReactor::Client::ReadPacing;
using Submission = RpcReactor::Client::Submission;
using Bidi = grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>;

constexpr auto kTimeout = std::chrono::seconds(5);

/// A service whose RPCs stay in flight until the client acts:
/// - GetFeature answers only after a delay, from a thread of its own.
/// - ListFeatures sends a few features, then finishes.
/// - RecordRoute reads until the client closes its stream.
/// - RouteChat echoes each note back, until the client closes its stream.
class TeardownService final : public routeguide::RouteGuide::CallbackService {
 public:
  ~TeardownService() override { JoinAnswers(); }

  /// Waits for every delayed GetFeature answer, so none outlives the server.
  void JoinAnswers() {
    std::lock_guard lock(mutex_);
    for (auto& answer : answers_) answer.join();
    answers_.clear();
  }

  grpc::ServerUnaryReactor* GetFeature(grpc::CallbackServerContext* context, const routeguide::Point*,
                                       routeguide::Feature*) override {
    auto* reactor = context->DefaultReactor();
    std::lock_guard lock(mutex_);
    answers_.emplace_back([reactor] {
      std::this_thread::sleep_for(std::chrono::milliseconds(300));
      reactor->Finish(grpc::Status::OK);
    });
    return reactor;
  }

  grpc::ServerWriteReactor<routeguide::Feature>* ListFeatures(grpc::CallbackServerContext*,
                                                              const routeguide::Rectangle*) override {
    class Reactor : public grpc::ServerWriteReactor<routeguide::Feature> {
     public:
      Reactor() { Next(); }
      void OnWriteDone(bool ok) override {
        if (!ok) {
          Finish(grpc::Status(grpc::StatusCode::UNKNOWN, "write failed"));
          return;
        }
        Next();
      }
      void OnDone() override { delete this; }

     private:
      void Next() {
        if (sent_ == 5) {
          Finish(grpc::Status::OK);
          return;
        }
        feature_ = rg_utils::MakeFeature("feature", 1, sent_++);
        StartWrite(&feature_);
      }
      routeguide::Feature feature_;
      int sent_ = 0;
    };
    return new Reactor();
  }

  grpc::ServerReadReactor<routeguide::Point>* RecordRoute(grpc::CallbackServerContext*,
                                                          routeguide::RouteSummary*) override {
    class Reactor : public grpc::ServerReadReactor<routeguide::Point> {
     public:
      Reactor() { StartRead(&point_); }
      void OnReadDone(bool ok) override {
        if (ok) {
          StartRead(&point_);
          return;
        }
        Finish(grpc::Status::OK);
      }
      void OnDone() override { delete this; }

     private:
      routeguide::Point point_;
    };
    return new Reactor();
  }

  grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote>* RouteChat(
      grpc::CallbackServerContext*) override {
    class Reactor : public grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote> {
     public:
      Reactor() { StartRead(&note_); }
      void OnReadDone(bool ok) override {
        if (!ok) {
          Finish(grpc::Status::OK);
          return;
        }
        echo_ = note_;
        StartWrite(&echo_);
      }
      void OnWriteDone(bool ok) override {
        if (!ok) {
          Finish(grpc::Status(grpc::StatusCode::UNKNOWN, "write failed"));
          return;
        }
        StartRead(&note_);
      }
      void OnDone() override { delete this; }

     private:
      routeguide::RouteNote note_;
      routeguide::RouteNote echo_;
    };
    return new Reactor();
  }

 private:
  std::mutex mutex_;
  std::vector<std::thread> answers_;
};

class ReactorTeardownTest : public RouteGuideTestFixtureBase<TeardownService> {
 protected:
  void TearDown() override {
    test_service_.JoinAnswers();
    RouteGuideTestFixtureBase::TearDown();
  }
};

// =============================================================================
// ActiveUnaryReactor
// =============================================================================

/// @test A unary reactor torn down mid-RPC: CancelAndWait() returns once OnDone() has run, and only then is
/// the reactor destroyed. Destroying it without the call is the use-after-free AddressSanitizer reports.
TEST_F(ReactorTeardownTest, Unary_MidRpc_CancelAndWaitThenDestroy) {
  std::atomic<bool> done_ran{false};
  routeguide::GetFeature::Callbacks<Delivery::kPush> cbs;
  cbs.done = [&done_ran](auto*, const grpc::Status&, std::unique_ptr<routeguide::Feature>) { done_ran = true; };
  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor<Delivery::kPush>>(
      *stub_, CreateClientContext(), rg_utils::MakePoint(1, 1), std::move(cbs));

  ASSERT_TRUE(reactor->CancelAndWait(kTimeout)) << "OnDone() never ran after the cancel";
  EXPECT_TRUE(done_ran.load());
  EXPECT_EQ(reactor->Status().error_code(), grpc::StatusCode::CANCELLED);
  reactor.reset();
}

/// @test CancelAndWait() after OnDone() has already run returns true at once, and changes nothing.
TEST_F(ReactorTeardownTest, Unary_AfterOnDone_ReturnsAtOnce) {
  std::promise<void> done;
  routeguide::GetFeature::Callbacks<Delivery::kPush> cbs;
  cbs.done = [&done](auto*, const grpc::Status&, std::unique_ptr<routeguide::Feature>) { done.set_value(); };
  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor<Delivery::kPush>>(
      *stub_, CreateClientContext(), rg_utils::MakePoint(1, 1), std::move(cbs));
  ASSERT_EQ(done.get_future().wait_for(kTimeout), std::future_status::ready);

  EXPECT_TRUE(reactor->CancelAndWait(std::chrono::milliseconds(0)));
  EXPECT_TRUE(reactor->Status().ok());
}

/// @test Called from inside the done callback, CancelAndWait() returns at once rather than waiting for the
/// OnDone() it runs in.
TEST_F(ReactorTeardownTest, Unary_FromDoneCallback_ReturnsAtOnce) {
  using Reactor = routeguide::GetFeature::ClientReactor<Delivery::kPush>;
  std::promise<bool> result;
  routeguide::GetFeature::Callbacks<Delivery::kPush> cbs;
  cbs.done = [&result](auto* base, const grpc::Status&, std::unique_ptr<routeguide::Feature>) {
    result.set_value(static_cast<Reactor*>(base)->CancelAndWait(std::chrono::milliseconds(0)));
  };
  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), rg_utils::MakePoint(1, 1), std::move(cbs));
  auto future = result.get_future();
  ASSERT_EQ(future.wait_for(kTimeout), std::future_status::ready);

  EXPECT_TRUE(future.get());
}

// =============================================================================
// ActiveReadReactor
// =============================================================================

/// @test A stream reader holding a message nobody will release: the hold keeps OnDone() from running, so
/// CancelAndWait() must release it. Under kPull with kTurnByTurn, the message waits for a GetResponse() that
/// never comes.
TEST_F(ReactorTeardownTest, Read_PullHeldMessageNeverPulled) {
  std::promise<void> held;
  routeguide::ListFeatures::Callbacks<Delivery::kPull, ReadPacing::kTurnByTurn> cbs;
  cbs.read_ok = [&held](auto*, const routeguide::Feature&) { held.set_value(); };
  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kTurnByTurn>>(
      *stub_, CreateClientContext(), rg_utils::MakeRectangle(0, 0, 1, 1), std::move(cbs));
  ASSERT_EQ(held.get_future().wait_for(kTimeout), std::future_status::ready);

  EXPECT_TRUE(reactor->CancelAndWait(kTimeout)) << "The held message's hold kept OnDone() from running";
}

/// @test The same under kPush with kPerMessage, for a message read_ok kept and never resumed.
TEST_F(ReactorTeardownTest, Read_PushKeptMessageNeverResumed) {
  std::promise<void> held;
  routeguide::ListFeatures::Callbacks<Delivery::kPush> cbs;
  cbs.read_ok = [&held](auto*, std::unique_ptr<routeguide::Feature>) {
    held.set_value();
    return true;  // kept, and never resumed
  };
  auto reactor = std::make_unique<routeguide::ListFeatures::ClientReactor<Delivery::kPush>>(
      *stub_, CreateClientContext(), rg_utils::MakeRectangle(0, 0, 1, 1), std::move(cbs));
  ASSERT_EQ(held.get_future().wait_for(kTimeout), std::future_status::ready);

  EXPECT_TRUE(reactor->CancelAndWait(kTimeout)) << "The kept message's hold kept OnDone() from running";
}

// =============================================================================
// ActiveWriteReactor
// =============================================================================

/// @test A writer under kApplicationPushes whose write side is idle and never closed: the write-flow hold
/// keeps OnDone() from running, so CancelAndWait() must end the flow.
TEST_F(ReactorTeardownTest, Write_IdleWriteSideNeverClosed) {
  std::promise<void> written;
  routeguide::RecordRoute::Callbacks<Delivery::kPull, Submission::kApplicationPushes> cbs;
  cbs.write_done = [&written](auto*, bool) { written.set_value(); };
  auto reactor =
      std::make_unique<routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kApplicationPushes>>(
          *stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(reactor->SendRequest(rg_utils::MakePoint(1, 1)));
  ASSERT_EQ(written.get_future().wait_for(kTimeout), std::future_status::ready);

  EXPECT_TRUE(reactor->CancelAndWait(kTimeout)) << "The write-flow hold kept OnDone() from running";
}

/// @test A writer under kReactorPulls whose source never runs dry: it holds nothing, and CancelAndWait()
/// stops the pulls.
TEST_F(ReactorTeardownTest, Write_ReactorPullsEndlessSource) {
  std::atomic<int> writes{0};
  routeguide::RecordRoute::Callbacks<Delivery::kPush, Submission::kReactorPulls> cbs;
  cbs.next_request = [](auto*) -> std::optional<routeguide::Point> { return rg_utils::MakePoint(1, 1); };
  cbs.write_done = [&writes](auto*, bool) { ++writes; };
  auto reactor = std::make_unique<routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kReactorPulls>>(
      *stub_, CreateClientContext(), std::move(cbs));
  while (writes.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  EXPECT_TRUE(reactor->CancelAndWait(kTimeout));
}

// =============================================================================
// ActiveBidiReactor
// =============================================================================

/// @test A bidi reactor under kApplicationPushes holding an echo nobody will resume: the read hold, and the
/// idle-window hold that only OnReadDone(false) would release, both keep OnDone() from running.
TEST_F(ReactorTeardownTest, Bidi_HeldEchoNeverResumed) {
  using Callbacks = routeguide::RouteChat::Callbacks<Delivery::kPush, Submission::kApplicationPushes,
                                                     ReadPacing::kTurnByTurn>;
  using Reactor = routeguide::RouteChat::ClientReactor<Delivery::kPush, Submission::kApplicationPushes,
                                                       ReadPacing::kTurnByTurn>;
  std::promise<void> held;
  Callbacks cbs;
  cbs.read_ok = [&held](Bidi*, std::unique_ptr<routeguide::RouteNote>) { held.set_value(); };
  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("echo me", 1, 1)));
  ASSERT_EQ(held.get_future().wait_for(kTimeout), std::future_status::ready);

  EXPECT_TRUE(reactor->CancelAndWait(kTimeout)) << "A read or idle-window hold kept OnDone() from running";
}

/// @test The same under kPull with kPerMessage, for an echo read_ok kept and never pulled.
TEST_F(ReactorTeardownTest, Bidi_KeptEchoNeverPulled) {
  using Callbacks = routeguide::RouteChat::Callbacks<Delivery::kPull, Submission::kApplicationPushes>;
  using Reactor = routeguide::RouteChat::ClientReactor<Delivery::kPull, Submission::kApplicationPushes>;
  std::promise<void> held;
  Callbacks cbs;
  cbs.read_ok = [&held](Bidi*, const routeguide::RouteNote&) {
    held.set_value();
    return true;  // kept, and never pulled
  };
  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(reactor->SendRequest(rg_utils::MakeRouteNote("echo me", 1, 1)));
  ASSERT_EQ(held.get_future().wait_for(kTimeout), std::future_status::ready);

  EXPECT_TRUE(reactor->CancelAndWait(kTimeout)) << "A read or idle-window hold kept OnDone() from running";
}

/// @test A bidi reactor under kReactorPulls whose source never runs dry, its read side reading on.
TEST_F(ReactorTeardownTest, Bidi_ReactorPullsEndlessSource) {
  using Callbacks = routeguide::RouteChat::Callbacks<Delivery::kPush, Submission::kReactorPulls,
                                                     ReadPacing::kContinuous>;
  using Reactor = routeguide::RouteChat::ClientReactor<Delivery::kPush, Submission::kReactorPulls,
                                                       ReadPacing::kContinuous>;
  std::atomic<int> writes{0};
  Callbacks cbs;
  cbs.next_request = [](Bidi*) -> std::optional<routeguide::RouteNote> {
    return rg_utils::MakeRouteNote("endless", 1, 1);
  };
  cbs.read_ok = [](Bidi*, std::unique_ptr<routeguide::RouteNote>) {};
  cbs.write_done = [&writes](Bidi*, bool) { ++writes; };
  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  while (writes.load() < 3) std::this_thread::sleep_for(std::chrono::milliseconds(1));

  EXPECT_TRUE(reactor->CancelAndWait(kTimeout));
}

}  // namespace
