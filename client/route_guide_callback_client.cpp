///
/// Copyright 2021 gRPC authors.
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
#include <grpcpp/alarm.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "generated/route_guide.grpc.pb.h"

#include "common/db_utils.h"
#include "proto/proto_utils.h"

using grpc::Channel;
using grpc::ClientContext;
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
      bool result;
      std::mutex mu;
      std::condition_variable cv;
      bool done = false;
      logger.info("REQUEST  | Point: {}", point.ShortDebugString());
      stub_->async()->GetFeature(&context, &point, &feature, [&result, &mu, &cv, &done, &feature, &logger](Status status) {
        logger.info("RESPONSE | Status: OK: {} msg:{} Feature: {}", status.ok(), status.error_message(), feature.ShortDebugString());
        std::lock_guard<std::mutex> lock(mu);
        result = (status.ok() && feature.has_location());
        done = true;
        logger.info("EXIT     | cv.notify_one()", result);
        cv.notify_one();
      });
      std::unique_lock<std::mutex> lock(mu);
      logger.info("EXIT     | waiting for cv.wait()", result);
      cv.wait(lock, [&done] { return done; });
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
    class Reader : public grpc::ClientReadReactor<Feature> {
     public:
      Reader(RouteGuide::Stub* stub, const Rectangle& rectangle) {
        logger_.info("ENTER    |");
        logger_.info("REQUEST  | Rectangle: {}", rectangle.ShortDebugString());
        stub->async()->ListFeatures(&context_, &rectangle, this);
        logger_.info("ENTER    | StartRead");
        StartRead(&feature_);
        logger_.info("ENTER    | StartCall");
        StartCall();
      }
      void OnReadDone(bool ok) override {
        if (ok) {
          logger_.info("RESPONSE | OK: {} Feature: {}", ok, feature_.ShortDebugString());
          StartRead(&feature_);
        } else {
          logger_.info("EXIT     | OnReadDone() OK: {}", ok);
        }
      }
      void OnDone(const Status& status) override {
        logger_.info("EXIT     | OnDone() Status: OK: {} msg: {}", status.ok(), status.error_message());
        std::unique_lock<std::mutex> l(mu_);
        status_ = status;
        done_ = true;
        cv_.notify_one();
      }
      Status Await() {
        std::unique_lock<std::mutex> l(mu_);
        logger_.info("EXIT     | pre-wait()");
        cv_.wait(l, [this] { return done_; });
        logger_.info("EXIT     | post-wait()");
        return std::move(status_);
      }

     private:
      ClientContext context_;
      Feature feature_;
      std::mutex mu_;      spdlog::logger& logger_ = *logger_ListFeatures;

      std::condition_variable cv_;
      Status status_;
      bool done_ = false;
    };

    Reader reader(stub_.get(), proto_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000));
    Status status = reader.Await();
    logger_ListFeatures->info("EXIT     | post-Await() OK: {} msg: {}", status.ok(), status.error_message());
  }

  void RecordRoute() {
    class Recorder : public grpc::ClientWriteReactor<Point> {
     public:
      Recorder(RouteGuide::Stub* stub, const FeatureList& feature_list)
          : feature_list_(feature_list) {
        logger_.info("ENTER    |");
        stub->async()->RecordRoute(&context_, &summary_, this);
        logger_.info("ENTER    | NextWrite");
        NextWrite();
        logger_.info("ENTER    | StartCall");
        StartCall();
      }
      void OnWriteDone(bool ok) override {
        logger_.info("         | OnWriteDone() OK: {} alarm_.Set()", ok);
        // Delay and then do the next write or WritesDone
        alarm_.Set(std::chrono::system_clock::now() + std::chrono::milliseconds(proto_utils::GetRandomTimeDelay()),
                   [this](const bool not_cancelled) {
                     if (!not_cancelled) {
                       logger_.info("REQUEST  | cancelled");
                       return;
                     }
                     NextWrite();
                   });
      }
      void OnDone(const Status& status) override {
        std::unique_lock<std::mutex> l(mu_);
        logger_.info("RESPONSE | OnDone() Status: OK: {} msg: {}", status.ok(), status.error_message());
        status_ = status;
        done_ = true;
        cv_.notify_one();
      }
      Status Await(RouteSummary& summary) {
        std::unique_lock<std::mutex> l(mu_);
        logger_.info("EXIT     | pre-wait()");
        cv_.wait(l, [this] { return done_; });
        logger_.info("EXIT     | post-wait()");
        swap(summary, summary_);
        return std::move(status_);
      }

     private:
      void NextWrite() {
        std::unique_lock<std::mutex> l(mu_);
        if (done_) {
          logger_.info("REQUEST  | was done_");
          return;  // No more pending writes, the stream is terminated
        }
        if (points_remaining_ != 0) {
          const Point& point = proto_utils::GetRandomPoint(feature_list_);
          logger_.info("REQUEST  | Point: {}", point.ShortDebugString());
          StartWrite(&point);
          points_remaining_--;
        } else {
          logger_.info("EXIT     | StartWritesDone");
          StartWritesDone();
        }
      }
      ClientContext context_;
      spdlog::logger& logger_ = *logger_RecordRoute;
      RouteSummary summary_;
      const FeatureList& feature_list_;
      grpc::Alarm alarm_;  // To postpone an action to do later in the eventloop (handled by gRPC in its own thread pool)
      std::mutex mu_;
      std::condition_variable cv_;
      Status status_;
      int points_remaining_ = 10;
      bool done_ = false;
    };
    RouteSummary summary;
    Recorder recorder(stub_.get(), feature_list_);
    Status status = recorder.Await(summary);
    logger_RecordRoute->info("EXIT     | post-Await() OK: {} msg: {} RouteSummary: {}", status.ok(), status.error_message(), summary.ShortDebugString());
  }

  void RouteChat() {
    class Chatter : public grpc::ClientBidiReactor<RouteNote, RouteNote> {
     public:
      explicit Chatter(RouteGuide::Stub* stub)
          : notes_{proto_utils::MakeRouteNote("First message", 1, 1),
                   proto_utils::MakeRouteNote("Second message", 2, 2),
                   proto_utils::MakeRouteNote("Third message", 3, 3),
                   proto_utils::MakeRouteNote("First message again", 1, 1)},
            notes_iterator_(notes_.begin()) {
        logger_.info("ENTER    |");
        stub->async()->RouteChat(&context_, this);
        logger_.info("ENTER    | NextWrite");
        NextWrite();
        logger_.info("ENTER    | StartRead");
        StartRead(&server_note_);
        logger_.info("ENTER    | StartCall");
        StartCall();
      }
      void OnWriteDone(bool ok) override {
        logger_.info("         | OnWriteDone() OK: {} alarm_.Set()", ok);
        // Delay and then do the next write or WritesDone
        alarm_.Set(std::chrono::system_clock::now() + std::chrono::milliseconds(proto_utils::GetRandomTimeDelay()),
                   [this](const bool not_cancelled) {
                     if (!not_cancelled) {
                       logger_.info("REQUEST  | cancelled");
                       return;
                     }
                     NextWrite();
                   });
      }
      void OnReadDone(bool ok) override {
        logger_.info("         | OnReadDone() OK: {}", ok);
        if (ok) {
          logger_.info("RESPONSE | RouteNote: {}", server_note_.ShortDebugString());
          logger_.info("         | StartRead");
          StartRead(&server_note_);
        }
      }
      void OnDone(const Status& status) override {
        std::unique_lock<std::mutex> l(mu_);
        logger_.info("EXIT     | OnDone() Status: OK: {} msg: {}", status.ok(), status.error_message());
        status_ = status;
        done_ = true;
        cv_.notify_one();
      }
      Status Await() {
        std::unique_lock<std::mutex> l(mu_);
        logger_.info("EXIT     | pre-wait()");
        cv_.wait(l, [this] { return done_; });
        logger_.info("EXIT     | post-wait()");
        return std::move(status_);
      }

     private:
      void NextWrite() {
        std::unique_lock<std::mutex> l(mu_);
        if (done_) {
          logger_.info("REQUEST  | was done_");
          return;  // No more pending writes, the stream is terminated
        }
        if (notes_iterator_ != notes_.end()) {
          const auto& note = *notes_iterator_;
          logger_.info("REQUEST  | RouteNote: {}", note.ShortDebugString());
          StartWrite(&note);
          ++notes_iterator_;
        } else {
          logger_.info("EXIT     | StartWritesDone");
          StartWritesDone();
        }
      }
      ClientContext context_;
      spdlog::logger& logger_ = *logger_RouteChat;
      const std::vector<RouteNote> notes_;
      std::vector<RouteNote>::const_iterator notes_iterator_;
      RouteNote server_note_;
      grpc::Alarm alarm_;  // To postpone an action to do later in the eventloop (handled by gRPC in its own thread pool)
      std::mutex mu_;
      std::condition_variable cv_;
      Status status_;
      bool done_ = false;
    };

    Chatter chatter(stub_.get());
    Status status = chatter.Await();
    logger_RouteChat->info("EXIT     | post-Await() OK: {} msg: {}", status.ok(), status.error_message());
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
