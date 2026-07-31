///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/scenarios/list_features_turn_by_turn.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::turn_by_turn {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with its continuous
// counterpart: a read event carries an owned message, and two handlers reclaiming the same pointer
// would free it twice.
constexpr auto kOnReadDoneOk{"ListFeaturesTurnOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"ListFeaturesTurnOnReadDoneNOk"};
constexpr auto kOnDone{"ListFeaturesTurnOnDone"};
}  // anonymous namespace

ListFeatures::ListFeatures(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_ok_(kOnReadDoneOk, [this](const EventLoop::Event* e) { OnReadOk(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

void ListFeatures::Start(routeguide::Rectangle rect) {
  using Callbacks = routeguide::ListFeatures::Callbacks;
  using ClientReactor = routeguide::ListFeatures::ClientReactor;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using ResponseT = routeguide::ListFeatures::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(rect));
    return;
  }

  Callbacks cbs;
  // (Point 2.13) TriggerEvent: OnReadDoneOk, handing the message ownership to the queue. The reactor
  // holds the RPC from here until ResumeRead(), so no further read is armed meanwhile.
  cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) {
    assert(!OnApplicationThread());  // gRPC thread
    logger_.info("read_ok  | #{} {}", ++read_seq_, response->GetTypeName());
    EventLoop::TriggerEvent(kOnReadDoneOk, response.release());
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

  // (Point 1.1) Create reactor. Delta against the continuous counterpart: the pacing argument.
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), rect, std::move(cbs),
                                             ReadPacing::kTurnByTurn);
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

void ListFeatures::OnReadOk(const EventLoop::Event* event) {
  // (Point 2.8) ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the message, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::ListFeatures::ResponseT> response{
      static_cast<routeguide::ListFeatures::ResponseT*>(event->getData())};
  const auto seq = ++handled_seq_;
  // (Point 2.9) update application with response
  logger_.info("RESPONSE | #{} {}: {}", seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  // (Point 2.17, 2.18, 2.19) Delta against the continuous counterpart: arm the next read and release
  // the hold OnReadDone() took. Called from the application thread on purpose: resuming inline from
  // the gRPC callback is legal and erases the point of the mode.
  assert(reactor_);
  // Logged before the call, not after. ResumeRead() issues StartRead() internally, so a server with
  // the next message ready can deliver it and log read_ok #k+1 before this thread reaches its next
  // statement. Reporting the resume first is what keeps the transcript's order the same as the
  // causal order, which is the whole point of the mode being observable.
  logger_.info("resumed  | #{} arming next read", seq);
  if (!reactor_->ResumeRead()) {
    logger_.info("resumed  | #{} no read armed, stream is over", seq);
  }
}

void ListFeatures::OnReadNOk(const EventLoop::Event* event) {
  // (Point 4.7) ProceedEvent: OnReadDoneNOk
  assert(OnApplicationThread());
  const auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.8) update application
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void ListFeatures::OnDone(const EventLoop::Event* event) {
  // (Point 4.9) ProceedEvent: OnDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.10) update application with status
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  // (Point 4.11) Destroy reactor
  reactor_.reset();
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
}

}  // namespace rg_demo::turn_by_turn
