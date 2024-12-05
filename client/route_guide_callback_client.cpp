/*
 *
 * Copyright 2021 gRPC authors.
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
#include <grpcpp/alarm.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/credentials.h>

#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

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
}  // anonymous namespace

class RouteGuideClient {
 public:
  explicit RouteGuideClient(const std::shared_ptr<Channel>& channel)
      : stub_(RouteGuide::NewStub(channel)) {}

  void GetFeature() {
    auto get_feature = [stub_ = stub_.get()](const Point& point, Feature& feature) {
      std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
      ClientContext context;
      bool result;
      std::mutex mu;
      std::condition_variable cv;
      bool done = false;
      std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] REQUEST  | Point: " << point.ShortDebugString()  << std::endl;
      stub_->async()->GetFeature(&context, &point, &feature, [&result, &mu, &cv, &done, &feature](Status status) {
        std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] RESPONSE | Status: OK:" << status.ok() << " msg:" << status.error_message() << " Feature: " << feature.ShortDebugString()  << std::endl;
        std::lock_guard<std::mutex> lock(mu);
        result = (status.ok() && feature.has_location());
        done = true;
        std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] EXIT     | cv.notify_one()" << std::endl;
        cv.notify_one();
      });
      std::unique_lock<std::mutex> lock(mu);
      std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] EXIT     | waiting for cv.wait()" << std::endl;
      cv.wait(lock, [&done] { return done; });
      std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] EXIT     | return " << result << std::endl;
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
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] REQUEST  | Rectangle: " << rectangle.ShortDebugString()  << std::endl;
        stub->async()->ListFeatures(&context_, &rectangle, this);
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] ENTER    | StartRead" << std::endl;
        StartRead(&feature_);
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] ENTER    | StartCall" << std::endl;
        StartCall();
      }
      void OnReadDone(bool ok) override {
        if (ok) {
          std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] RESPONSE | OK:" << ok << " Feature: " << feature_.ShortDebugString()  << std::endl;
          StartRead(&feature_);
        } else {
          std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     | OnReadDone() OK:" << ok << std::endl;
        }
      }
      void OnDone(const Status& status) override {
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     | OnDone() Status: OK:" << status.ok() << " msg:" << status.error_message() << std::endl;
        std::unique_lock<std::mutex> l(mu_);
        status_ = status;
        done_ = true;
        cv_.notify_one();
      }
      Status Await() {
        std::unique_lock<std::mutex> l(mu_);
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     | pre-wait()" << std::endl;
        cv_.wait(l, [this] { return done_; });
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     | post-wait()" << std::endl;
        return std::move(status_);
      }

     private:
      ClientContext context_;
      Feature feature_;
      std::mutex mu_;
      std::condition_variable cv_;
      Status status_;
      bool done_ = false;
    };

    Reader reader(stub_.get(), proto_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000));
    Status status = reader.Await();
    std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     | post-Await() OK:" << status.ok() << " msg:" << status.error_message() << std::endl;
  }

  void RecordRoute() {
    class Recorder : public grpc::ClientWriteReactor<Point> {
     public:
      Recorder(RouteGuide::Stub* stub, const FeatureList& feature_list)
          : feature_list_(feature_list) {
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
        stub->async()->RecordRoute(&context_, &summary_, this);
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] ENTER    | NextWrite" << std::endl;
        NextWrite();
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] ENTER    | StartCall" << std::endl;
        StartCall();
      }
      void OnWriteDone(bool ok) override {
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "]          | OnWriteDone() OK: " << ok << " alarm_.Set()" << std::endl;
        // Delay and then do the next write or WritesDone
        alarm_.Set(std::chrono::system_clock::now() + std::chrono::milliseconds(proto_utils::GetRandomTimeDelay()),
                   [this](const bool not_cancelled) {
                     if (!not_cancelled) {
                       std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] REQUEST  | cancelled" << std::endl;
                       return;
                     }
                     NextWrite();
                   });
      }
      void OnDone(const Status& status) override {
        std::unique_lock<std::mutex> l(mu_);
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] RESPONSE | OnDone() Status: OK:" << status.ok() << " msg:" << status.error_message() << " Summary: " << summary_.ShortDebugString()  << std::endl;
        status_ = status;
        done_ = true;
        cv_.notify_one();
      }
      Status Await(RouteSummary& summary) {
        std::unique_lock<std::mutex> l(mu_);
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] EXIT     | pre-wait()" << std::endl;
        cv_.wait(l, [this] { return done_; });
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] EXIT     | post-wait()" << std::endl;
        swap(summary, summary_);
        return std::move(status_);
      }

     private:
      void NextWrite() {
        std::unique_lock<std::mutex> l(mu_);
        if (done_) {
          std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] REQUEST  | was done_ " << std::endl;
          return;  // No more pending writes, the stream is terminated
        }
        if (points_remaining_ != 0) {
          const Point& point = proto_utils::GetRandomPoint(feature_list_);
          std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] REQUEST  | Point: " << point.ShortDebugString()  << std::endl;
          StartWrite(&point);
          points_remaining_--;
        } else {
          std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] EXIT     | StartWritesDone" << std::endl;
          StartWritesDone();
        }
      }
      ClientContext context_;
      int points_remaining_ = 10;
      RouteSummary summary_;
      const FeatureList& feature_list_;
      grpc::Alarm alarm_;  // To postpone an action to do later in the eventloop (handled by gRPC in its own thread pool)
      std::mutex mu_;
      std::condition_variable cv_;
      Status status_;
      bool done_ = false;
    };
    RouteSummary summary;
    Recorder recorder(stub_.get(), feature_list_);
    Status status = recorder.Await(summary);
    std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] EXIT     | post-Await() OK:" << status.ok() << " msg:" << status.error_message() << " RouteSummary: " << summary.ShortDebugString()  << std::endl;
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
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
        stub->async()->RouteChat(&context_, this);
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] ENTER    | NextWrite" << std::endl;
        NextWrite();
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] ENTER    | StartRead" << std::endl;
        StartRead(&server_note_);
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] ENTER    | StartCall" << std::endl;
        StartCall();
      }
      void OnWriteDone(bool ok) override {
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "]          | OnWriteDone() OK: " << ok << " alarm_.Set()" << std::endl;
        // Delay and then do the next write or WritesDone
        alarm_.Set(std::chrono::system_clock::now() + std::chrono::milliseconds(proto_utils::GetRandomTimeDelay()),
                   [this](const bool not_cancelled) {
                     if (!not_cancelled) {
                       std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] REQUEST  | cancelled" << std::endl;
                       return;
                     }
                     NextWrite();
                   });
      }
      void OnReadDone(bool ok) override {
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "]          | OnReadDone() OK:" << ok << std::endl;
        if (ok) {
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] RESPONSE | RouteNote: " << server_note_.ShortDebugString()  << std::endl;
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "]          | StartRead" << std::endl;
          StartRead(&server_note_);
        }
      }
      void OnDone(const Status& status) override {
        std::unique_lock<std::mutex> l(mu_);
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | OnDone() Status: OK:" << status.ok() << " msg:" << status.error_message() << std::endl;
        status_ = status;
        done_ = true;
        cv_.notify_one();
      }
      Status Await() {
        std::unique_lock<std::mutex> l(mu_);
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | pre-wait()" << std::endl;
        cv_.wait(l, [this] { return done_; });
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | post-wait()" << std::endl;
        return std::move(status_);
      }

     private:
      void NextWrite() {
        std::unique_lock<std::mutex> l(mu_);
        if (done_) {
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] REQUEST  | was done_ " << std::endl;
          return;  // No more pending writes, the stream is terminated
        }
        if (notes_iterator_ != notes_.end()) {
          const auto& note = *notes_iterator_;
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] REQUEST  | RouteNote: " << note.ShortDebugString()  << std::endl;
          StartWrite(&note);
          ++notes_iterator_;
        } else {
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | StartWritesDone" << std::endl;
          StartWritesDone();
        }
      }
      ClientContext context_;
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
    std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | post-Await() OK:" << status.ok() << " msg:" << status.error_message() << std::endl;
  }

 private:
  std::unique_ptr<RouteGuide::Stub> stub_;
};

int main(int argc, char** argv) {
  assert(main_thread == std::this_thread::get_id());
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  db_utils::ParseDb(db_utils::GetDbFileContent(argc, argv), feature_list_);
  RouteGuideClient guide(
      grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));

  std::cout << "-------------- GetFeature --------------" << std::endl;
  guide.GetFeature();
  std::cout << "-------------- ListFeatures --------------" << std::endl;
  guide.ListFeatures();
  std::cout << "-------------- RecordRoute --------------" << std::endl;
  guide.RecordRoute();
  std::cout << "-------------- RouteChat --------------" << std::endl;
  guide.RouteChat();

  return 0;
}
