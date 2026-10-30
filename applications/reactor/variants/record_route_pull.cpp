///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/record_route_pull.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::pull {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with its push counterpart,
// whose OnDone handler would otherwise read this reactor pointer as an owned message.
constexpr auto kOnWriteDone{"RecordRoutePullOnWriteDone"};
constexpr auto kOnDone{"RecordRoutePullOnDone"};
}  // anonymous namespace

RecordRoute::RecordRoute(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_write_done_(kOnWriteDone, [this](const EventLoop::Event* e) { OnWriteDone(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

RecordRoute::~RecordRoute() {
  TearDownReactor(reactor_, logger_);
}

void RecordRoute::Start(std::vector<routeguide::Point> points) {
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
  // TriggerEvent: OnDone. The response stays in the reactor, so the event carries the reactor itself.
  cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnDone, reactor);
  };

  // (Point 1.1) Create reactor
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), std::move(cbs));
  activity_.Begin();
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
  SendNextPoint();
}

void RecordRoute::OnWriteDone(const EventLoop::Event* event) {
  // ProceedEvent: OnWriteDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
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
  // The event data is the reactor, which still holds the response
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  // The slot is populated by now, for the same reason as the GetFeature scenarios.
  assert(reactor == reactor_.get());
  if (const auto status = reactor_->Status(); status.ok()) {
    routeguide::RecordRoute::ResponseT response;
    if (reactor_->GetResponse(response)) {
      logger_.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
    }
  } else {
    logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor),
                 status.ok(), status.error_message());
  }
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  reactor_.reset();
  activity_.End();
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

}  // namespace rg_demo::pull
