///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <grpcpp/client_context.h>
#include <spdlog/logger.h>

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

}  // namespace rg_demo
