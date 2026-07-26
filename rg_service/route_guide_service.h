///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2025 anderewrey
///

#ifndef RG_SERVICE_ROUTE_GUIDE_SERVICE_H_
#define RG_SERVICE_ROUTE_GUIDE_SERVICE_H_

#include "generated/route_guide.grpc.pb.h"
#include "generated/route_guide.pb.h"

#include <cstdint>

#include "rg_service/rg_logger.h"

namespace routeguide {

// RPC method enumeration
enum class RpcMethods : std::uint8_t {
  kGetFeature,
  kListFeatures,
  kRecordRoute,
  kRouteChat,
  kRpcMethodsLast,
};
constexpr auto kRpcMethodsQty = static_cast<size_t>(RpcMethods::kRpcMethodsLast);

// Convert RpcMethods enum to string
constexpr std::string_view ToString(const RpcMethods method) {
  switch (method) {
    case RpcMethods::kGetFeature:   return "GetFeature";
    case RpcMethods::kListFeatures: return "ListFeatures";
    case RpcMethods::kRecordRoute:  return "RecordRoute";
    case RpcMethods::kRouteChat:    return "RouteChat";
    default: return "Unknown";
  }
}

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

}  // namespace routeguide

#endif  // RG_SERVICE_ROUTE_GUIDE_SERVICE_H_
