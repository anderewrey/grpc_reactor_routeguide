///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/list_features_push_per_message.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::push::per_message {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"ListFeaturesPushMsgOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"ListFeaturesPushMsgOnReadDoneNOk"};
constexpr auto kOnDone{"ListFeaturesPushMsgOnDone"};
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
  // (Point 2.6, or 2.14 when not held) TriggerEvent: OnReadDoneOk, handing the message ownership to the
  // queue. Delta against the turn-by-turn counterpart: the return value is the hold decision, taken for
  // this message only, here by the arbitrary rule of holding every second message.
  // Returning true holds the RPC until ResumeRead(); returning false lets the reactor read on at once.
  // The message is queued either way, so its ownership always ends in exactly one OnReadOk(): only the
  // pace of the stream depends on the answer.
  cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) -> bool {
    assert(!OnApplicationThread());  // gRPC thread
    const auto seq = ++read_seq_;
    const bool hold = std::exchange(hold_next_, !hold_next_);
    logger_.info("read_ok  | #{} {}, {}", seq, response->GetTypeName(), hold ? "held" : "not held");
    // Recorded before the hand-off, so the handler that will release the hold cannot miss it.
    if (hold) held_seq_ = seq;
    EventLoop::TriggerEvent(kOnReadDoneOk, response.release());
    return hold;
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
  // (Point 2.8, or 2.18 when not held) ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the message, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::ListFeatures::ResponseT> response{
      static_cast<routeguide::ListFeatures::ResponseT*>(event->getData())};
  const auto seq = ++handled_seq_;
  // (Point 2.9, or 2.19 when not held) update application with response
  logger_.info("RESPONSE | #{} {}: {}", seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  // (Point 2.10, 2.11, 2.12) Delta against the turn-by-turn counterpart: a hold is released only by the
  // handler of the message read_ok held. The queue is FIFO, so every message read before it has been
  // handled by then.
  // Not called for a declined message, although ResumeRead() returns false when nothing is held.
  // Under kPerMessage the reactor takes a hold on every message before read_ok answers, so a
  // ResumeRead() issued meanwhile would claim that hold and arm the next read while read_ok still
  // runs. The two read_ok calls would then overlap, and the later one's event could be queued first,
  // which breaks the FIFO order every handler numbers its messages by.
  unsigned held = seq;
  if (!held_seq_.compare_exchange_strong(held, 0)) return;
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

}  // namespace rg_demo::push::per_message
