///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/gpx_track_upload.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstddef>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase {
namespace {
// Event names are literal per-scenario constants on purpose. Deriving them from an identifier would
// make the string seen in a log line unfindable in this source tree. EventLoop appends handlers per
// name rather than replacing them, so this scenario must not share a name with any sibling.
constexpr auto kOnWriteDone{"GpxTrackUploadOnWriteDone"};
constexpr auto kOnDone{"GpxTrackUploadOnDone"};
}  // anonymous namespace

GpxTrackUpload::GpxTrackUpload(routeguide::RouteGuide::Stub& stub)
    : stub_(stub),
      on_write_done_(kOnWriteDone, [this](const EventLoop::Event* e) { OnWriteDone(e); }),
      on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {
}

GpxTrackUpload::~GpxTrackUpload() {
  TearDownReactor(reactor_, logger_);
}

void GpxTrackUpload::Start(std::vector<routeguide::Point> track) {
  using ResponseT = routeguide::RecordRoute::ResponseT;
  if (reactor_) {
    logger_.info("         | reactor[{}] already in execution, ignoring {} points", fmt::ptr(reactor_.get()),
                 track.size());
    return;
  }
  track_size_ = track.size();
  pending_ = std::move(track);
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
  activity_ = Activity::Begin();
  logger_.info("         | reactor[{}] created", fmt::ptr(reactor_.get()));
}

std::optional<routeguide::Point> GpxTrackUpload::NextPoint() {
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

void GpxTrackUpload::OnWriteDone(const EventLoop::Event* event) {
  // ProceedEvent: OnWriteDone
  assert(OnApplicationThread());
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  logger_.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
}

void GpxTrackUpload::OnDone(const EventLoop::Event* event) {
  // ProceedEvent: OnDone
  assert(OnApplicationThread());
  // Ends the scenario when this handler returns, after the reactor is destroyed.
  const Activity activity{std::move(activity_)};
  // The event data is the reactor, which still holds the response
  const auto* reactor = static_cast<ClientReactor*>(event->getData());
  assert(reactor == reactor_.get());
  if (const auto status = reactor_->Status(); status.ok()) {
    routeguide::RecordRoute::ResponseT response;
    if (reactor_->GetResponse(response)) {
      logger_.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
      // The upload screen shows the summary. Every point of the track was sent, and counted.
      assert(static_cast<std::size_t>(response.point_count()) == track_size_);
      logger_.info("summary  | {} points, {} known places passed, {} m", response.point_count(),
                   response.feature_count(), response.distance());
    }
  } else {
    logger_.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor),
                 status.ok(), status.error_message());
  }
  logger_.info("         | reactor[{}] ended", fmt::ptr(reactor));
  reactor_.reset();
}

}  // namespace rg_demo::usecase
