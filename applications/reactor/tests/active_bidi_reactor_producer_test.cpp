///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///
/// ActiveBidiReactor Test Suite, Submission::kReactorPulls
///
/// Tests the bidirectional reactor when the reactor pulls each request from the application's
/// next_request callback, using the RouteChat RPC. The write side then takes no hold, and runs on its
/// own next to the read side, which keeps its ReadPacing.
///
/// Submission is independent of Delivery and ReadPacing, which the kApplicationPushes suites already
/// cover, so this file uses Delivery::kPull with the default ReadPacing::kPerMessage only.
///
/// @see /docs/testing.md for comprehensive test documentation

#include <gtest/gtest.h>

#include <grpc/grpc.h>

#include <atomic>
#include <chrono>
#include <concepts>
#include <future>
#include <map>
#include <memory>
#include <optional>
#include <semaphore>
#include <string>
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
/// counterparts are covered by active_bidi_reactor_pull_test.cpp and active_bidi_reactor_push_test.cpp.
constexpr auto kDelivery = RpcReactor::Client::Delivery::kPull;
constexpr auto kSubmission = RpcReactor::Client::Submission::kReactorPulls;

using Reactor = routeguide::RouteChat::ClientReactor<kDelivery, kSubmission>;
using Callbacks = routeguide::RouteChat::Callbacks<kDelivery, kSubmission>;
using PushingReactor =
    routeguide::RouteChat::ClientReactor<kDelivery, RpcReactor::Client::Submission::kApplicationPushes>;
using PushingCallbacks =
    routeguide::RouteChat::Callbacks<kDelivery, RpcReactor::Client::Submission::kApplicationPushes>;
using Bidi = grpc::ClientBidiReactor<routeguide::RouteNote, routeguide::RouteNote>;

// =============================================================================
// Compile-time API checks
// =============================================================================

/// Whether `ReactorT` offers the application's write calls.
template <class ReactorT>
concept HasWriteCalls = requires(ReactorT& reactor, routeguide::RouteNote note) {
  { reactor.SendRequest(std::move(note)) } -> std::same_as<bool>;
  { reactor.SendLastRequest(std::move(note)) } -> std::same_as<bool>;
  { reactor.CloseRequestStream() } -> std::same_as<bool>;
};

/// Whether `ReactorT` offers GetResponse(), which the Delivery and ReadPacing decide, not the Submission.
template <class ReactorT>
concept HasGetResponse = requires(ReactorT& reactor, routeguide::RouteNote& note) {
  { reactor.GetResponse(note) } -> std::same_as<bool>;
};

static_assert(!HasWriteCalls<Reactor>);
static_assert(HasWriteCalls<PushingReactor>);
static_assert(HasGetResponse<Reactor>);
static_assert(std::is_same_v<decltype(Callbacks::next_request),
                             std::function<std::optional<routeguide::RouteNote>(Bidi*)>>);
static_assert(std::is_same_v<decltype(PushingCallbacks::next_request), std::monostate>);

/// Test service implementing RouteChat: answers each note with every note received before at the same
/// location, and can finish the RPC after a given number of notes.
class TestRouteGuideService final : public routeguide::RouteGuide::CallbackService {
 public:
  /// Makes the server finish every RouteChat RPC with OK once it has read `max` notes.
  void SetMaxMessages(int max) { max_messages_ = max; }

  grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote>* RouteChat(
      grpc::CallbackServerContext* /*context*/) override {
    class RouteChatReactor : public grpc::ServerBidiReactor<routeguide::RouteNote, routeguide::RouteNote> {
     public:
      explicit RouteChatReactor(int max_messages) : max_messages_(max_messages) { StartRead(&incoming_); }

      void OnReadDone(bool ok) override {
        if (!ok || (max_messages_ > 0 && ++count_ >= max_messages_)) {
          Finish(grpc::Status::OK);
          return;
        }
        auto& seen = by_location_[{incoming_.location().latitude(), incoming_.location().longitude()}];
        pending_ = seen;
        seen.push_back(incoming_);
        SendNextOrRead();
      }

      void OnWriteDone(bool ok) override {
        if (!ok) {
          Finish(grpc::Status(grpc::StatusCode::INTERNAL, "Write failed"));
          return;
        }
        SendNextOrRead();
      }

      void OnDone() override { delete this; }

     private:
      void SendNextOrRead() {
        if (pending_.empty()) {
          StartRead(&incoming_);
          return;
        }
        outgoing_ = std::move(pending_.back());
        pending_.pop_back();
        StartWrite(&outgoing_);
      }

      int max_messages_;
      int count_ = 0;
      routeguide::RouteNote incoming_;
      routeguide::RouteNote outgoing_;
      std::vector<routeguide::RouteNote> pending_;
      std::map<std::pair<int, int>, std::vector<routeguide::RouteNote>> by_location_;
    };

    return new RouteChatReactor(max_messages_);
  }

 private:
  int max_messages_ = -1;  // -1 means no limit
};

class ActiveBidiReactorProducerTest : public RouteGuideTestFixtureBase<TestRouteGuideService> {
 protected:
  /// Binds a done slot that completes `promise` with the RPC's status.
  static void BindDone(Callbacks& cbs, std::promise<grpc::Status>& promise) {
    cbs.done = [&promise](Bidi*, const grpc::Status& status) { promise.set_value(status); };
  }
};

/// A source of `count` notes at one location, so the server answers the n-th with the n-1 before it.
class NoteSource {
 public:
  explicit NoteSource(int count) : count_(count) {}

  std::optional<routeguide::RouteNote> Next() {
    ++pull_count_;
    if (sent_ == count_) return std::nullopt;
    return rg_utils::MakeRouteNote("Note " + std::to_string(sent_++), 100, 200);
  }

  int pull_count() const { return pull_count_; }

 private:
  int count_;
  int sent_ = 0;
  int pull_count_ = 0;
};

// =============================================================================
// Tests
// =============================================================================

/// @test The reactor pulls every request and closes the request stream, while the read side reads on.
///
/// Three notes at one location draw 0 + 1 + 2 answers. Verifies that the RPC ends with OK once the
/// server finishes, with every answer read, after one pull per note plus the one that closes the stream.
TEST_F(ActiveBidiReactorProducerTest, PullsEveryRequest_ThenClosesStream) {
  std::promise<grpc::Status> done_promise;
  auto done_future = done_promise.get_future();
  NoteSource source(3);
  std::atomic<int> received{0};
  std::atomic<int> write_done_count{0};

  Callbacks cbs;
  cbs.next_request = [&source](Bidi*) { return source.Next(); };
  cbs.write_done = [&](Bidi*, bool ok) {
    EXPECT_TRUE(ok);
    ++write_done_count;
  };
  cbs.read_ok = [&received](Bidi*, const routeguide::RouteNote&) {
    ++received;
    return false;  // Read on
  };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const auto status = done_future.get();
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_EQ(received.load(), 3);
  EXPECT_EQ(write_done_count.load(), 3);
  EXPECT_EQ(source.pull_count(), 4);
}

/// @test A next_request with nothing to send closes the request stream before the RPC starts, and the
/// server, which then reads nothing, finishes it.
TEST_F(ActiveBidiReactorProducerTest, EmptySource_ClosesBeforeAnyWrite) {
  std::promise<grpc::Status> done_promise;
  auto done_future = done_promise.get_future();
  NoteSource source(0);
  std::atomic<bool> read_nok{false};

  Callbacks cbs;
  cbs.next_request = [&source](Bidi*) { return source.Next(); };
  cbs.read_nok = [&read_nok](Bidi*) { read_nok = true; };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  const auto status = done_future.get();
  EXPECT_TRUE(status.ok()) << status.error_message();
  EXPECT_TRUE(read_nok.load());
  EXPECT_EQ(source.pull_count(), 1);
}

/// @test The server finishes while the reactor is still pulling: OnDone() follows with no application
/// action, since the write side holds nothing, and the reactor stops pulling.
TEST_F(ActiveBidiReactorProducerTest, ServerFinishesFirst_CompletesWithoutApplicationAction) {
  test_service_.SetMaxMessages(2);
  std::promise<grpc::Status> done_promise;
  auto done_future = done_promise.get_future();
  std::atomic<int> pull_count{0};
  std::atomic<int> ok_writes{0};

  Callbacks cbs;
  cbs.next_request = [&](Bidi*) -> std::optional<routeguide::RouteNote> {
    ++pull_count;
    return rg_utils::MakeRouteNote("Endless", 100, 200);  // Never runs dry
  };
  cbs.write_done = [&ok_writes](Bidi*, bool ok) {
    if (ok) ++ok_writes;
  };
  cbs.read_ok = [](Bidi*, const routeguide::RouteNote&) { return false; };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  EXPECT_TRUE(done_future.get().ok());
  // One pull to start, then at most one per successful write: none after the stream ended.
  EXPECT_LE(pull_count.load(), ok_writes.load() + 1);
}

/// @test TryCancel() from write_done stops the pulls at once, and the RPC ends cancelled.
TEST_F(ActiveBidiReactorProducerTest, TryCancelFromWriteDone_StopsPulling) {
  std::promise<grpc::Status> done_promise;
  auto done_future = done_promise.get_future();
  std::atomic<int> pull_count{0};
  std::atomic<int> write_done_count{0};

  Callbacks cbs;
  cbs.next_request = [&](Bidi*) -> std::optional<routeguide::RouteNote> {
    ++pull_count;
    return rg_utils::MakeRouteNote("Endless", 100, 200);
  };
  cbs.write_done = [&](Bidi* base_reactor, bool) {
    if (++write_done_count == 2) static_cast<Reactor*>(base_reactor)->TryCancel();
  };
  cbs.read_ok = [](Bidi*, const routeguide::RouteNote&) { return false; };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  EXPECT_EQ(done_future.get().error_code(), grpc::StatusCode::CANCELLED);
  EXPECT_EQ(write_done_count.load(), 2);
  EXPECT_EQ(pull_count.load(), 2);
}

/// @test A held read stalls only the read side: the reactor keeps pulling and writing, and closes the
/// request stream, while the first answer is still held.
///
/// read_ok keeps every answer, so each one waits for GetResponse(). The test calls none until the
/// source has run dry, which proves the writes went on regardless, then pulls every answer out.
TEST_F(ActiveBidiReactorProducerTest, HeldRead_DoesNotStallPulls) {
  std::promise<grpc::Status> done_promise;
  auto done_future = done_promise.get_future();
  std::promise<void> source_dry_promise;
  auto source_dry_future = source_dry_promise.get_future();
  NoteSource source(3);
  std::counting_semaphore<> held{0};

  Callbacks cbs;
  cbs.next_request = [&](Bidi*) {
    auto note = source.Next();
    if (!note) source_dry_promise.set_value();
    return note;
  };
  cbs.read_ok = [&held](Bidi*, const routeguide::RouteNote&) {
    held.release();
    return true;  // Keep it held, for GetResponse()
  };
  BindDone(cbs, done_promise);

  auto reactor = std::make_unique<Reactor>(*stub_, CreateClientContext(), std::move(cbs));
  ASSERT_EQ(source_dry_future.wait_for(std::chrono::seconds(5)), std::future_status::ready)
      << "The pulls stalled behind the held read";

  // Three notes at one location draw 0 + 1 + 2 answers.
  int pulled_out = 0;
  for (; pulled_out < 3; ++pulled_out) {
    if (!held.try_acquire_for(std::chrono::seconds(5))) break;
    routeguide::RouteNote note;
    EXPECT_TRUE(reactor->GetResponse(note));
  }
  ASSERT_TRUE(AwaitRpc(done_future, [&] { reactor->TryCancel(); }));

  EXPECT_TRUE(done_future.get().ok());
  EXPECT_EQ(pulled_out, 3);
}

}  // namespace
