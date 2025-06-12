///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "common/db_utils.h"

#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>

#include "generated/route_guide.grpc.pb.h"
#include "proto/proto_utils.h"

// Expect only arg: --db_path=path/to/route_guide_db.json.
DEFINE_string(db_path, "route_guide_db.json", "path to .json database file");

using routeguide::Feature;

// The data representation for the route_guide database file and requires to have exactly the following JSON structure:
// [{"location": { "latitude": 123, "longitude": 456}, "name": "the name can be empty" }, { ... } ... ]
struct FeatureData {
  struct Location {
    int latitude{};
    int longitude{};
  } location;
  std::string name;
};
using FeatureDataList = std::vector<FeatureData>;

std::string db_utils::GetDbFileContent() {
  static const std::string db_path = FLAGS_db_path.empty() ? "route_guide_db.json" : FLAGS_db_path;

  std::ifstream db_file(db_path);
  if (!db_file.is_open()) {
    spdlog::critical("Failed to open {}", db_path);
    abort();
  }
  std::stringstream db;
  db << db_file.rdbuf();

  return db.str();
}

void db_utils::ParseDb(const std::string& db, FeatureList& feature_list) {
  feature_list.clear();
  FeatureDataList features;
  if (auto error = glz::read_json(features, db)) {
    spdlog::error("Error parsing the db file: code {} {}", fmt::underlying(error.ec), glz::nameof(error.ec));
    return;
  }
  for (const auto& [location, name] : features) {
    feature_list.emplace_back(proto_utils::MakeFeature(name, location.latitude, location.longitude));
  }
  spdlog::info("DB parsed, loaded {} features.", feature_list.size());
}
