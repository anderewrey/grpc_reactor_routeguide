#pragma once

#include <string>
#include <vector>

#include "generated/route_guide.grpc.pb.h"

using FeatureList = std::vector<routeguide::Feature>;

namespace proto_utils {
routeguide::Point MakePoint(int32_t latitude, int32_t longitude);
routeguide::Rectangle MakeRectangle(int32_t latitude_lo, int32_t longitude_lo, int32_t latitude_hi, int32_t longitude_hi);
routeguide::Feature MakeFeature(const std::string& name, int32_t latitude, int32_t longitude);
routeguide::RouteNote MakeRouteNote(const std::string& message, int32_t latitude, int32_t longitude);
double GetDistance(const routeguide::Point& start, const routeguide::Point& end);
const char* GetFeatureName(const routeguide::Point& point, const FeatureList& feature_list);
bool IsPointWithinRectangle(const routeguide::Rectangle& rectangle, const routeguide::Point& point);
bool AreEqual(const routeguide::Point& point1, const routeguide::Point& point2);
routeguide::Feature GetFeatureFromPoint(const FeatureList& feature_list, const routeguide::Point& point);
const routeguide::Point& GetRandomPoint(const FeatureList& feature_list);
int GetRandomTimeDelay();
}  // namespace rg_utils
