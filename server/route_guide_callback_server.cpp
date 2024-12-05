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

using grpc::CallbackServerContext;
using grpc::Server;
using grpc::ServerBuilder;
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

class RouteGuideImpl final : public RouteGuide::CallbackService {
 public:
  grpc::ServerUnaryReactor* GetFeature(CallbackServerContext* context,
                                       const Point* point,
                                       Feature* feature) override {
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] REQUEST  | Point: " << point->ShortDebugString()  << std::endl;
    *feature = proto_utils::GetFeatureFromPoint(feature_list_, *point);
    auto* reactor = context->DefaultReactor();
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] RESPONSE | Feature: " << feature->ShortDebugString()  << std::endl;
    reactor->Finish(Status::OK);
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] EXIT     |" << std::endl;
    return reactor;
  }

  grpc::ServerWriteReactor<Feature>* ListFeatures(CallbackServerContext* context,
                                                  const Rectangle* rectangle) override {
    class Lister : public grpc::ServerWriteReactor<Feature> {
     public:
      Lister(const Rectangle& rectangle, const FeatureList& feature_list)
          : rectangle_(rectangle),
            feature_list_(feature_list),
            next_feature_(feature_list_.begin()) {
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] REQUEST  | Rectangle: " << rectangle_.ShortDebugString()  << std::endl;
        NextWrite();
      }
      void OnDone() override {
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     | OnDone()" << std::endl;
        delete this;
      }
      void OnWriteDone(bool /*ok*/) override { NextWrite(); }

     private:
      void NextWrite() {
        /* TODO(ajcote) Short-term objective:
      void NextWrite() {
        if (auto& feature = FindNextFeature(..)) {
          StartWrite(&feature);
        } else {
          // Didn't write anything, all is done.
          Finish(Status::OK);
        }
      }*/
        while (next_feature_ != feature_list_.end()) {
          const Feature& f = *next_feature_++;
          if (proto_utils::IsPointWithinRectangle(rectangle_, f.location())) {
            std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] RESPONSE | Feature: " << f.ShortDebugString()  << std::endl;
            StartWrite(&f);
            return;
          }
        }
        // Didn't write anything, all is done.
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     | Pre-Finish()" << std::endl;
        Finish(Status::OK);
        std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] EXIT     | Post-Finish()" << std::endl;
      }
      const Rectangle& rectangle_;
      const FeatureList& feature_list_;
      FeatureList::const_iterator next_feature_;
    };
    return new Lister(*rectangle, feature_list_);
  }

  grpc::ServerReadReactor<Point>* RecordRoute(CallbackServerContext* context,
                                              RouteSummary* summary) override {
    class Recorder : public grpc::ServerReadReactor<Point> {
     public:
      Recorder(RouteSummary& summary, const FeatureList& feature_list)
          : summary_(summary),
            feature_list_(feature_list) {
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
        StartRead(&point_);
      }
      void OnDone() override {
        std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] EXIT     |" << std::endl;
        delete this;
      }
      void OnReadDone(bool ok) override {
        if (ok) {
          std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] REQUEST  | Point: " << point_.ShortDebugString()  << std::endl;
          point_count_++;
          if (const auto name = proto_utils::GetFeatureName(point_, feature_list_); name && strlen(name) > 0) {
            feature_count_++;
          }
          if (point_count_ != 1) {
            distance_ += proto_utils::GetDistance(previous_, point_);
          }
          previous_ = point_;
          StartRead(&point_);
        } else {
          summary_.set_point_count(point_count_);
          summary_.set_feature_count(feature_count_);
          summary_.set_distance(static_cast<long>(distance_));
          using namespace std::chrono;
          auto secs = duration_cast<seconds>(system_clock::now() - start_time_).count();
          summary_.set_elapsed_time(secs);
          std::cout << "RouteGuide::RecordRoute[" << std::this_thread::get_id() << "] RESPONSE | RouteSummary: " << summary_.ShortDebugString()  << std::endl;
          Finish(Status::OK);
        }
      }

     private:
      system_clock::time_point start_time_ = system_clock::now();
      RouteSummary& summary_;
      const FeatureList& feature_list_;
      Point point_;
      int point_count_ = 0;
      int feature_count_ = 0;
      float distance_ = 0.0;
      Point previous_;
    };
    return new Recorder(*summary, feature_list_);
  }

  grpc::ServerBidiReactor<RouteNote, RouteNote>* RouteChat(CallbackServerContext* context) override {
    class Chatter : public grpc::ServerBidiReactor<RouteNote, RouteNote> {
     public:
      Chatter(std::mutex& mu, std::vector<RouteNote>& received_notes)
          : mu_(mu), received_notes_(received_notes) {
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] ENTER    |" << std::endl;
        StartRead(&note_);
      }
      void OnDone() override {
        std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | OnDone()" << std::endl;
        delete this;
      }
      void OnReadDone(bool ok) override {
        if (ok) {
          if (note_.message().empty()) {
            std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] RESPONSE | RouteNote: " << note_.ShortDebugString()  << std::endl;
            StartWriteAndFinish(&note_, grpc::WriteOptions(), Status::OK);
            std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | StartWriteAndFinish()" << std::endl;
            return;
          }
          // Unlike the other example in this directory that's not using
          // the reactor pattern, we can't grab a local lock to secure the
          // access to the notes vector, because the reactor will most likely
          // make us jump threads, so we'll have to use a different locking
          // strategy. We'll grab the lock locally to build a copy of the
          // list of nodes we're going to send, then we'll grab the lock
          // again to append the received note to the existing vector.
          mu_.lock();
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] REQUEST  | RouteNote: " << note_.ShortDebugString()  << std::endl;
          std::copy_if(received_notes_.begin(), received_notes_.end(),
                       std::back_inserter(to_send_notes_),
                       [this](const RouteNote& note) {
                         return proto_utils::AreEqual(note.location(), note_.location());
                       });
          mu_.unlock();
          notes_iterator_ = to_send_notes_.begin();
          NextWrite();
        } else {
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | Pre-Finish()" << std::endl;
          Finish(Status::OK);
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] EXIT     | Post-Finish()" << std::endl;
        }
      }
      void OnWriteDone(bool /*ok*/) override { NextWrite(); }

     private:
      void NextWrite() {
        if (notes_iterator_ != to_send_notes_.end()) {
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "] RESPONSE | RouteNote: " << notes_iterator_->ShortDebugString()  << std::endl;
          StartWrite(&*notes_iterator_);
          ++notes_iterator_;
        } else {
          mu_.lock();
          received_notes_.push_back(note_);
          mu_.unlock();
          std::cout << "RouteGuide::RouteChat[" << std::this_thread::get_id() << "]          | no more response, waiting for next read"  << std::endl;
          StartRead(&note_);
        }
      }
      RouteNote note_;
      std::mutex& mu_;
      std::vector<RouteNote>& received_notes_;
      std::vector<RouteNote> to_send_notes_;
      std::vector<RouteNote>::iterator notes_iterator_;
    };
    return new Chatter(mu_, received_notes_);
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
