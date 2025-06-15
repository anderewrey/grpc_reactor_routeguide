///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "common/db_utils.h"

#include <gflags/gflags.h>
#include <spdlog/spdlog.h>

#include <string>
#include <vector>

#include <glaze/glaze.hpp>

#include "generated/route_guide.grpc.pb.h"
#include "proto/proto_utils.h"

// Expect only arg: --db_path=path/to/route_guide_db.json.
DEFINE_string(db_path, "route_guide_db.json", "path to .json database file");

using routeguide::Feature;

// The data representation for the route_guide database file and requires to have exactly the following JSON structure:
// [{"location": { "latitude": 123, "longitude": 456}, "name": "the name can be empty" }, { ... }, ... ]
struct FeatureJson {
  struct Location {
    int latitude{};
    int longitude{};
  } location;
  std::string name;
};
using FeatureJsonList = std::vector<FeatureJson>;

FeatureList db_utils::GetDbFileContent() {
  if (FLAGS_db_path.empty()) {
    spdlog::error("arg --db_path is empty");
    return {};
  }

  FeatureJsonList json_data;
  if (const auto error = glz::read_file_json(json_data, FLAGS_db_path, std::string())) {
    spdlog::error("Error parsing the db file: code {} {}", fmt::underlying(error.ec), glz::nameof(error.ec));
    return {};
  }

  FeatureList feature_list;
  for (const auto& [location, name] : json_data) {
    feature_list.emplace_back(proto_utils::MakeFeature(name, location.latitude, location.longitude));
  }

  spdlog::info("DB parsed, loaded {} features.", feature_list.size());
  return feature_list;
}
