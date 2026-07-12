///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2026 anderewrey
///

#pragma once

#include <vector>

#include "rg_service/route_guide_service.h"

using FeatureList = std::vector<routeguide::Feature>;

namespace rg_db {
FeatureList GetInitialFeatures();
}  // namespace rg_db
