///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#include "applications/reactor/demo_support.h"

#include <spdlog/pattern_formatter.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <ctime>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "applications/reactor/coro_task.h"

namespace rg_demo {
namespace {
// Captured at static initialization, which runs before main() and on the thread that entered it.
const std::thread::id application_thread = std::this_thread::get_id();

// The %k flag of the coroutine applications' pattern: the coroutine task the logging thread runs.
class TaskFlag final : public spdlog::custom_flag_formatter {
 public:
  void format(const spdlog::details::log_msg& /*msg*/, const std::tm& /*time*/, spdlog::memory_buf_t& dest) override {
    using RpcReactor::coro::CurrentTask;
    const CurrentTask::Id task = CurrentTask::Get();
    const std::string text = task == CurrentTask::kNone ? std::string("task#-") : "task#" + std::to_string(task);
    dest.append(text.data(), text.data() + text.size());
  }
  [[nodiscard]] std::unique_ptr<custom_flag_formatter> clone() const override {
    return std::make_unique<TaskFlag>();
  }
};
}  // anonymous namespace

bool OnApplicationThread() {
  return application_thread == std::this_thread::get_id();
}

std::unique_ptr<grpc::ClientContext> CreateClientContext() {
  auto context = std::make_unique<grpc::ClientContext>();
  // context->set_wait_for_ready(true);
  return context;
}

void SetCoroutineLogPattern() {
  auto formatter = std::make_unique<spdlog::pattern_formatter>();
  formatter->add_flag<TaskFlag>('k').set_pattern("[%H:%M:%S.%f][%n][%t][%k][%^%L%$] %v");
  spdlog::set_formatter(std::move(formatter));
}

spdlog::logger& VariantLogger(const std::string& name) {
  auto logger = spdlog::get(name);
  if (!logger) logger = spdlog::stdout_color_mt(name);
  return *logger;
}

}  // namespace rg_demo
