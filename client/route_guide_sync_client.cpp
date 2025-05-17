///
/// Copyright 2015 gRPC authors.
///
/// Licensed under the Apache License, Version 2.0 (the "License");
/// you may not use this file except in compliance with the License.
/// You may obtain a copy of the License at
///
///     http://www.apache.org/licenses/LICENSE-2.0
///
/// Unless required by applicable law or agreed to in writing, software
/// distributed under the License is distributed on an "AS IS" BASIS,
/// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
/// See the License for the specific language governing permissions and
/// limitations under the License.
///

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "generated/route_guide.grpc.pb.h"

#include "common/db_utils.h"
#include "proto/proto_utils.h"

using grpc::Channel;
using grpc::ClientContext;
using grpc::ClientReader;
using grpc::ClientReaderWriter;
using grpc::ClientWriter;
using grpc::Status;
using routeguide::Feature;
using routeguide::Point;
using routeguide::Rectangle;
using routeguide::RouteGuide;
using routeguide::RouteNote;
using routeguide::RouteSummary;

namespace {
std::thread::id main_thread = std::this_thread::get_id();
FeatureList feature_list_;

// Create and return a shared_ptr to a multithreaded console logger.
auto logger_GetFeature = spdlog::stdout_color_mt("GetFeature");
auto logger_ListFeatures = spdlog::stdout_color_mt("ListFeatures");
auto logger_RecordRoute = spdlog::stdout_color_mt("RecordRoute");
auto logger_RouteChat = spdlog::stdout_color_mt("RouteChat");
}  // anonymous namespace

class RouteGuideClient {
 public:
  explicit RouteGuideClient(const std::shared_ptr<Channel>& channel)
      : stub_(RouteGuide::NewStub(channel)) {}

  void GetFeature() {
    auto get_feature = [stub_ = stub_.get(), &logger = *logger_GetFeature](const Point& point, Feature& feature) {
      logger.info("ENTER    |");
      ClientContext context;
      logger.info("REQUEST  | Point: {}", point.ShortDebugString());
      Status status = stub_->GetFeature(&context, point, &feature);
      if (!status.ok()) {
        logger.info("EXIT     | OK: {}  msg: {}", status.ok(), status.error_message());
        return false;
      }
      logger.info("RESPONSE | Feature: {}", feature.ShortDebugString());

      const bool result = feature.has_location();
      logger.info("EXIT     | return {}", result);
      return result;
    };
    Feature feature;
    get_feature(proto_utils::MakePoint(409146138, -746188906), feature);
    get_feature(proto_utils::MakePoint(1, 1), feature);
    get_feature(proto_utils::MakePoint(0, 0), feature);
    get_feature({}, feature);
  }

  void ListFeatures() {
    auto& logger = *logger_ListFeatures;
    Feature feature;
    ClientContext context;
    logger.info("ENTER    |");
    const auto rectangle = proto_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000);
    logger.info("REQUEST  | Rectangle: {}", rectangle.ShortDebugString());
    auto reader = stub_->ListFeatures(&context, rectangle);
    while (reader->Read(&feature)) {
      logger.info("RESPONSE | Feature: {}", feature.ShortDebugString());
    }
    logger.info("EXIT     | Pre-Finish()");
    Status status = reader->Finish();
    logger.info("EXIT     | Post-Finish() OK: {}  msg: {}", status.ok(), status.error_message());
  }

  void RecordRoute() {
    auto& logger = *logger_RecordRoute;
    logger.info("ENTER    |");
    RouteSummary summary;
    ClientContext context;
    const int kPoints = 10;

    auto writer = stub_->RecordRoute(&context, &summary);
    for (int i = 0; i < kPoints; i++) {
      const Point& point = proto_utils::GetRandomPoint(feature_list_);
      logger.info("REQUEST  | Point: {}", point.ShortDebugString());
      if (!writer->Write(point)) {
        // Broken stream.
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(proto_utils::GetRandomTimeDelay()));
    }
    logger.info("EXIT     | WritesDone");
    writer->WritesDone();
    logger.info("EXIT     | Finish");
    Status status = writer->Finish();
    logger.info("RESPONSE | Status: OK: {} msg: {} RouteSummary: {}", status.ok(), status.error_message(), summary.ShortDebugString());
    logger.info("EXIT     |");
  }

  void RouteChat() {
    auto& logger = *logger_RouteChat;
    logger.info("ENTER    |");
    ClientContext context;
    auto stream = stub_->RouteChat(&context);
    std::thread writer([stream = stream.get(), &logger]() {
      std::vector<RouteNote> notes{proto_utils::MakeRouteNote("First message", 1, 1),
                                   proto_utils::MakeRouteNote("Second message", 2, 2),
                                   proto_utils::MakeRouteNote("Third message", 3, 3),
                                   proto_utils::MakeRouteNote("First message again", 1, 1)};
      for (const RouteNote& note : notes) {
      logger.info("REQUEST  | RouteNote: {}", note.ShortDebugString());
        stream->Write(note);
      }
      logger.info("EXIT     | pre-WritesDone");
      logger.info("EXIT     | pre-WritesDone");
      stream->WritesDone();
      logger.info("EXIT     | post-WritesDone");
    });

    RouteNote server_note;
    while (stream->Read(&server_note)) {
          logger.info("RESPONSE | RouteNote: {}", server_note.ShortDebugString());
    }
    logger.info("EXIT     | waiting for writer.join()");
    writer.join();
    logger.info("EXIT     | Pre-Finish()");
    Status status = stream->Finish();
    logger.info("EXIT     | Post-Finish() OK: {}  msg: {}", status.ok(), status.error_message());
  }

 private:
  std::unique_ptr<RouteGuide::Stub> stub_;
};

int main(int argc, char** argv) {
  assert(main_thread == std::this_thread::get_id());
  spdlog::set_pattern("[%H:%M:%S.%f][%n][%t][%^%L%$] %v");

  // Expect only arg: --db_path=path/to/route_guide_db.json.
  db_utils::ParseDb(db_utils::GetDbFileContent(argc, argv), feature_list_);

  RouteGuideClient guide(
      grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));

  spdlog::info("-------------- GetFeature --------------");
  guide.GetFeature();
  spdlog::info("-------------- ListFeatures --------------");
  guide.ListFeatures();
  spdlog::info("-------------- RecordRoute --------------");
  guide.RecordRoute();
  spdlog::info("-------------- RouteChat --------------");
  guide.RouteChat();

  return 0;
}
