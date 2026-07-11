///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "rg_service/rg_db.h"

#include <spdlog/spdlog.h>

#include <array>
#include <cstdint>
#include <string_view>

#include "rg_service/rg_utils.h"
#include "rg_service/route_guide_db.inc"

FeatureList rg_db::GetInitialFeatures() {
  FeatureList feature_list;
  feature_list.reserve(kInitialFeatures.size());
  for (const auto& [name, latitude, longitude] : kInitialFeatures) {
    feature_list.emplace_back(rg_utils::MakeFeature(name, latitude, longitude));
  }

  spdlog::info("Initial features loaded, {} features.", feature_list.size());
  return feature_list;
}
