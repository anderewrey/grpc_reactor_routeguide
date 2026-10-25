///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/scenarios/live_vehicle_position.h"

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
constexpr auto kOnLatestFix{"LiveVehiclePositionOnLatestFix"};
constexpr auto kOnReadDoneNOk{"LiveVehiclePositionOnReadDoneNOk"};
constexpr auto kOnDone{"LiveVehiclePositionOnDone"};

// How long redrawing the map takes. The server streams the whole rectangle in a few milliseconds, so a
// map this slow draws only a few of its fixes, and the slot drops the others.
constexpr auto kRedrawTime{std::chrono::milliseconds(5)};
}  // anonymous namespace

LiveVehiclePosition::LiveVehiclePosition(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_latest_fix_(kOnLatestFix, [this](const EventLoop::Event* e) { OnLatestFix(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

LiveVehiclePosition::~LiveVehiclePosition() {
  TearDownReactor(reactor_, logger_);
}

void LiveVehiclePosition::Start(routeguide::Rectangle rect) {
  using ResponseT = routeguide::ListFeatures::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(rect));
    return;
  }

  Callbacks cbs;
  // (Point 2.16) Copies the lent fix into the slot, inside the reaction, since the reactor re-arms the
  // read into the same storage as soon as this returns. Delta against list_features_pull_continuous.cpp:
  // the message reaches the application thread after all, through the slot rather than through an event
  // per message. A fix the map has not drawn yet is replaced, and an event is triggered only when the slot
  // was empty, so however fast the fixes come, at most one event waits in the queue. Put() only takes a
  // short lock: the reaction never waits on the application.
  cbs.read_ok = [this](auto*, const ResponseT& response) {
    assert(!OnApplicationThread());  // gRPC thread
    const unsigned seq = ++read_seq_;
    logger_.info("read_ok  | #{} {}: {}", seq, response.GetTypeName(), protobuf_utils::ToString(response));
    if (latest_.Put(Fix{seq, response})) EventLoop::TriggerEvent(kOnLatestFix);
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

void LiveVehiclePosition::OnLatestFix(const EventLoop::Event*) {
  // ProceedEvent: the slot's notification, which carries no payload: the fix waits in the slot.
  assert(OnApplicationThread());
  // Empty when the OnDone handler already took the last fix.
  if (auto fix = latest_.Take()) Draw(*fix);
}

void LiveVehiclePosition::OnReadNOk(const EventLoop::Event* event) {
  // (Point 4.7) ProceedEvent: OnReadDoneNOk
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.8) update application
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void LiveVehiclePosition::OnDone(const EventLoop::Event* event) {
  // (Point 4.9) ProceedEvent: OnDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.10) update application with status. Every reaction has returned before OnDone() fires, so
  // the last fix is in the slot unless a handler already took it: the newest value is never lost.
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  if (auto fix = latest_.Take()) Draw(*fix);
  const unsigned read = read_seq_;
  const auto replaced = latest_.Replaced();
  logger_.info("totals   | {} fixes, {} drawn, {} replaced before drawing, last drawn #{}", read, drawn_, replaced,
               last_drawn_seq_);
  assert(drawn_ + replaced == read);
  assert(last_drawn_seq_ == read);
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  // (Point 4.11) Destroy reactor
  reactor_.reset();
}

void LiveVehiclePosition::Draw(const Fix& fix) {
  assert(OnApplicationThread());
  ++drawn_;
  last_drawn_seq_ = fix.seq;
  logger_.info("RESPONSE | #{} {}: {}", fix.seq, fix.feature.GetTypeName(), protobuf_utils::ToString(fix.feature));
  // Stands for the map's redraw. It holds the application thread, as real work would.
  std::this_thread::sleep_for(kRedrawTime);
}

}  // namespace rg_demo::usage
