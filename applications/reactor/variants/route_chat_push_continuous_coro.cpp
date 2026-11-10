///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/variants/route_chat_push_continuous_coro.h"

#include <spdlog/spdlog.h>

#include <cassert>
#include <cstddef>
#include <utility>
#include <vector>

#include "protobuf_utils/protobuf_utils.h"

namespace rg_demo::push::continuous::coro {

RouteChat::RouteChat(RpcReactor::coro::Executor& executor, routeguide::RouteGuide::Stub& stub)
    : executor_(executor), stub_(stub) {
}

void RouteChat::Start(std::vector<routeguide::RouteNote> notes) {
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

RpcReactor::coro::Task<> RouteChat::Run(std::vector<routeguide::RouteNote> notes,
                                        [[maybe_unused]] Activity activity) {
  assert(OnApplicationThread());
  // (Point 1.1) Create reactor, which starts the RPC and arms the first read. Declared before the join, so both
  // flows end before it does.
  Chat chat{executor_, stub_, CreateClientContext()};
  logger_.info("         | rpc started");
  // Both flows run at once on the application thread: each suspends at its own co_await, and the join resumes
  // this coroutine once both have returned.
  const auto [shown, sent] = co_await RpcReactor::coro::WhenAll(ShowReplies(&chat), SendNotes(&chat, std::move(notes)));
  assert(OnApplicationThread());
  logger_.info("         | {} note(s) sent, {} reply(ies) shown", sent, shown);
  // (Point 4.9, 4.10) Resumed on the application thread once OnDone() has run. (Point 4.11) The reactor is
  // destroyed.
  const grpc::Status status = co_await chat.Finish();
  assert(OnApplicationThread());
  logger_.info("         | Status: OK: {} msg: {}", status.ok(), status.error_message());
  logger_.info("         | rpc ended");
}

RpcReactor::coro::Task<unsigned> RouteChat::ShowReplies(Chat* chat) {
  unsigned shown = 0;
  // Resumed on the application thread with each reply, owned, at once when the reactor already queued it.
  while (auto response = co_await chat->Read()) {
    assert(OnApplicationThread());
    logger_.info("RESPONSE | #{} {}: {}", ++shown, response->GetTypeName(), protobuf_utils::ToString(*response));
  }
  assert(OnApplicationThread());
  logger_.info("         | read stream ended");
  co_return shown;
}

RpcReactor::coro::Task<std::size_t> RouteChat::SendNotes(Chat* chat, std::vector<routeguide::RouteNote> notes) {
  std::size_t sent = 0;
  for (auto& note : notes) {
    // Resumed on the application thread once the write has completed: the next write may start. A rejected
    // write means the RPC has ended, so the notes left are not sent.
    if (!co_await chat->Write(std::move(note))) {
      assert(OnApplicationThread());
      logger_.info("         | write rejected, {} note(s) left unsent", notes.size() - sent);
      break;
    }
    assert(OnApplicationThread());
    logger_.info("sent     | #{}", ++sent);
  }
  // Delta against the callback parent: the last note is not sent with SendLastRequest(), since the loop does not
  // single it out. The request stream is closed after it, which never suspends; the server may keep sending.
  static_cast<void>(co_await chat->Close());
  co_return sent;
}

}  // namespace rg_demo::push::continuous::coro
