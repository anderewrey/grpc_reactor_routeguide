///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/turn_by_turn_guidance.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"TurnByTurnGuidanceOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"TurnByTurnGuidanceOnReadDoneNOk"};
constexpr auto kOnDone{"TurnByTurnGuidanceOnDone"};
constexpr auto kOnDriverConfirmed{"TurnByTurnGuidanceOnDriverConfirmed"};

// How long the driver takes to confirm a stop. Longer than the server takes to send the next one, so
// the stream would run ahead of the driver if anything but the confirmation released it.
constexpr std::size_t kConfirmTimeMs{2};
}  // anonymous namespace

TurnByTurnGuidance::TurnByTurnGuidance(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_ok_(kOnReadDoneOk, [this](const EventLoop::Event* e) { OnReadOk(e); }),
      on_driver_confirmed_(kOnDriverConfirmed, [this](const EventLoop::Event* e) { OnDriverConfirmed(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

TurnByTurnGuidance::~TurnByTurnGuidance() {
  TearDownReactor(reactor_, logger_);
}

void TurnByTurnGuidance::Start(routeguide::Rectangle rect) {
  using ResponseT = routeguide::ListFeatures::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(rect));
    return;
  }

  Callbacks cbs;
  // (Point 2.6) TriggerEvent: OnReadDoneOk, handing the message ownership to the queue. The reactor
  // holds the RPC from here until ResumeRead(), so no further read is armed meanwhile.
  cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) {
    assert(!OnApplicationThread());  // gRPC thread
    const auto seq = ++read_seq_;
    // One stop in flight: every stop before this one was confirmed.
    assert(seq == confirmed_ + 1);
    logger_.info("read_ok  | #{} {}", seq, response->GetTypeName());
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

  // (Point 1.1) Create reactor
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), rect, std::move(cbs));
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

void TurnByTurnGuidance::OnReadOk(const EventLoop::Event* event) {
  // (Point 2.8) ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the message, not the reactor: reclaim the released ownership
  std::unique_ptr<routeguide::ListFeatures::ResponseT> response{
      static_cast<routeguide::ListFeatures::ResponseT*>(event->getData())};
  const auto seq = ++handled_seq_;
  // (Point 2.9) update application with response: the screen shows the stop, and keeps it, owned, until
  // the driver confirms it.
  logger_.info("RESPONSE | #{} {}: {}", seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  assert(!current_stop_);
  current_stop_ = std::move(response);
  EventLoop::TriggerEvent(kOnDriverConfirmed, kConfirmTimeMs);
}

void TurnByTurnGuidance::OnDriverConfirmed(const EventLoop::Event* /*event*/) {
  // ProceedEvent: the driver's confirmation, which carries no payload: the stop is the one on screen.
  assert(OnApplicationThread());
  assert(current_stop_);
  current_stop_.reset();
  const auto seq = ++confirmed_;
  // (Point 2.10, 2.11, 2.12) Delta against list_features_push_turn_by_turn.cpp: the next read is armed
  // from this later event, not from the handler that received the stop. The hold OnReadDone() took keeps
  // the stream stalled meanwhile, for as long as the driver takes.
  assert(reactor_);
  // Logged before the call, not after. ResumeRead() issues StartRead() internally, so a server with
  // the next message ready can deliver it and log read_ok #k+1 before this thread reaches its next
  // statement. Reporting the resume first is what keeps the transcript's order the same as the
  // causal order, which is the whole point of the mode being observable.
  logger_.info("resumed  | #{} driver confirmed, arming next read", seq);
  if (!reactor_->ResumeRead()) {
    logger_.info("resumed  | #{} no read armed, stream is over", seq);
  }
}

void TurnByTurnGuidance::OnReadNOk(const EventLoop::Event* event) {
  // (Point 4.7) ProceedEvent: OnReadDoneNOk
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.8) update application
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void TurnByTurnGuidance::OnDone(const EventLoop::Event* event) {
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

}  // namespace rg_demo::usecase
