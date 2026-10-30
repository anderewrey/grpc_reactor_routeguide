///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/whats_here.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with its push counterpart,
// whose handler would otherwise read this reactor pointer as an owned message.
constexpr auto kOnDone{"WhatsHereOnDone"};
}  // anonymous namespace

WhatsHere::WhatsHere(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

WhatsHere::~WhatsHere() {
  TearDownReactor(reactor_, logger_);
}

void WhatsHere::Start(routeguide::Point point) {
  using ResponseT = routeguide::GetFeature::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(point));
    return;
  }
  Callbacks cbs;
  // (Point 3.4) TriggerEvent: OnDone. The response stays in the reactor, so the event carries the
  // reactor itself.
  cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
    assert(!OnApplicationThread());  // gRPC thread
    EventLoop::TriggerEvent(kOnDone, reactor);
  };

  // (Point 1.1) Create reactor
  reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), point, std::move(cbs));
  activity_.Begin();
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

void WhatsHere::OnDone(const EventLoop::Event* event) {
  // (Point 3.5) ProceedEvent: OnDone
  assert(OnApplicationThread());
  // The event data is the reactor, which still holds the response
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  // The slot is populated by now: it is assigned on this same thread, and a second Start() is
  // refused while one RPC is in flight.
  assert(reactor == reactor_.get());
  if (const auto status = reactor_->Status(); status.ok()) {
    // (Point 3.6) extracts response, by swap rather than deep-copy
    routeguide::GetFeature::ResponseT response;
    if (reactor_->GetResponse(response)) {
      // (Point 3.7) update application with response: the card shows the name of the place, if any. The
      // server answers a point with no known place with an empty feature.
      logger_.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
      if (response.name().empty()) {
        logger_.info("card     | nothing named here");
      } else {
        logger_.info("card     | {}", response.name());
      }
    }
  } else {
    logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor),
                 status.ok(), status.error_message());
  }
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  // (Point 3.8) Destroy reactor
  reactor_.reset();
  activity_.End();
}

}  // namespace rg_demo::usecase
