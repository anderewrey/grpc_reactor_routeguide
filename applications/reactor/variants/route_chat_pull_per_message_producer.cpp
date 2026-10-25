///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/route_chat_pull_per_message_producer.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::pull::per_message::producer {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"RouteChatPullMsgProducerOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"RouteChatPullMsgProducerOnReadDoneNOk"};
constexpr auto kOnWriteDone{"RouteChatPullMsgProducerOnWriteDone"};
constexpr auto kOnDone{"RouteChatPullMsgProducerOnDone"};
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
  pending_ = std::move(notes);
  pulled_seq_ = 0;

  Callbacks cbs;
  // TriggerEvent: OnReadDoneOk, for kept notes only. Delta against the turn-by-turn counterpart: the
  // return value is the hold decision, taken for this note only, which makes the callback a filter.
  // Returning true keeps the note: the reactor holds the read side until GetResponse(), exactly as under
  // kTurnByTurn, and the event carries the reactor itself. Returning false declines it: the reactor reads
  // on at once into the same storage, so the lent note is gone when this returns, and no event may refer
  // to it.
  cbs.read_ok = [this](auto* reactor, const ResponseT& response) -> bool {
    assert(!OnApplicationThread());  // gRPC thread
    auto text = protobuf_utils::ToString(response);
    if (!seen_.insert(text).second) {
      // Declined: the reactor itself arms the next read and releases the hold.
      logger_.info("skipped  | #{} repeated note: {}", ++skipped_seq_, text);
      return false;
    }
    logger_.info("read_ok  | #{} {}", ++read_seq_, response.GetTypeName());
    EventLoop::TriggerEvent(kOnReadDoneOk, reactor);
    return true;
  };
  // TriggerEvent: OnReadDoneNOk
  cbs.read_nok = [](auto* reactor) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnReadDoneNOk, reactor);
  };
  // Delta against route_chat_pull_per_message.cpp: the reactor pulls each note, so this slot replaces the
  // application's SendNextNote() pump. It answers at once, from the queue, as the slot requires.
  cbs.next_request = [this](auto*) { return NextNote(); };
  // TriggerEvent: OnWriteDone. Only for the log: the reactor pulls the next note right after this.
  cbs.write_done = [](auto* reactor, bool) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnWriteDone, reactor);
  };
  // TriggerEvent: OnDone
  cbs.done = [](auto* reactor, const grpc::Status&) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnDone, reactor);
  };

  // (Point 1.1) Create reactor. Its constructor already pulled the first note, on this thread.
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), std::move(cbs));
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

std::optional<routeguide::RouteNote> RouteChat::NextNote() {
  // Runs on the application thread for the first pull, inside the reactor's constructor, and on a gRPC
  // thread for every later one.
  if (pending_.empty()) {
    logger_.info("next     | none left, the reactor closes the request stream");
    return std::nullopt;
  }
  auto note = std::move(pending_.front());
  pending_.erase(pending_.begin());
  logger_.info("next     | #{} {}", ++pulled_seq_, protobuf_utils::ToString(note));
  return note;
}

void RouteChat::OnReadOk(const EventLoop::Event* event) {
  // ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the reactor, which still holds the message
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  const auto seq = ++handled_seq_;
  // Logged before the call, and the reason is in list_features_push_turn_by_turn.cpp: GetResponse()
  // issues StartRead() internally, so read_ok #k+1 can be logged before this thread runs another
  // statement.
  logger_.info("pulled   | #{} arming next read", seq);
  // Extract the message, re-arm the read side and release the hold OnReadDone() took, all in one call.
  // Called for a kept note only: a declined one was never held, and a call for it could release the
  // next note's hold instead.
  routeguide::RouteChat::ResponseT response;
  if (!reactor_->GetResponse(response)) {
    logger_.info("pulled   | #{} no message ready", seq);
    return;
  }
  logger_.info("RESPONSE | #{} {}: {}", seq, response.GetTypeName(), protobuf_utils::ToString(response));
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
}

void RouteChat::OnDone(const EventLoop::Event* event) {
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  const auto status = reactor_->Status();
  logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor), status.ok(),
               status.error_message());
  // Every note was either kept or skipped.
  logger_.info("totals   | {} kept, {} skipped", handled_seq_, skipped_seq_.load());
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  reactor_.reset();
}

}  // namespace rg_demo::pull::per_message::producer
