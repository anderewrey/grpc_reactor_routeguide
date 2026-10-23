///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///

#include "rg_service/rg_logger.h"

#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <memory>
#include <string>

#include "common/compat.h"

#include "rg_service/route_guide_service.h"

namespace routeguide::logger {

namespace {
class RGLoggers {
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
};
}  // namespace

spdlog::logger& Get(const RpcMethods method) {
  // Function-local static: construction (which can throw, since it creates spdlog sinks) happens
  // on first call rather than before main(), so a failure propagates as a normal, catchable
  // exception instead of an uncatchable one from static initialization.
  static const RGLoggers loggers;
  return loggers[method];
}

}  // namespace routeguide::logger
