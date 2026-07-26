///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024 anderewrey
///

#ifndef RG_SERVICE_RG_DB_H_
#define RG_SERVICE_RG_DB_H_

#include <vector>

#include "rg_service/route_guide_service.h"

using FeatureList = std::vector<routeguide::Feature>;

namespace rg_db {
FeatureList GetInitialFeatures();
}  // namespace rg_db

#endif  // RG_SERVICE_RG_DB_H_
