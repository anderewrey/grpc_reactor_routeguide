///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include "common/db_utils.h"

#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <glaze/glaze.hpp>
#include <gflags/gflags.h>  // NOLINT(build/include_order)
#include <spdlog/spdlog.h>  // NOLINT(build/include_order)

#include "generated/route_guide.grpc.pb.h"
#include "proto/proto_utils.h"

// Expect only arg: --db_path=path/to/route_guide_db.json.
DEFINE_string(db_path, "route_guide_db.json", "path to .json database file");

using routeguide::Feature;

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

// A hardcoded parser for the route_guide database file and requires to have exactly the following JSON structure:
// [{"location": { "latitude": 123, "longitude": 456}, "name": "the name can be empty" }, { ... } ... ]
class RouteGuideDBParser {
 public:
  explicit RouteGuideDBParser(const std::string& db) : db_(glz::minify_json(db)) {
    if (!FindNextMatch("[")) {
      SetFailedAndReturnFalse();
    }
  }

  [[nodiscard]] bool Finished() const { return current_ >= db_.size(); }

  bool TryParseOne(Feature& feature) {
    if (failed_ || Finished() || !FindNextMatch("{")) {
      return SetFailedAndReturnFalse();
    }

    if (!FindNextMatch(location_) || !FindNextMatch("{") || !FindNextMatch(latitude_)) {
      return SetFailedAndReturnFalse();
    }
    const auto latitude = ReadNextAsNumerical();

    if (!FindNextMatch(",") || !FindNextMatch(longitude_)) {
      return SetFailedAndReturnFalse();
    }
    const auto longitude = ReadNextAsNumerical();

    if (!FindNextMatch("},") || !FindNextMatch(name_) || !FindNextMatch("\"")) {
      return SetFailedAndReturnFalse();
    }
    const auto name_start = current_;
    while (current_ != db_.size() && db_[current_++] != '"') {}
    if (current_ == db_.size()) {
      return SetFailedAndReturnFalse();
    }
    const auto name = db_.substr(name_start, current_ - name_start - 1);
    feature = proto_utils::MakeFeature(name, latitude, longitude);
    if (!FindNextMatch("},")) {
      if (db_[current_ - 1] == ']' && current_ == db_.size()) {
        return true;
      }
      return SetFailedAndReturnFalse();
    }
    return true;
  }

 private:
  bool SetFailedAndReturnFalse() {
    failed_ = true;
    return false;
  }

  [[nodiscard]] bool FindNextMatch(const std::string_view prefix) {
    const bool eq = db_.substr(current_, prefix.size()) == prefix;
    current_ += prefix.size();
    return eq;
  }

  [[nodiscard]] long ReadNextAsNumerical() {  // NOLINT(runtime/int)
    const auto start = current_;
    while (current_ != db_.size() &&
           db_[current_] != ',' &&
           db_[current_] != '}') {
      current_++;
    }
    // It will throw an exception if fails.
    return std::stol(db_.substr(start, current_ - start));
  }

  bool failed_{false};
  const std::string db_;
  size_t current_{0};
  static constexpr auto location_ = R"("location":)";
  static constexpr auto latitude_ = R"("latitude":)";
  static constexpr auto longitude_ = R"("longitude":)";
  static constexpr auto name_ = R"("name":)";
};

void db_utils::ParseDb(const std::string& db, FeatureList& feature_list) {
  feature_list.clear();
  RouteGuideDBParser parser(db);
  while (!parser.Finished()) {
    if (!parser.TryParseOne(feature_list.emplace_back())) {
      spdlog::error("Error parsing the db file");
      feature_list.clear();
      break;
    }
  }
  spdlog::info("DB parsed, loaded {} features.", feature_list.size());
}
