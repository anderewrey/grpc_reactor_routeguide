/*
 *
 * Copyright 2015 gRPC authors.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 *
 */
#include <grpc/grpc.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/security/server_credentials.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "generated/route_guide.grpc.pb.h"

#include "common/db_utils.h"
#include "proto/proto_utils.h"

using grpc::Server;
using grpc::ServerBuilder;
using grpc::ServerContext;
using grpc::ServerReader;
using grpc::ServerReaderWriter;
using grpc::ServerWriter;
using grpc::Status;
using routeguide::Feature;
using routeguide::Point;
using routeguide::Rectangle;
using routeguide::RouteGuide;
using routeguide::RouteNote;
using routeguide::RouteSummary;
using std::chrono::system_clock;

namespace {
std::thread::id main_thread = std::this_thread::get_id();
FeatureList feature_list_;

// Create and return a shared_ptr to a multithreaded console logger.
auto logger_GetFeature = spdlog::stdout_color_mt("GetFeature");
auto logger_ListFeatures = spdlog::stdout_color_mt("ListFeatures");
auto logger_RecordRoute = spdlog::stdout_color_mt("RecordRoute");
auto logger_RouteChat = spdlog::stdout_color_mt("RouteChat");
}  // anonymous namespace

class RouteGuideImpl final : public RouteGuide::Service {
 public:
  Status GetFeature(ServerContext* context, const Point* point,
                    Feature* feature) override {
    auto& logger = *logger_GetFeature;
    logger.info("ENTER    |");
    logger.info("REQUEST  | Point: {}", point->ShortDebugString());
    *feature = proto_utils::GetFeatureFromPoint(feature_list_, *point);
    logger.info("RESPONSE | Feature: {}", feature->ShortDebugString());
    logger.info("EXIT     |");
    return Status::OK;
  }

  Status ListFeatures(ServerContext* context,
                      const Rectangle* rectangle,
                      ServerWriter<Feature>* writer) override {
    auto& logger = *logger_ListFeatures;
    logger.info("ENTER    |");
    logger.info("REQUEST  | Rectangle: {}", rectangle->ShortDebugString());
    for (const Feature& f : feature_list_) {
      if (proto_utils::IsPointWithinRectangle(*rectangle, f.location())) {
        logger.info("RESPONSE | Feature: {}", f.ShortDebugString());
        writer->Write(f);
      }
    }
    logger.info("EXIT     |");
    return Status::OK;
  }

  Status RecordRoute(ServerContext* context, ServerReader<Point>* reader,
                     RouteSummary* summary) override {
    auto& logger = *logger_RecordRoute;
    logger.info("ENTER    |");
    Point point;
    int point_count = 0;
    int feature_count = 0;
    auto distance = 0.0;
    Point previous;

    const auto start_time = system_clock::now();
    while (reader->Read(&point)) {
      logger.info("REQUEST  | Point: {}", point.ShortDebugString());
      point_count++;
      if (const auto name = proto_utils::GetFeatureName(point, feature_list_); name && strlen(name) > 0) {
        feature_count++;
      }
      if (point_count != 1) {
        distance += proto_utils::GetDistance(previous, point);
      }
      previous = point;
    }
    const auto end_time = system_clock::now();
    summary->set_point_count(point_count);
    summary->set_feature_count(feature_count);
    summary->set_distance(distance);
    const auto secs = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
    summary->set_elapsed_time(secs);
    logger.info("RESPONSE | RouteSummary: {}", summary->ShortDebugString());
    logger.info("EXIT     |");
    return Status::OK;
  }

  Status RouteChat(ServerContext* context,
                   ServerReaderWriter<RouteNote, RouteNote>* stream) override {
    auto& logger = *logger_RouteChat;
    logger.info("ENTER    |");
    RouteNote note;
    while (stream->Read(&note)) {
      logger.info("REQUEST  | RouteNote: {}", note.ShortDebugString());
      std::unique_lock<std::mutex> lock(mu_);
      for (const RouteNote& n : received_notes_) {
        if (proto_utils::AreEqual(n.location(), note.location())) {
          logger.info("RESPONSE | RouteNote: {}", n.ShortDebugString());
          stream->Write(n);
        }
      }
      received_notes_.push_back(note);
    }
    logger.info("EXIT     |");
    return Status::OK;
  }

 private:
  std::mutex mu_;
  std::vector<RouteNote> received_notes_;
};

void RunServer() {
  spdlog::info("-------------- Server creation --------------");
  static constexpr std::string server_address("0.0.0.0:50051");

  RouteGuideImpl service;
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  spdlog::info("Server BuildAndStart");
  auto server = builder.BuildAndStart();
  spdlog::info("Server listening on {}", server_address);
  server->Wait();
}

int main(int argc, char** argv) {
  assert(main_thread == std::this_thread::get_id());
  spdlog::set_pattern("[%H:%M:%S.%f][%n][%t][%^%L%$] %v");

  // Expect only arg: --db_path=path/to/route_guide_db.json.
  db_utils::ParseDb(db_utils::GetDbFileContent(argc, argv), feature_list_);
  RunServer();
  return 0;
}
