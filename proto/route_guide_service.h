///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#pragma once

// Single point of import for generated proto/gRPC code
#include "generated/route_guide.pb.h"
#include "generated/route_guide.grpc.pb.h"

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <array>
#include <memory>

#include "common/utility.h"

namespace routeguide {

// RPC method enumeration
enum class RpcMethods {
  kGetFeature,
  kListFeatures,
  kRecordRoute,
  kRouteChat,
  kRpcMethodsLast,
};
constexpr auto kRpcMethodsQty = static_cast<size_t>(RpcMethods::kRpcMethodsLast);

// GetFeature RPC metadata
namespace GetFeature {
using RequestT = Point;
using ResponseT = Feature;
inline constexpr auto RpcKey = RpcMethods::kGetFeature;
}  // namespace GetFeature

// ListFeatures RPC metadata
namespace ListFeatures {
using RequestT = Rectangle;
using ResponseT = Feature;
inline constexpr auto RpcKey = RpcMethods::kListFeatures;
}  // namespace ListFeatures

// RecordRoute RPC metadata
namespace RecordRoute {
using RequestT = Point;
using ResponseT = RouteSummary;
inline constexpr auto RpcKey = RpcMethods::kRecordRoute;
}  // namespace RecordRoute

// RouteChat RPC metadata
namespace RouteChat {
using RequestT = RouteNote;
using ResponseT = RouteNote;
inline constexpr auto RpcKey = RpcMethods::kRouteChat;
}  // namespace RouteChat

// Logger registry for RouteGuide service
namespace logger {
inline std::array<std::shared_ptr<spdlog::logger>, kRpcMethodsQty>& Registry() {
  static std::array<std::shared_ptr<spdlog::logger>, kRpcMethodsQty> loggers = {
      spdlog::stdout_color_mt("GetFeature"),
      spdlog::stdout_color_mt("ListFeatures"),
      spdlog::stdout_color_mt("RecordRoute"),
      spdlog::stdout_color_mt("RouteChat")};
  return loggers;
}

inline spdlog::logger& Get(RpcMethods method) {
  return *Registry()[std::to_underlying(method)];
}
}  // namespace logger

}  // namespace routeguide
