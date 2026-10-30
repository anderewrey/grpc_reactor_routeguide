///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/record_route_pull_producer.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::pull::producer {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling.
constexpr auto kOnWriteDone{"RecordRoutePullProducerOnWriteDone"};
constexpr auto kOnDone{"RecordRoutePullProducerOnDone"};
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
  pending_ = std::move(points);
  pulled_seq_ = 0;

  Callbacks cbs;
  // Delta against record_route_pull.cpp: the reactor pulls each point, so this slot replaces the
  // application's SendNextPoint() pump. It answers at once, from the queue, as the slot requires.
  cbs.next_request = [this](auto*) { return NextPoint(); };
  // TriggerEvent: OnWriteDone. Only for the log: the reactor pulls the next point right after this.
  cbs.write_done = [](auto* reactor, bool) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnWriteDone, reactor);
  };
  // TriggerEvent: OnDone. The response stays in the reactor, so the event carries the reactor itself.
  cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnDone, reactor);
  };

  // (Point 1.1) Create reactor. Its constructor already pulled the first point, on this thread.
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), std::move(cbs));
  activity_.Begin();
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

std::optional<routeguide::Point> RecordRoute::NextPoint() {
  // Runs on the application thread for the first pull, inside the reactor's constructor, and on a gRPC
  // thread for every later one.
  if (pending_.empty()) {
    logger_.info("next     | none left, the reactor closes the request stream");
    return std::nullopt;
  }
  auto point = std::move(pending_.front());
  pending_.erase(pending_.begin());
  logger_.info("next     | #{} {}", ++pulled_seq_, protobuf_utils::ToString(point));
  return point;
}

void RecordRoute::OnWriteDone(const EventLoop::Event* event) {
  // ProceedEvent: OnWriteDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void RecordRoute::OnDone(const EventLoop::Event* event) {
  // ProceedEvent: OnDone
  assert(OnApplicationThread());
  // The event data is the reactor, which still holds the response
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
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

}  // namespace rg_demo::pull::producer
