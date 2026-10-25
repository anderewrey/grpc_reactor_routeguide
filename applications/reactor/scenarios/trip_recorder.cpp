///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/scenarios/trip_recorder.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <memory>
#include <thread>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usage {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"TripRecorderOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"TripRecorderOnReadDoneNOk"};
constexpr auto kOnDone{"TripRecorderOnDone"};

// How long storing one report takes. The server streams the whole rectangle in a few milliseconds, so
// storage this slow is still on the first reports when the last one arrives.
constexpr auto kStoreTime{std::chrono::milliseconds(1)};
}  // anonymous namespace

TripRecorder::TripRecorder(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_ok_(kOnReadDoneOk, [this](const EventLoop::Event* e) { OnReadOk(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

TripRecorder::~TripRecorder() {
  TearDownReactor(reactor_, logger_);
}

void TripRecorder::Start(routeguide::Rectangle rect) {
  using ResponseT = routeguide::ListFeatures::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(rect));
    return;
  }

  Callbacks cbs;
  // (Point 2.14) TriggerEvent: OnReadDoneOk, handing the report's ownership to the queue. The reactor
  // arms the next read as soon as this returns, so the queue grows as fast as the server sends. Delta
  // against list_features_push_continuous.cpp: each line reports the backlog, the reports queued but not
  // yet stored, which nothing bounds.
  cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) {
    assert(!OnApplicationThread());  // gRPC thread
    const unsigned seq = ++read_seq_;
    const unsigned backlog = seq - stored_seq_;
    if (backlog > peak_backlog_) peak_backlog_ = backlog;
    logger_.info("read_ok  | #{} {}, backlog {}", seq, response->GetTypeName(), backlog);
    EventLoop::TriggerEvent(kOnReadDoneOk, response.release());
  };
  // (Point 4.3) TriggerEvent: OnReadDoneNOk
  cbs.read_nok = [](auto* reactor) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnReadDoneNOk, reactor);
  };
  // (Point 4.6) TriggerEvent: OnDone. Delta against list_features_push_continuous.cpp: the RPC is over,
  // but the trip log is not, since the reports still queued are all ahead of this event.
  cbs.done = [this](auto* reactor, const grpc::Status&) {
    assert(!OnApplicationThread());  // gRPC thread
    logger_.info("done     | RPC ended, {} reports still queued for the trip log", read_seq_ - stored_seq_);
    EventLoop::TriggerEvent(kOnDone, reactor);
  };

  // (Point 1.1) Create reactor
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), rect, std::move(cbs));
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

void TripRecorder::OnReadOk(const EventLoop::Event* event) {
  // (Point 2.18) ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the report, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::ListFeatures::ResponseT> response{
      static_cast<routeguide::ListFeatures::ResponseT*>(event->getData())};
  // (Point 2.19) update application with response: store it in the trip log. Nothing to resume: the
  // reactor already re-armed the read when read_ok returned, and offers no ResumeRead() under this pacing.
  const unsigned seq = stored_seq_ + 1;
  logger_.info("RESPONSE | #{} {}: {}", seq, response->GetTypeName(), protobuf_utils::ToString(*response));
  // Stands for the write to storage. It holds the application thread, as real work would.
  std::this_thread::sleep_for(kStoreTime);
  stored_seq_ = seq;
}

void TripRecorder::OnReadNOk(const EventLoop::Event* event) {
  // (Point 4.7) ProceedEvent: OnReadDoneNOk
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.8) update application
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void TripRecorder::OnDone(const EventLoop::Event* event) {
  // (Point 4.9) ProceedEvent: OnDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.10) update application with status. The queue is FIFO and every report was queued before
  // this event, so the trip log holds every one of them, in order.
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  const unsigned read = read_seq_;
  const unsigned stored = stored_seq_;
  logger_.info("totals   | {} reports read, {} stored in the trip log, peak backlog {}", read, stored,
               peak_backlog_.load());
  assert(stored == read);
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  // (Point 4.11) Destroy reactor
  reactor_.reset();
}

}  // namespace rg_demo::usage
