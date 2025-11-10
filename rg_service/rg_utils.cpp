///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "rg_service/rg_utils.h"

#include <algorithm>
#include <cctype>
#include <numbers>
#include <random>
#include <string>

#include "generated/route_guide.grpc.pb.h"

using routeguide::Feature;
using routeguide::Point;
using routeguide::Rectangle;
using routeguide::RouteGuide;
using routeguide::RouteNote;
using routeguide::RouteSummary;

Point rg_utils::MakePoint(const int32_t latitude, const int32_t longitude) {
  Point p;
  p.set_latitude(latitude);
  p.set_longitude(longitude);
  return p;
}

Rectangle rg_utils::MakeRectangle(const int32_t latitude_lo, const int32_t longitude_lo,
                                          const int32_t latitude_hi, const int32_t longitude_hi) {
  Rectangle rect;
  *rect.mutable_lo() = MakePoint(latitude_lo, longitude_lo);
  *rect.mutable_hi() = MakePoint(latitude_hi, longitude_hi);
  return rect;
}

Feature rg_utils::MakeFeature(const std::string& name, const int32_t latitude, const int32_t longitude) {
  Feature f;
  f.set_name(name);
  *f.mutable_location() = MakePoint(latitude, longitude);
  return f;
}

RouteNote rg_utils::MakeRouteNote(const std::string& message, const int32_t latitude, const int32_t longitude) {
  RouteNote n;
  n.set_message(message);
  *n.mutable_location() = MakePoint(latitude, longitude);
  return n;
}

// The formula is based on http://mathforum.org/library/drmath/view/51879.html
double rg_utils::GetDistance(const Point& start, const Point& end) {
  static constexpr auto kCoordFactor{10000000.0};
  static constexpr auto kR{6371000};  // the mean radius of the Earth, meters
  auto to_radian = [](auto num) {
    return (num * std::numbers::pi) / 180;
  };
  const auto lat_1 = start.latitude() / kCoordFactor;
  const auto lat_2 = end.latitude() / kCoordFactor;
  const auto lon_1 = start.longitude() / kCoordFactor;
  const auto lon_2 = end.longitude() / kCoordFactor;
  const auto lat_rad_1 = to_radian(lat_1);
  const auto lat_rad_2 = to_radian(lat_2);
  const auto delta_lat_rad = to_radian(lat_2 - lat_1);
  const auto delta_lon_rad = to_radian(lon_2 - lon_1);

  const auto a = pow(sin(delta_lat_rad / 2), 2) +
                 cos(lat_rad_1) * cos(lat_rad_2) * pow(sin(delta_lon_rad / 2), 2);
  const auto c = 2 * atan2(sqrt(a), sqrt(1 - a));
  return kR * c;
}

const char* rg_utils::GetFeatureName(const Point& point, const FeatureList& feature_list) {
  for (const Feature& f : feature_list) {
    if (f.location().latitude() == point.latitude() &&
        f.location().longitude() == point.longitude()) {
      return f.name().c_str();
    }
  }
  return nullptr;
}

bool rg_utils::IsPointWithinRectangle(const Rectangle& rectangle, const Point& point) {
  const auto left = std::min(rectangle.lo().longitude(), rectangle.hi().longitude());
  const auto right = std::max(rectangle.lo().longitude(), rectangle.hi().longitude());
  const auto top = std::max(rectangle.lo().latitude(), rectangle.hi().latitude());
  const auto bottom = std::min(rectangle.lo().latitude(), rectangle.hi().latitude());
  return (point.longitude() >= left && point.longitude() <= right &&
          point.latitude() >= bottom && point.latitude() <= top);
}

Feature rg_utils::GetFeatureFromPoint(const FeatureList& feature_list, const Point& point) {
  Feature feature;
  if (const auto name = GetFeatureName(point, feature_list)) {
    if (strlen(name) > 0) {
      feature.set_name(name);
    }
    *feature.mutable_location() = point;
  }
  return feature;
}

const Point& rg_utils::GetRandomPoint(const FeatureList& feature_list) {
  static unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  static std::default_random_engine generator(seed);
  std::uniform_int_distribution<unsigned> feature_distribution(0, feature_list.size() - 1);
  return feature_list[feature_distribution(generator)].location();
}

unsigned rg_utils::GetRandomTimeDelay() {
  static unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  static std::default_random_engine generator(seed);
  static std::uniform_int_distribution<unsigned> delay_distribution(500, 1500);
  return delay_distribution(generator);
}

bool routeguide::operator==(const Point& point1, const Point& point2) {
  return point1.latitude() == point2.latitude() &&
         point1.longitude() == point2.longitude();
}
