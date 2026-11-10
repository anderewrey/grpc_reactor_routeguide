///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/usecases/convoy_chat_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <chrono>
#include <cstddef>
#include <utility>
#include <vector>

#include "applications/reactor/reactor_eventloop.h"
#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::usecase::coro {
namespace {
// A driver types a note every kTypingPeriod, as in convoy_chat.cpp, except notes kTogetherFirst and
// kTogetherSecond, which two drivers type at the same moment: the second one waits for the first one's write.
constexpr std::chrono::milliseconds kTypingPeriod{10};
constexpr std::size_t kTogetherFirst{1};
constexpr std::size_t kTogetherSecond{2};

/// When note `i`, counted from 0, is typed, after the drivers start typing.
std::chrono::milliseconds TypedAt(std::size_t i) {
  const std::size_t moment = i == kTogetherSecond ? kTogetherFirst : i;
  return kTypingPeriod * static_cast<int>(moment + 1);
}
}  // anonymous namespace

ConvoyChat::ConvoyChat(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void ConvoyChat::Start(std::vector<routeguide::RouteNote> notes) {
  if (roots_.Size() != 0) {
    logger_.info("         | rpc already in execution, ignoring {} notes", notes.size());
    return;
  }
  if (notes.empty()) {
    logger_.info("         | no notes to send, ignoring");
    return;
  }
  roots_.Spawn(Run(std::move(notes), Activity::Begin()));
}

RpcReactor::coro::Task<> ConvoyChat::Run(std::vector<routeguide::RouteNote> notes,
                                         [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which starts the RPC and arms the first read. Declared before the join, so both
  // flows end before it does.
  Chat chat{executor_, stub_, CreateClientContext()};
  logger_.info("         | rpc started");
  const auto [shown, typed] = co_await RpcReactor::coro::WhenAll(ShowNotes(&chat), TypeNotes(&chat, std::move(notes)));
  assert(OnApplicationThread());
  // Every note read was shown: the Bidi queued each one, and the read flow took them all.
  logger_.info("totals   | {} notes typed, {} notes shown", typed, shown);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run. (Point 4.11) The reactor is
  // destroyed.
  const grpc::Status status = co_await chat.Finish();
  assert(OnApplicationThread());
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

RpcReactor::coro::Task<unsigned> ConvoyChat::ShowNotes(Chat* chat) {
  unsigned shown = 0;
  while (auto response = co_await chat->Read()) {
    assert(OnApplicationThread());
    // Shows the note in the chat
    logger_.info("RESPONSE | #{} {}: {}", ++shown, response->GetTypeName(), protobuf_utils::ToString(*response));
  }
  assert(OnApplicationThread());
  logger_.info("         | read stream ended");
  co_return shown;
}

RpcReactor::coro::Task<std::size_t> ConvoyChat::TypeNotes(Chat* chat, std::vector<routeguide::RouteNote> notes) {
  using Clock = std::chrono::steady_clock;
  // The drivers start typing. Nothing is sent before the first note is typed, and the read flow runs meanwhile.
  const auto start = Clock::now();
  std::size_t typed = 0;
  for (auto& note : notes) {
    // Delta against route_chat_push_continuous_coro.cpp: the note is typed first. A note already typed came while
    // the last write was in flight, and waited for it.
    const auto due = start + TypedAt(typed);
    ++typed;
    if (const auto now = Clock::now(); now < due) {
      co_await RpcReactor::coro::EventLoopExecutor::After(std::chrono::ceil<std::chrono::milliseconds>(due - now));
      assert(OnApplicationThread());
      logger_.info("typed    | note #{} sent at once", typed);
    } else {
      logger_.info("typed    | note #{} waited for a write in flight", typed);
    }
    // Resumed on the application thread once the write has completed. A rejected write means the RPC has ended.
    if (!co_await chat->Write(std::move(note))) {
      assert(OnApplicationThread());
      logger_.info("         | write rejected, {} note(s) left unsent", notes.size() - typed + 1);
      break;
    }
    assert(OnApplicationThread());
  }
  // Nobody has a note left to type. Delta against the callback twin: the last note is not sent with
  // SendLastRequest(); the request stream is closed after it, and the server may keep sending.
  static_cast<void>(co_await chat->Close());
  co_return typed;
}

}  // namespace rg_demo::usecase::coro
