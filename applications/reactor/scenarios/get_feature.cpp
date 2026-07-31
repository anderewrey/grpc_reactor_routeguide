///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/scenarios/get_feature.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so one name means one registration and one scenario object.
constexpr auto kOnDone{"GetFeatureOnDone"};
}  // anonymous namespace

GetFeature::GetFeature(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

void GetFeature::Start(routeguide::Point point) {
  using Callbacks = routeguide::GetFeature::Callbacks;
  using ClientReactor = routeguide::GetFeature::ClientReactor;
  using ResponseT = routeguide::GetFeature::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(point));
    return;
  }
  Callbacks cbs;
  // (Point 3.4) TriggerEvent: OnDone, handing the response ownership to the queue
  cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnDone, response.release());
  };

  // (Point 1.1) Create reactor
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), point, std::move(cbs));
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

void GetFeature::OnDone(const EventLoop::Event* event) {
  // (Point 3.5) ProceedEvent: OnDone
  assert(OnApplicationThread());
  // The event data is the response, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::GetFeature::ResponseT> response{
      static_cast<routeguide::GetFeature::ResponseT*>(event->getData())};
  // The slot is populated by now: it is assigned on this same thread, and a second Start() is
  // refused while one RPC is in flight.
  assert(reactor_);
  if (const auto status = reactor_->Status(); status.ok()) {
    // (Point 3.6) update application with response
    logger_.info("RESPONSE | {}: {}", response->GetTypeName(), protobuf_utils::ToString(*response));
  } else {
    logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor_.get()),
                 status.ok(), status.error_message());
  }
  // (Point 3.7) Destroy reactor
  const auto* reactor = reactor_.get();
  reactor_.reset();
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
}

}  // namespace rg_demo
