///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <grpcpp/client_context.h>
#include <spdlog/logger.h>

#include <chrono>
#include <memory>
#include <string>

/// Helpers shared by every demo scenario. Nothing here belongs to the reactor library: these are the
/// application-side conveniences the scenarios happen to have in common.
namespace rg_demo {

/// Reports whether the caller runs on the thread that entered main() and drives the EventLoop.
/// Every scenario asserts on it, from both sides: an application-thread handler asserts it holds, a
/// gRPC-thread callback asserts it does not. The thread is captured at static initialization, which
/// runs before main() on that same thread.
/// @return true when called on the application thread
bool OnApplicationThread();

/// Creates the client context handed to a reactor at construction. Kept in one place so a change of
/// context policy, a deadline for instance, applies to every scenario at once.
/// @return a fresh context, owned by the caller until it is moved into a reactor
std::unique_ptr<grpc::ClientContext> CreateClientContext();

/// Returns the logger dedicated to one reactor variant, creating it on first use.
/// The registry in rg_logger.h is keyed by RPC method and carries no variant axis, so the pacing
/// twins of one method would share a logger name and their lines would interleave under a single
/// grep. Extending that registry is story 5.5; until it lands the demo names its own loggers.
/// Created after spdlog::set_pattern(), so the registry's pattern is applied to them.
/// @param name logger name, for example "ListFeatures.cont"
/// @return reference to the registered logger
spdlog::logger& VariantLogger(const std::string& name);

/// Sets the log pattern of the coroutine applications: the pattern of the callback applications, with the
/// running coroutine task after the thread id, as [task#N], or [task#-] outside any task. Every coroutine
/// resumes on the application thread, so the thread id no longer tells two coroutines apart; the task does.
/// Call it first in main(), before any logger is created, so every logger gets it.
void SetCoroutineLogPattern();

/// Marks one scenario as in progress, from Begin() to End() or its destruction, for RunUntilDone(). A
/// scenario begins it in Start() and ends it once its work is over: after its last handler has destroyed
/// the reactor, or at the end of its coroutine. The calls run on the application thread, and repeated
/// calls do nothing.
class Activity {
 public:
  Activity() = default;
  ~Activity() { End(); }

  Activity(const Activity&) = delete;
  Activity& operator=(const Activity&) = delete;
  Activity(Activity&&) = delete;
  Activity& operator=(Activity&&) = delete;

  void Begin();
  void End();

 private:
  bool active_{false};
};

/// Runs the EventLoop on the calling thread, the application thread, until no Activity is in progress, then
/// halts it and returns. Returns at once when none is. An event already queued when the last Activity ends
/// still runs, while a timed event still pending is dropped: hence the rule that a scenario ends its
/// Activity only once it has nothing left to wait for.
void RunUntilDone();

/// Tears a scenario's reactor down when the scenario is destroyed, whether or not its RPC has ended. An RPC
/// still in flight is cancelled, and the reactor destroyed only once its OnDone() has run, since gRPC still
/// calls a reactor until then. Should OnDone() never come, the reactor is leaked rather than freed under
/// gRPC. The demo only reaches this with an RPC in flight when its event loop stops while one runs, which
/// nothing in it does today: the hardening is for an application that does.
/// @param reactor the scenario's reactor slot, empty once the RPC has ended
/// @param logger the scenario's logger, for the case where OnDone() never comes
template <class ReactorT>
void TearDownReactor(std::unique_ptr<ReactorT>& reactor, spdlog::logger& logger) {
  if (!reactor) return;
  if (reactor->CancelAndWait(std::chrono::seconds(5))) {
    reactor.reset();
    return;
  }
  logger.error("         | reactor[{}] never reached OnDone after its cancel, leaked", fmt::ptr(reactor.get()));
  static_cast<void>(reactor.release());
}

}  // namespace rg_demo
