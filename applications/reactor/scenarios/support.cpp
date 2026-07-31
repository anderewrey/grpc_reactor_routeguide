///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/scenarios/support.h"

#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>
#include <thread>

namespace rg_demo {
namespace {
// Captured at static initialization, which runs before main() and on the thread that entered it.
const std::thread::id application_thread = std::this_thread::get_id();
}  // anonymous namespace

bool OnApplicationThread() {
  return application_thread == std::this_thread::get_id();
}

std::unique_ptr<grpc::ClientContext> CreateClientContext() {
  auto context = std::make_unique<grpc::ClientContext>();
  // context->set_wait_for_ready(true);
  return context;
}

spdlog::logger& VariantLogger(const std::string& name) {
  auto logger = spdlog::get(name);
  if (!logger) logger = spdlog::stdout_color_mt(name);
  return *logger;
}

}  // namespace rg_demo
