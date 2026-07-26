///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///

#pragma once

#include <spdlog/logger.h>

#include <cstdint>

namespace routeguide {

// Forward declaration
enum class RpcMethods : std::uint8_t;

namespace logger {

// Get logger for specific RPC method
spdlog::logger& Get(RpcMethods method);

}  // namespace logger

}  // namespace routeguide

