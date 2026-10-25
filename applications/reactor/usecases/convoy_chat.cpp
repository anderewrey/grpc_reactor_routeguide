///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/convoy_chat.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"ConvoyChatOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"ConvoyChatOnReadDoneNOk"};
constexpr auto kOnWriteDone{"ConvoyChatOnWriteDone"};
constexpr auto kOnDone{"ConvoyChatOnDone"};
constexpr auto kOnNoteTyped{"ConvoyChatOnNoteTyped"};

// A driver types a note every kTypingPeriodMs, except notes kTogetherFirst and kTogetherSecond, which two
// drivers type at the same moment: the second one waits for the first one's write.
constexpr std::size_t kTypingPeriodMs{10};
constexpr std::size_t kTogetherFirst{1};
constexpr std::size_t kTogetherSecond{2};
}  // anonymous namespace

ConvoyChat::ConvoyChat(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_ok_(kOnReadDoneOk, [this](const EventLoop::Event* e) { OnReadOk(e); }),
      on_note_typed_(kOnNoteTyped, [this](const EventLoop::Event* e) { OnNoteTyped(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_write_done_(kOnWriteDone, [this](const EventLoop::Event* e) { OnWriteDone(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

ConvoyChat::~ConvoyChat() {
  TearDownReactor(reactor_, logger_);
}

void ConvoyChat::Start(std::vector<routeguide::RouteNote> notes) {
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
  script_ = std::move(notes);
  pending_.clear();
  typed_ = 0;
  write_in_flight_ = false;

  Callbacks cbs;
  // TriggerEvent: OnReadDoneOk, handing the message ownership to the queue. The reactor re-arms the
  // read side as soon as this returns.
  cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) {
    assert(!OnApplicationThread());  // gRPC thread
    logger_.info("read_ok  | #{} {}", ++read_seq_, response->GetTypeName());
    EventLoop::TriggerEvent(kOnReadDoneOk, response.release());
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
  // Lets the drivers type: one timed event per note, delivered on this thread. Nothing is sent before
  // the first note is typed, and the read side runs meanwhile.
  for (std::size_t i = 0; i < script_.size(); ++i) {
    const std::size_t moment = i == kTogetherSecond ? kTogetherFirst : i;
    EventLoop::TriggerEvent(kOnNoteTyped, (moment + 1) * kTypingPeriodMs);
  }
}

void ConvoyChat::OnNoteTyped(const EventLoop::Event* /*event*/) {
  // ProceedEvent: a typed note, which carries no payload: the note is the script's next one.
  assert(OnApplicationThread());
  assert(reactor_ && !script_.empty());
  pending_.push_back(std::move(script_.front()));
  script_.erase(script_.begin());
  ++typed_;
  if (write_in_flight_) {
    logger_.info("typed    | note #{} queued, a write is in flight, {} waiting", typed_, pending_.size());
    return;
  }
  logger_.info("typed    | note #{} sent at once", typed_);
  SendNextNote();
}

void ConvoyChat::OnReadOk(const EventLoop::Event* event) {
  // ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the message, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::RouteChat::ResponseT> response{
      static_cast<routeguide::RouteChat::ResponseT*>(event->getData())};
  const auto seq = ++handled_seq_;
  // Shows the note in the chat
  logger_.info("RESPONSE | #{} {}: {}", seq, response->GetTypeName(), protobuf_utils::ToString(*response));
}

void ConvoyChat::OnReadNOk(const EventLoop::Event* event) {
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void ConvoyChat::OnWriteDone(const EventLoop::Event* event) {
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  write_in_flight_ = false;
  // Sends the next note if one was typed meanwhile. Otherwise the next typed note sends it.
  SendNextNote();
}

void ConvoyChat::OnDone(const EventLoop::Event* event) {
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  const auto status = reactor_->Status();
  logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor), status.ok(),
               status.error_message());
  // Every note read was shown: the read side never waited, and nothing was dropped.
  assert(handled_seq_ == read_seq_);
  logger_.info("totals   | {} notes typed, {} notes shown", typed_, handled_seq_);
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  reactor_.reset();
}

void ConvoyChat::SendNextNote() {
  if (write_in_flight_ || pending_.empty()) return;
  auto note = std::move(pending_.front());
  pending_.erase(pending_.begin());
  // The last note of the chat: nobody has a note left to type, and no other note waits.
  const bool last = script_.empty() && pending_.empty();
  // A rejected write is a dead end for this pump: the note has already left the queue, and no
  // OnWriteDone will arrive to drive the next one. Report it rather than stalling in silence.
  const bool accepted = last ? reactor_->SendLastRequest(std::move(note)) : reactor_->SendRequest(std::move(note));
  if (!accepted) {
    logger_.info("         | write rejected, {} note(s) left unsent", pending_.size() + 1);
    return;
  }
  write_in_flight_ = true;
}

}  // namespace rg_demo::usecase
