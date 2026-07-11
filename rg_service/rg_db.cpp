///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "rg_service/rg_db.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

#include "rg_service/rg_utils.h"

namespace rg_db {

#include "rg_service/route_guide_db.inc"

FeatureList GetInitialFeatures() {
  FeatureList feature_list;
  feature_list.reserve(kInitialFeatures.size());
  for (const auto& [latitude, longitude, name] : kInitialFeatures) {
    feature_list.emplace_back(rg_utils::MakeFeature(std::string(name), latitude, longitude));
  }

  spdlog::info("Initial features loaded, {} features.", feature_list.size());
  return feature_list;
}

}  // namespace rg_db
