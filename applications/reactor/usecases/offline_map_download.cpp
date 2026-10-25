///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/offline_map_download.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"OfflineMapDownloadOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"OfflineMapDownloadOnReadDoneNOk"};
constexpr auto kOnDone{"OfflineMapDownloadOnDone"};

// How many places may wait for the disk writer before read_ok holds the stream. Small on purpose, so the
// bound engages within the region's hundred places and the transcript shows both answers. A real
// application sizes it from the memory it can spare for places waiting to be written.
constexpr unsigned kMaxInFlight{3};

// How long the disk writer takes to save one place: longer than the server takes to send the next.
constexpr auto kSaveTime{std::chrono::milliseconds(1)};
}  // anonymous namespace

OfflineMapDownload::OfflineMapDownload(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_ok_(kOnReadDoneOk, [this](const EventLoop::Event* e) { OnReadOk(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

OfflineMapDownload::~OfflineMapDownload() {
  TearDownReactor(reactor_, logger_);
}

void OfflineMapDownload::Start(routeguide::Rectangle rect) {
  using ResponseT = routeguide::ListFeatures::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(rect));
    return;
  }

  Callbacks cbs;
  // (Point 2.6, or 2.14 when not held) TriggerEvent: OnReadDoneOk, handing the message ownership to the
  // queue. Delta against the turn-by-turn counterpart: the return value is the hold decision, taken for
  // this message only, here from the writer's backlog: the stream waits once kMaxInFlight places do.
  // Returning true holds the RPC until ResumeRead(); returning false lets the reactor read on at once.
  // The message is queued either way, so its ownership always ends in exactly one OnReadOk(): only the
  // pace of the stream depends on the answer.
  cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) -> bool {
    assert(!OnApplicationThread());  // gRPC thread
    // Counted before the hand-off, so the handler's decrement for this place can never run first.
    const auto in_flight = ++in_flight_;
    const auto seq = ++read_seq_;
    const bool hold = in_flight >= kMaxInFlight;
    if (hold) ++holds_;
    logger_.info("read_ok  | #{} {}, {} in flight, {}", seq, response->GetTypeName(), in_flight,
                 hold ? "held" : "not held");
    // Recorded before the hand-off too, so the handler that will release the hold cannot miss it.
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

void OfflineMapDownload::OnReadOk(const EventLoop::Event* event) {
  // (Point 2.8, or 2.18 when not held) ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the message, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::ListFeatures::ResponseT> response{
      static_cast<routeguide::ListFeatures::ResponseT*>(event->getData())};
  const auto seq = ++handled_seq_;
  // (Point 2.9, or 2.19 when not held) save the place for offline use, which takes a while
  logger_.info("RESPONSE | #{} {}: {}", seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  std::this_thread::sleep_for(kSaveTime);
  // Saved: its in-flight slot is freed.
  --in_flight_;
  // (Point 2.10, 2.11, 2.12) Delta against the turn-by-turn counterpart: a hold is released only by the
  // handler of the message read_ok held. The queue is FIFO, so every message read before it has been
  // handled by then: the stream stops once kMaxInFlight places wait, and restarts once they are saved.
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

void OfflineMapDownload::OnReadNOk(const EventLoop::Event* event) {
  // (Point 4.7) ProceedEvent: OnReadDoneNOk
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.8) update application
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void OfflineMapDownload::OnDone(const EventLoop::Event* event) {
  // (Point 4.9) ProceedEvent: OnDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.10) update application with status. The handler of the last place runs before this one, and
  // the stream cannot end while a held place waits, so every place is saved by now.
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  assert(in_flight_ == 0);
  logger_.info("totals   | {} places saved, download paused {} times", handled_seq_, holds_.load());
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  // (Point 4.11) Destroy reactor
  reactor_.reset();
}

}  // namespace rg_demo::usecase
