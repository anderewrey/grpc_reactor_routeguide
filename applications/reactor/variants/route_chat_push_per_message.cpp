///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/route_chat_push_per_message.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::push::per_message {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"RouteChatPushMsgOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"RouteChatPushMsgOnReadDoneNOk"};
constexpr auto kOnWriteDone{"RouteChatPushMsgOnWriteDone"};
constexpr auto kOnDone{"RouteChatPushMsgOnDone"};

// How many notes may wait for the application before read_ok holds the read side. Smaller than the
// ListFeatures namesake's bound, since a RouteChat stream carries only a few dozen notes, and the
// transcript should still show both answers. 1 would hold every note, as the turn-by-turn counterpart
// does.
constexpr unsigned kMaxInFlight{2};
}  // anonymous namespace

RouteChat::RouteChat(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_ok_(kOnReadDoneOk, [this](const EventLoop::Event* e) { OnReadOk(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_write_done_(kOnWriteDone, [this](const EventLoop::Event* e) { OnWriteDone(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

RouteChat::~RouteChat() {
  TearDownReactor(reactor_, logger_);
}

void RouteChat::Start(std::vector<routeguide::RouteNote> notes) {
  using ResponseT = routeguide::RouteChat::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring {} notes", fmt::ptr(reactor_.get()),
                 notes.size());
    return;
  }
  if (notes.empty()) {
    logger_.info("         | no notes to send, ignoring");
    return;
  }
  pending_ = std::move(notes);

  Callbacks cbs;
  // TriggerEvent: OnReadDoneOk, handing the message ownership to the queue. Delta against the
  // turn-by-turn counterpart: the return value is the hold decision, taken for this note only.
  // Returning true holds the read side until ResumeRead(); returning false lets the reactor read on at
  // once. The note is queued either way, so its ownership always ends in exactly one OnReadOk(): only
  // the pace of the read side depends on the answer.
  cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) -> bool {
    assert(!OnApplicationThread());  // gRPC thread
    // Counted before the hand-off, so the handler's decrement for this note can never run first.
    const auto in_flight = ++in_flight_;
    const auto seq = ++read_seq_;
    const bool hold = in_flight >= kMaxInFlight;
    logger_.info("read_ok  | #{} {}, {} in flight, {}", seq, response->GetTypeName(), in_flight,
                 hold ? "held" : "not held");
    // Recorded before the hand-off too, so the handler that will release the hold cannot miss it.
    if (hold) held_seq_ = seq;
    EventLoop::TriggerEvent(kOnReadDoneOk, response.release());
    return hold;
  };
  // TriggerEvent: OnReadDoneNOk
  cbs.read_nok = [](auto* reactor) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnReadDoneNOk, reactor);
  };
  // TriggerEvent: OnWriteDone
  cbs.write_done = [](auto* reactor, bool) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnWriteDone, reactor);
  };
  // TriggerEvent: OnDone
  cbs.done = [](auto* reactor, const grpc::Status&) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnDone, reactor);
  };

  // (Point 1.1) Create reactor
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), std::move(cbs));
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
  SendNextNote();
}

void RouteChat::OnReadOk(const EventLoop::Event* event) {
  // ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the message, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::RouteChat::ResponseT> response{
      static_cast<routeguide::RouteChat::ResponseT*>(event->getData())};
  const auto seq = ++handled_seq_;
  logger_.info("RESPONSE | #{} {}: {}", seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  // Done with the note: its in-flight slot is freed.
  --in_flight_;
  // Delta against the turn-by-turn counterpart: a hold is released only by the handler of the note
  // read_ok held, for the reason given in list_features_push_per_message.cpp: a ResumeRead() for a
  // declined note could claim the hold of the next one while its read_ok is still running. The queue is
  // FIFO, so every note read before the held one has been handled by then.
  unsigned held = seq;
  if (!held_seq_.compare_exchange_strong(held, 0)) return;
  assert(reactor_);
  // Logged before the call, and the reason is in list_features_push_turn_by_turn.cpp: ResumeRead()
  // issues StartRead() internally, so read_ok #k+1 can be logged before this thread runs another
  // statement.
  logger_.info("resumed  | #{} arming next read", seq);
  if (!reactor_->ResumeRead()) {
    logger_.info("resumed  | #{} no read armed, stream is over", seq);
  }
}

void RouteChat::OnReadNOk(const EventLoop::Event* event) {
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void RouteChat::OnWriteDone(const EventLoop::Event* event) {
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  if (!pending_.empty()) {
    SendNextNote();
  }
}

void RouteChat::OnDone(const EventLoop::Event* event) {
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  const auto status = reactor_->Status();
  logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor), status.ok(),
               status.error_message());
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  reactor_.reset();
}

void RouteChat::SendNextNote() {
  auto note = std::move(pending_.front());
  pending_.erase(pending_.begin());
  const bool last = pending_.empty();
  // A rejected write is a dead end for this pump: the note has already left the queue, and no
  // OnWriteDone will arrive to drive the next one. Report it rather than stalling in silence.
  const bool accepted = last ? reactor_->SendLastRequest(std::move(note)) : reactor_->SendRequest(std::move(note));
  if (!accepted) {
    logger_.info("         | write rejected, {} note(s) left unsent", pending_.size() + 1);
  }
}

}  // namespace rg_demo::push::per_message
