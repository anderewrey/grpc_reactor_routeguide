///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "rg_service/rg_logger.h"
#include "rg_service/route_guide_service.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <memory>

#include "common/compat.h"

namespace routeguide::logger {
static class RGLoggers {
public:
  RGLoggers() {
    for (size_t i = 0; i < kRpcMethodsQty; ++i) {
      const auto method = static_cast<RpcMethods>(i);
      loggers_[i] = spdlog::stdout_color_mt(std::string(ToString(method)));
    }
  }
  auto& operator[](const RpcMethods method) const {
    return *loggers_[std::to_underlying(method)];
  }
private:
  std::array<std::shared_ptr<spdlog::logger>, kRpcMethodsQty> loggers_;
} loggers;

spdlog::logger& Get(const RpcMethods method) {
  return loggers[method];
}

}  // namespace routeguide::logger
