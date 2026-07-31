///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/scenarios/record_route.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so one name means one registration and one scenario object.
constexpr auto kOnWriteDone{"RecordRouteOnWriteDone"};
constexpr auto kOnDone{"RecordRouteOnDone"};
}  // anonymous namespace

RecordRoute::RecordRoute(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_write_done_(kOnWriteDone, [this](const EventLoop::Event* e) { OnWriteDone(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

void RecordRoute::Start(std::vector<routeguide::Point> points) {
  using Callbacks = routeguide::RecordRoute::Callbacks;
  using ClientReactor = routeguide::RecordRoute::ClientReactor;
  using ResponseT = routeguide::RecordRoute::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring {} points", fmt::ptr(reactor_.get()),
                 points.size());
    return;
  }
  if (points.empty()) {
    logger_.info("         | no points to send, ignoring");
    return;
  }
  pending_ = std::move(points);

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
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
  SendNextPoint();
}

void RecordRoute::OnWriteDone(const EventLoop::Event* event) {
  // ProceedEvent: OnWriteDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<routeguide::RecordRoute::ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  // The point already sent to SendLastRequest() also triggers OnWriteDone, so check the pending
  // queue rather than unconditionally sending: it is empty once the last point sent was the final
  // one.
  if (!pending_.empty()) {
    SendNextPoint();
  }
}

void RecordRoute::OnDone(const EventLoop::Event* event) {
  // ProceedEvent: OnDone
  assert(OnApplicationThread());
  // The event data is the response, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::RecordRoute::ResponseT> response{
      static_cast<routeguide::RecordRoute::ResponseT*>(event->getData())};
  // The slot is populated by now, for the same reason as the GetFeature scenario.
  assert(reactor_);
  if (const auto status = reactor_->Status(); status.ok()) {
    logger_.info("RESPONSE | {}: {}", response->GetTypeName(), protobuf_utils::ToString(*response));
  } else {
    logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor_.get()),
                 status.ok(), status.error_message());
  }
  const auto* reactor = reactor_.get();
  reactor_.reset();
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
}

void RecordRoute::SendNextPoint() {
  auto point = std::move(pending_.front());
  pending_.erase(pending_.begin());
  const bool last = pending_.empty();
  // A rejected write is a dead end for this pump: the point has already left the queue, and no
  // OnWriteDone will arrive to drive the next one. Report it rather than stalling in silence.
  const bool accepted =
      last ? reactor_->SendLastRequest(std::move(point)) : reactor_->SendRequest(std::move(point));
  if (!accepted) {
    logger_.info("         | write rejected, {} point(s) left unsent", pending_.size() + 1);
  }
}

}  // namespace rg_demo
