#include "proto/proto_utils.h"

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

Point proto_utils::MakePoint(const int32_t latitude, const int32_t longitude) {
  Point p;
  p.set_latitude(latitude);
  p.set_longitude(longitude);
  return p;
}

Rectangle proto_utils::MakeRectangle(const int32_t latitude_lo, const int32_t longitude_lo, const int32_t latitude_hi, const int32_t longitude_hi) {
  Rectangle rect;
  *rect.mutable_lo() = MakePoint(latitude_lo, longitude_lo);
  *rect.mutable_hi() = MakePoint(latitude_hi, longitude_hi);
  return rect;
}

Feature proto_utils::MakeFeature(const std::string& name, const int32_t latitude, const int32_t longitude) {
  Feature f;
  f.set_name(name);
  *f.mutable_location() = MakePoint(latitude, longitude);
  return f;
}

RouteNote proto_utils::MakeRouteNote(const std::string& message, const int32_t latitude, const int32_t longitude) {
  RouteNote n;
  n.set_message(message);
  *n.mutable_location() = MakePoint(latitude, longitude);
  return n;
}

// The formula is based on http://mathforum.org/library/drmath/view/51879.html
double proto_utils::GetDistance(const Point& start, const Point& end) {
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

const char* proto_utils::GetFeatureName(const Point& point, const FeatureList& feature_list) {
  for (const Feature& f : feature_list) {
    if (f.location().latitude() == point.latitude() &&
        f.location().longitude() == point.longitude()) {
      return f.name().c_str();
    }
  }
  return nullptr;
}

bool proto_utils::IsPointWithinRectangle(const Rectangle& rectangle, const Point& point) {
  const auto left = std::min(rectangle.lo().longitude(), rectangle.hi().longitude());
  const auto right = std::max(rectangle.lo().longitude(), rectangle.hi().longitude());
  const auto top = std::max(rectangle.lo().latitude(), rectangle.hi().latitude());
  const auto bottom = std::min(rectangle.lo().latitude(), rectangle.hi().latitude());
  return (point.longitude() >= left && point.longitude() <= right &&
          point.latitude() >= bottom && point.latitude() <= top);
}

bool proto_utils::AreEqual(const Point& point1, const Point& point2) {
  return point1.latitude() == point2.latitude() &&
         point1.longitude() == point2.longitude();
}

Feature proto_utils::GetFeatureFromPoint(const FeatureList& feature_list, const Point& point) {
  Feature feature;
  if (const auto name = GetFeatureName(point, feature_list)) {
    if (strlen(name) > 0) {
      feature.set_name(name);
    }
    *feature.mutable_location() = point;
  }
  return feature;
}

const Point& proto_utils::GetRandomPoint(const FeatureList& feature_list) {
  static unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  static std::default_random_engine generator(seed);
  std::uniform_int_distribution<int> feature_distribution(0, feature_list.size() - 1);
  return feature_list[feature_distribution(generator)].location();
}

int proto_utils::GetRandomTimeDelay() {
  static unsigned seed = std::chrono::system_clock::now().time_since_epoch().count();
  static std::default_random_engine generator(seed);
  static std::uniform_int_distribution<int> delay_distribution(500, 1500);
  return delay_distribution(generator);
}
