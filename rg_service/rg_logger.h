///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2026 anderewrey
///

#pragma once

#include <spdlog/logger.h>

namespace routeguide {

// Forward declaration
enum class RpcMethods;

namespace logger {

// Get logger for specific RPC method
spdlog::logger& Get(RpcMethods method);

}  // namespace logger

}  // namespace routeguide
