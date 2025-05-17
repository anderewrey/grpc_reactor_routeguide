///
/// Copyright 2024-2025 anderewrey.
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///

#pragma once

#include <string>
#include <vector>

#include "generated/route_guide.grpc.pb.h"

using FeatureList = std::vector<routeguide::Feature>;

namespace proto_utils {
routeguide::Point MakePoint(int32_t latitude, int32_t longitude);
routeguide::Rectangle MakeRectangle(int32_t latitude_lo, int32_t longitude_lo,
                                    int32_t latitude_hi, int32_t longitude_hi);
routeguide::Feature MakeFeature(const std::string& name, int32_t latitude, int32_t longitude);
routeguide::RouteNote MakeRouteNote(const std::string& message, int32_t latitude, int32_t longitude);
double GetDistance(const routeguide::Point& start, const routeguide::Point& end);
const char* GetFeatureName(const routeguide::Point& point, const FeatureList& feature_list);
bool IsPointWithinRectangle(const routeguide::Rectangle& rectangle, const routeguide::Point& point);
bool AreEqual(const routeguide::Point& point1, const routeguide::Point& point2);
routeguide::Feature GetFeatureFromPoint(const FeatureList& feature_list, const routeguide::Point& point);
const routeguide::Point& GetRandomPoint(const FeatureList& feature_list);
int GetRandomTimeDelay();
}  // namespace proto_utils
