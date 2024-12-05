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
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <string>
#include <thread>

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
}  // anonymous namespace

class RouteGuideImpl final : public RouteGuide::Service {
 public:
  Status GetFeature(ServerContext* context, const Point* point,
                    Feature* feature) override {
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] REQUEST  | Point: " << point->ShortDebugString()  << std::endl;
    *feature = proto_utils::GetFeatureFromPoint(feature_list_, *point);
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] RESPONSE | Feature: " << feature->ShortDebugString()  << std::endl;
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] EXIT     |" << std::endl;
    return Status::OK;
  }

  Status ListFeatures(ServerContext* context,
                      const Rectangle* rectangle,
                      ServerWriter<Feature>* writer) override {
    std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
    std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] REQUEST  | Rectangle: " << rectangle->ShortDebugString()  << std::endl;
    for (const Feature& f : feature_list_) {
      if (proto_utils::IsPointWithinRectangle(*rectangle, f.location())) {
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] RESPONSE | Feature: " << f.ShortDebugString()  << std::endl;
        writer->Write(f);
      }
    }
    std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     |" << std::endl;
    return Status::OK;
  }

  Status RecordRoute(ServerContext* context, ServerReader<Point>* reader,
                     RouteSummary* summary) override {
    std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
    Point point;
    int point_count = 0;
    int feature_count = 0;
    float distance = 0.0;
    Point previous;

    system_clock::time_point start_time = system_clock::now();
    while (reader->Read(&point)) {
    std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] REQUEST  | Point: " << point.ShortDebugString()  << std::endl;
      point_count++;
      if (const auto name = proto_utils::GetFeatureName(point, feature_list_); name && strlen(name) > 0) {
        feature_count++;
      }
      if (point_count != 1) {
        distance += proto_utils::GetDistance(previous, point);
      }
      previous = point;
    }
    system_clock::time_point end_time = system_clock::now();
    summary->set_point_count(point_count);
    summary->set_feature_count(feature_count);
    summary->set_distance(static_cast<long>(distance));
    auto secs = std::chrono::duration_cast<std::chrono::seconds>(end_time - start_time).count();
    summary->set_elapsed_time(secs);
    std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] RESPONSE | RouteSummary: " << summary->ShortDebugString()  << std::endl;
    std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] EXIT     |" << std::endl;
    return Status::OK;
  }

  Status RouteChat(ServerContext* context,
                   ServerReaderWriter<RouteNote, RouteNote>* stream) override {
    std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
    RouteNote note;
    while (stream->Read(&note)) {
      std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] REQUEST  | RouteNote: " << note.ShortDebugString()  << std::endl;
      std::unique_lock<std::mutex> lock(mu_);
      for (const RouteNote& n : received_notes_) {
        if (proto_utils::AreEqual(n.location(), note.location())) {
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] RESPONSE | RouteNote: " << n.ShortDebugString()  << std::endl;
          stream->Write(n);
        }
      }
      received_notes_.push_back(note);
    }
    std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     |" << std::endl;
    return Status::OK;
  }

 private:
  std::mutex mu_;
  std::vector<RouteNote> received_notes_;
};

void RunServer() {
  std::cout << "RouteGuide::RunServer[" << std::this_thread::get_id() << "] Server creation" << std::endl;
  std::string server_address("0.0.0.0:50051");

  RouteGuideImpl service;
  ServerBuilder builder;
  builder.AddListeningPort(server_address, grpc::InsecureServerCredentials());
  builder.RegisterService(&service);
  std::cout << "RouteGuide::RunServer[" << std::this_thread::get_id() << "] Server BuildAndStart" << std::endl;
  std::unique_ptr<Server> server(builder.BuildAndStart());
  std::cout << "RouteGuide::RunServer[" << std::this_thread::get_id() << "] Server listening on " << server_address << std::endl;
  server->Wait();
}

int main(int argc, char** argv) {
  assert(main_thread == std::this_thread::get_id());
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  db_utils::ParseDb(db_utils::GetDbFileContent(argc, argv), feature_list_);

  RunServer();

  return 0;
}
