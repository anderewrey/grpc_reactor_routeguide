///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/unnamed_places_audit.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneNOk{"UnnamedPlacesAuditOnReadDoneNOk"};
constexpr auto kOnDone{"UnnamedPlacesAuditOnDone"};
}  // anonymous namespace

UnnamedPlacesAudit::UnnamedPlacesAudit(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

UnnamedPlacesAudit::~UnnamedPlacesAudit() {
  TearDownReactor(reactor_, logger_);
}

void UnnamedPlacesAudit::Start(routeguide::Rectangle rect) {
  using ResponseT = routeguide::ListFeatures::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(rect));
    return;
  }

  Callbacks cbs;
  // (Point 2.16) Consumes the message inside the reaction. Delta against the turn-by-turn counterpart: no
  // event is queued per message, since the reactor re-arms the read into the same storage as soon as
  // this returns, and the lent message is overwritten before the application thread could look at it.
  // Whatever the application needs from a message is therefore taken here, on the gRPC thread, without
  // waiting on anything; here it is the audit's aggregate, kept in atomic counters. Keeping a whole message
  // instead would need a deep copy, which is what Delivery::kPush avoids by handing it out owned.
  cbs.read_ok = [this](auto*, const ResponseT& response) {
    assert(!OnApplicationThread());  // gRPC thread
    if (response.name().empty()) ++unnamed_count_;
    logger_.info("read_ok  | #{} {}: {}", ++read_seq_, response.GetTypeName(), protobuf_utils::ToString(response));
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
  activity_ = Activity::Begin();
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

void UnnamedPlacesAudit::OnReadNOk(const EventLoop::Event* event) {
  // (Point 4.7) ProceedEvent: OnReadDoneNOk
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.8) update application
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void UnnamedPlacesAudit::OnDone(const EventLoop::Event* event) {
  // (Point 4.9) ProceedEvent: OnDone
  assert(OnApplicationThread());
  // Ends the scenario when this handler returns, after the reactor is destroyed.
  const Activity activity{std::move(activity_)};
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.10) update application with status, and with the aggregate the reactions built. Every
  // reaction has returned before OnDone() fires, so the counters are final.
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  const unsigned read = read_seq_;
  const unsigned unnamed = unnamed_count_;
  logger_.info("totals   | {} places audited, {} unnamed, {} named", read, unnamed, read - unnamed);
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  // (Point 4.11) Destroy reactor
  reactor_.reset();
}

}  // namespace rg_demo::usecase
