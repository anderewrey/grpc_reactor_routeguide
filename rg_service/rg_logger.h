///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///

#ifndef RG_SERVICE_RG_LOGGER_H_
#define RG_SERVICE_RG_LOGGER_H_

#include <spdlog/logger.h>

namespace routeguide {

// Forward declaration
enum class RpcMethods;

namespace logger {

// Get logger for specific RPC method
spdlog::logger& Get(RpcMethods method);

}  // namespace logger

}  // namespace routeguide

#endif  // RG_SERVICE_RG_LOGGER_H_
