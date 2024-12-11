#pragma once

#include <string>
#include <vector>

#include "generated/route_guide.grpc.pb.h"

using FeatureList = std::vector<routeguide::Feature>;

namespace db_utils {
std::string GetDbFileContent(int argc, char** argv);
void ParseDb(const std::string& db, FeatureList& feature_list);
}  // namespace db_utils
