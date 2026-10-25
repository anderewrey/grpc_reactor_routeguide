///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/list_features_push_continuous.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <memory>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::push::continuous {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling: a push
// read event carries an owned message, and two handlers reclaiming the same pointer would free it
// twice, while a pull handler would read it as a reactor.
constexpr auto kOnReadDoneOk{"ListFeaturesPushContOnReadDoneOk"};
constexpr auto kOnReadDoneNOk{"ListFeaturesPushContOnReadDoneNOk"};
constexpr auto kOnDone{"ListFeaturesPushContOnDone"};
}  // anonymous namespace

ListFeatures::ListFeatures(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_read_ok_(kOnReadDoneOk, [this](const EventLoop::Event* e) { OnReadOk(e); }),
      on_read_nok_(kOnReadDoneNOk, [this](const EventLoop::Event* e) { OnReadNOk(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

ListFeatures::~ListFeatures() {
  TearDownReactor(reactor_, logger_);
}

void ListFeatures::Start(routeguide::Rectangle rect) {
  using ResponseT = routeguide::ListFeatures::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_.get()),
                 protobuf_utils::ToString(rect));
    return;
  }

  Callbacks cbs;
  // (Point 2.14) TriggerEvent: OnReadDoneOk, handing the message ownership to the queue. The reactor
  // arms the next read as soon as this returns, so the queue grows as fast as the server sends.
  cbs.read_ok = [this](auto*, std::unique_ptr<ResponseT> response) {
    assert(!OnApplicationThread());  // gRPC thread
    logger_.info("read_ok  | #{} {}", ++read_seq_, response->GetTypeName());
    EventLoop::TriggerEvent(kOnReadDoneOk, response.release());
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

void ListFeatures::OnReadOk(const EventLoop::Event* event) {
  // (Point 2.18) ProceedEvent: OnReadDoneOk
  assert(OnApplicationThread());
  // The event data is the message, not the reactor: reclaim the released ownership
  const std::unique_ptr<routeguide::ListFeatures::ResponseT> response{
      static_cast<routeguide::ListFeatures::ResponseT*>(event->getData())};
  const auto seq = ++handled_seq_;
  // (Point 2.19) update application with response. Nothing to resume: the reactor already re-armed the
  // read when read_ok returned, and offers no ResumeRead() under this pacing.
  logger_.info("RESPONSE | #{} {}: {}", seq, response->GetTypeName(), protobuf_utils::ToString(*response));
}

void ListFeatures::OnReadNOk(const EventLoop::Event* event) {
  // (Point 4.7) ProceedEvent: OnReadDoneNOk
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.8) update application
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void ListFeatures::OnDone(const EventLoop::Event* event) {
  // (Point 4.9) ProceedEvent: OnDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  // (Point 4.10) update application with status
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  // (Point 4.11) Destroy reactor
  reactor_.reset();
}

}  // namespace rg_demo::push::continuous
