///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#pragma once

#include <vector>

#include "proto/route_guide_service.h"

using FeatureList = std::vector<routeguide::Feature>;

namespace db_utils {
FeatureList GetDbFileContent();
}  // namespace db_utils
