///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/live_route_recording.h"

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
// name rather than replacing them, so this scenario must not share a name with its pull counterpart:
// its OnDone event carries an owned message, which only one handler may reclaim.
constexpr auto kOnGpsFix{"LiveRouteRecordingOnGpsFix"};
constexpr auto kOnWriteDone{"LiveRouteRecordingOnWriteDone"};
constexpr auto kOnDone{"LiveRouteRecordingOnDone"};

// The GPS produces a fix every kGpsPeriodMs. Fixes kTunnelFirst to kTunnelLast are delivered together, at
// the time of the last one, as a receiver does with the fixes it buffered in a tunnel: those arrive
// faster than a write completes, so they wait in the queue.
constexpr std::size_t kGpsPeriodMs{20};
constexpr std::size_t kTunnelFirst{3};
constexpr std::size_t kTunnelLast{5};
}  // anonymous namespace

LiveRouteRecording::LiveRouteRecording(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_gps_fix_(kOnGpsFix, [this](const EventLoop::Event* e) { OnGpsFix(e); }),
      on_write_done_(kOnWriteDone, [this](const EventLoop::Event* e) { OnWriteDone(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

LiveRouteRecording::~LiveRouteRecording() {
  TearDownReactor(reactor_, logger_);
}

void LiveRouteRecording::Start(std::vector<routeguide::Point> drive) {
  using ResponseT = routeguide::RecordRoute::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring {} points", fmt::ptr(reactor_.get()),
                 drive.size());
    return;
  }
  if (drive.empty()) {
    logger_.info("         | no points to send, ignoring");
    return;
  }
  drive_ = std::move(drive);
  pending_.clear();
  fixes_ = 0;
  write_in_flight_ = false;

  Callbacks cbs;
  // TriggerEvent: OnWriteDone
  cbs.write_done = [](auto* reactor, bool) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnWriteDone, reactor);
  };
  // TriggerEvent: OnDone, handing the response ownership to the queue
  cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnDone, response.release());
  };

  // (Point 1.1) Create reactor
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), std::move(cbs));
  activity_ = Activity::Begin();
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
  // Switches the GPS on: one timed event per fix, delivered on this thread. Nothing is sent before the
  // first fix, and nothing here waits for it.
  for (std::size_t i = 0; i < drive_.size(); ++i) {
    const std::size_t fix = (i >= kTunnelFirst && i <= kTunnelLast) ? kTunnelLast : i;
    EventLoop::TriggerEvent(kOnGpsFix, (fix + 1) * kGpsPeriodMs);
  }
}

void LiveRouteRecording::OnGpsFix(const EventLoop::Event* /*event*/) {
  // ProceedEvent: a GPS fix, which carries no payload: the point is the drive's next one.
  assert(OnApplicationThread());
  assert(reactor_ && !drive_.empty());
  pending_.push_back(std::move(drive_.front()));
  drive_.erase(drive_.begin());
  ++fixes_;
  if (write_in_flight_) {
    logger_.info("gps      | fix #{} queued, a write is in flight, {} waiting", fixes_, pending_.size());
    return;
  }
  logger_.info("gps      | fix #{} sent at once", fixes_);
  SendNextPoint();
}

void LiveRouteRecording::OnWriteDone(const EventLoop::Event* event) {
  // ProceedEvent: OnWriteDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  write_in_flight_ = false;
  // Sends the next point if one arrived meanwhile. Otherwise the next GPS fix sends it.
  SendNextPoint();
}

void LiveRouteRecording::OnDone(const EventLoop::Event* event) {
  // ProceedEvent: OnDone
  assert(OnApplicationThread());
  // Ends the scenario when this handler returns, after the reactor is destroyed.
  const Activity activity{std::move(activity_)};
  // The event data is the response, not the reactor: reclaim the released ownership
  std::unique_ptr<routeguide::RecordRoute::ResponseT> response{
      static_cast<routeguide::RecordRoute::ResponseT*>(event->getData())};
  // The slot is populated by now, for the same reason as the GetFeature scenarios.
  assert(reactor_);
  if (const auto status = reactor_->Status(); status.ok()) {
    logger_.info("RESPONSE | {}: {}", response->GetTypeName(), protobuf_utils::ToString(*response));
    // Every fix of the drive was sent, and counted.
    assert(static_cast<std::size_t>(response->point_count()) == fixes_);
    // Handed to the trip history, which keeps the summary it now owns.
    trip_history_.push_back(std::move(response));
    logger_.info("history  | {} trip(s) recorded", trip_history_.size());
  } else {
    logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor_.get()),
                 status.ok(), status.error_message());
  }
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor_.get()));
  reactor_.reset();
}

void LiveRouteRecording::SendNextPoint() {
  if (write_in_flight_ || pending_.empty()) return;
  auto point = std::move(pending_.front());
  pending_.erase(pending_.begin());
  // The last point of the drive: the GPS has no fix left, and no other point waits.
  const bool last = drive_.empty() && pending_.empty();
  // A rejected write is a dead end for this pump: the point has already left the queue, and no
  // OnWriteDone will arrive to drive the next one. Report it rather than stalling in silence.
  const bool accepted =
      last ? reactor_->SendLastRequest(std::move(point)) : reactor_->SendRequest(std::move(point));
  if (!accepted) {
    logger_.info("         | write rejected, {} point(s) left unsent", pending_.size() + 1);
    return;
  }
  write_in_flight_ = true;
}

}  // namespace rg_demo::usecase
