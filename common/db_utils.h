///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#pragma once

#include <string>
#include <vector>

#include "generated/route_guide.grpc.pb.h"

using FeatureList = std::vector<routeguide::Feature>;

namespace db_utils {
FeatureList GetDbFileContent();
}  // namespace db_utils
