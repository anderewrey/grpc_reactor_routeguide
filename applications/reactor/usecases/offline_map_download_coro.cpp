///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/offline_map_download_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <thread>
#include <utility>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase::coro {
namespace {
// How many places may wait for the disk writer before the reader holds the stream, as in offline_map_download.cpp.
// Small on purpose, so the bound engages within the region's hundred places.
constexpr std::size_t kMaxInFlight{3};

// How long the disk writer takes to save one place: longer than the server takes to send the next.
constexpr auto kSaveTime{std::chrono::milliseconds(1)};
}  // anonymous namespace

OfflineMapDownload::OfflineMapDownload(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void OfflineMapDownload::Start(routeguide::Rectangle rect) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring: {}", protobuf_utils::ToString(rect));
    return;
  }
  roots_.Spawn(Run(std::move(rect), Activity::Begin()));
}

RpcReactor::coro::Task<> OfflineMapDownload::Run(routeguide::Rectangle rect, [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which sends the request and arms the first read
  Reader reader{executor_, stub_, rect, kMaxInFlight, CreateClientContext()};
  logger_.info("         | rpc started, pausing the download once {} places wait", kMaxInFlight);
  unsigned seq = 0;
  // (Point 2.6, or 2.14 when not held) The read callback queues each place, owned, in the reader, and holds the
  // stream once kMaxInFlight wait. (Point 2.8) Each Read() resumes on the application thread with the oldest place
  // queued, and the one that takes the queue below the bound resumes the download, before the place is saved.
  while (const auto place = co_await reader.Read()) {
    assert(OnApplicationThread());
    // (Point 2.9) save the place for offline use, which takes a while
    logger_.info("RESPONSE | #{} {}: {}", ++seq, place->GetTypeName(), protobuf_utils::ToString(*place));
    std::this_thread::sleep_for(kSaveTime);
    // Delta against list_features_push_per_message_coro.cpp: what waits for the writer once this place is saved.
    logger_.info("saved    | #{}, {} waiting", seq, reader.Queued());
  }
  // (Point 4.7, 4.8) Read() gave nothing: every place queued is saved, and the download has ended.
  assert(OnApplicationThread());
  logger_.info("         | stream ended after {} messages", seq);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run, with the status. (Point 4.11)
  // The reactor is destroyed.
  const grpc::Status status = co_await reader.Finish();
  assert(OnApplicationThread());
  logger_.info("totals   | {} places saved, download paused {} times", seq, reader.Holds());
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

}  // namespace rg_demo::usecase::coro
