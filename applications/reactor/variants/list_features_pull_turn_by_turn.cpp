///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/list_features_pull_turn_by_turn.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::pull::turn_by_turn {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"ListFeaturesPullTurnOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"ListFeaturesPullTurnOnReadDoneNOk"};
constexpr auto kOnDone{"ListFeaturesPullTurnOnDone"};
}  // anonymous namespace

ListFeatures::ListFeatures(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_ok_(kOnReadDoneOk, [this](const EventLoop::Event* e) { OnReadOk(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

ListFeatures::~ListFeatures() {
  TearDownReactor(reactor_, logger_);
}

void ListFeatures::Start(const routeguide::Rectangle& rect) {
  using ResponseT = routeguide::ListFeatures::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(rect));
    return;
  }

  Callbacks cbs;
  // (Point 2.6) TriggerEvent: OnReadDoneOk. The message stays in the reactor, so the event carries the
  // reactor itself. The reactor holds the RPC from here until GetResponse(), so no further read is
  // armed meanwhile, and the lent message stays valid until it is pulled.
  cbs.read_ok = [this](auto* reactor, const ResponseT& response) {
    assert(!OnApplicationThread());  // gRPC thread
    logger_.info("read_ok  | #{} {}", ++read_seq_, response.GetTypeName());
    EventLoop::TriggerEvent(kOnReadDoneOk, reactor);
  };
  // (Point 4.3) TriggerEvent: OnReadDoneNOk
  cbs.read_nok = [](auto* reactor) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnReadDoneNOk, reactor);
  };
  // (Point 4.6) TriggerEvent: OnDone
  cbs.done = [](auto* reactor, const grpc::Status&) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnDone, reactor);
  };

  // (Point 1.1) Create reactor
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), rect, std::move(cbs));
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

void ListFeatures::OnReadOk(const EventLoop::Event* event) {
  // (Point 2.8) ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the reactor, which still holds the message
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  const auto seq = ++handled_seq_;
  // Logged before the call, for the reason given in list_features_push_turn_by_turn.cpp: GetResponse()
  // issues StartRead() internally, so read_ok #k+1 can be logged before this thread runs another
  // statement.
  logger_.info("pulled   | #{} arming next read", seq);
  // (Point 2.9 to 2.13) Delta against the push counterpart: extract the message, arm the next read
  // and release the hold OnReadDone() took, all in one call.
  routeguide::ListFeatures::ResponseT response;
  if (!reactor_->GetResponse(response)) {
    logger_.info("pulled   | #{} no message ready", seq);
    return;
  }
  // update application with response
  logger_.info("RESPONSE | #{} {}: {}", seq, response.GetTypeName(), protobuf_utils::ToString(response));
}

void ListFeatures::OnReadNOk(const EventLoop::Event* event) {
  // (Point 4.7) ProceedEvent: OnReadDoneNOk
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.8) update application
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void ListFeatures::OnDone(const EventLoop::Event* event) {
  // (Point 4.9) ProceedEvent: OnDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.10) update application with status
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  // (Point 4.11) Destroy reactor
  reactor_.reset();
}

}  // namespace rg_demo::pull::turn_by_turn
