///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2015 gRPC authors
/// Copyright 2024-2025 anderewrey
///

#include <grpc/grpc.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/security/server_credentials.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <gflags/gflags.h>

#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "proto/route_guide_service.h"

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
}  // anonymous namespace

class RouteGuideImpl final : public RouteGuide::Service {
 public:
  Status GetFeature(ServerContext* context, const Point* point,
                    Feature* feature) override {
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kGetFeature);
    logger.info("ENTER    |");
    logger.info("REQUEST  | Point: {}", proto_utils::ToString(*point));
    *feature = proto_utils::GetFeatureFromPoint(feature_list_, *point);
    logger.info("RESPONSE | Feature: {}", proto_utils::ToString(*feature));
    logger.info("EXIT     |");
    return Status::OK;
  }

  Status ListFeatures(ServerContext* context,
                      const Rectangle* rectangle,
                      ServerWriter<Feature>* writer) override {
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kListFeatures);
    logger.info("ENTER    |");
    logger.info("REQUEST  | Rectangle: {}", proto_utils::ToString(*rectangle));
    for (const Feature& f : feature_list_) {
      if (proto_utils::IsPointWithinRectangle(*rectangle, f.location())) {
        logger.info("RESPONSE | Feature: {}", proto_utils::ToString(f));
        writer->Write(f);
      }
    }
    logger.info("EXIT     |");
    return Status::OK;
  }

  Status RecordRoute(ServerContext* context, ServerReader<Point>* reader,
                     RouteSummary* summary) override {
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kRecordRoute);
    logger.info("ENTER    |");
    Point point;
    int point_count = 0;
    int feature_count = 0;
    auto distance = 0.0;
    Point previous;

    const auto start_time = system_clock::now();
    while (reader->Read(&point)) {
      logger.info("REQUEST  | Point: {}", proto_utils::ToString(point));
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
    logger.info("RESPONSE | RouteSummary: {}", proto_utils::ToString(*summary));
    logger.info("EXIT     |");
    return Status::OK;
  }

  Status RouteChat(ServerContext* context,
                   ServerReaderWriter<RouteNote, RouteNote>* stream) override {
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kRouteChat);
    logger.info("ENTER    |");
    RouteNote note;
    while (stream->Read(&note)) {
      logger.info("REQUEST  | RouteNote: {}", proto_utils::ToString(note));
      std::unique_lock lock(mu_);
      for (const RouteNote& n : received_notes_) {
        if (n.location() == note.location()) {
          logger.info("RESPONSE | RouteNote: {}", proto_utils::ToString(n));
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
  static const std::string server_address("0.0.0.0:50051");

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
  auto logger_Main = spdlog::stdout_color_mt("Main");
  spdlog::set_default_logger(logger_Main);

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  feature_list_ = db_utils::GetDbFileContent();
  RunServer();

  gflags::ShutDownCommandLineFlags();
  return 0;
}
