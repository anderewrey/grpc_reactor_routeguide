///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2015 gRPC authors
/// Copyright 2024-2025 anderewrey
///

#include <grpc/grpc.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>
#include <grpcpp/server_context.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <gflags/gflags.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <memory>
#include <string>
#include <thread>
#include <vector>

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

// Create and return a shared_ptr to a multithreaded console logger.
auto logger_GetFeature = spdlog::stdout_color_mt("GetFeature");
auto logger_ListFeatures = spdlog::stdout_color_mt("ListFeatures");
auto logger_RecordRoute = spdlog::stdout_color_mt("RecordRoute");
auto logger_RouteChat = spdlog::stdout_color_mt("RouteChat");
}  // anonymous namespace

class RouteGuideImpl final : public RouteGuide::CallbackService {
 public:
  grpc::ServerUnaryReactor* GetFeature(CallbackServerContext* context,
                                       const Point* point,
                                       Feature* feature) override {
    auto& logger = *logger_GetFeature;
    logger.info("ENTER    |");
    logger.info("REQUEST  | Point: {}", proto_utils::ToString(*point));
    *feature = proto_utils::GetFeatureFromPoint(feature_list_, *point);
    auto* reactor = context->DefaultReactor();
    logger.info("RESPONSE | Feature: {}", proto_utils::ToString(*feature));
    reactor->Finish(Status::OK);
    logger.info("EXIT     |");
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
        logger_.info("ENTER    |");
        logger_.info("REQUEST  | Rectangle: {}", proto_utils::ToString(rectangle_));
        NextWrite();
      }
      void OnDone() override {
        logger_.info("EXIT     |");
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
            logger_.info("RESPONSE | Feature: {}", proto_utils::ToString(f));
            StartWrite(&f);
            return;
          }
        }
        // Didn't write anything, all is done.
        logger_.info("EXIT     | Pre-Finish()");
        Finish(Status::OK);
        logger_.info("EXIT     | Post-Finish()");
      }
      spdlog::logger& logger_ = *logger_ListFeatures;
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
        logger_.info("ENTER    |");
        StartRead(&point_);
      }
      void OnDone() override {
        logger_.info("EXIT     |");
        delete this;
      }
      void OnReadDone(const bool ok) override {
        if (ok) {
          logger_.info("REQUEST  | Point: {}", proto_utils::ToString(point_));
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
          summary_.set_distance(static_cast<int32_t>(distance_));
          using namespace std::chrono;
          auto secs = duration_cast<seconds>(system_clock::now() - start_time_).count();
          summary_.set_elapsed_time(secs);
          logger_.info("RESPONSE | RouteSummary: {}", proto_utils::ToString(summary_));
          Finish(Status::OK);
        }
      }

     private:
      system_clock::time_point start_time_ = system_clock::now();
      RouteSummary& summary_;
      const FeatureList& feature_list_;
      spdlog::logger& logger_ = *logger_RecordRoute;
      Point point_;
      int point_count_ = 0;
      int feature_count_ = 0;
      double distance_ = 0.0;
      Point previous_;
    };
    return new Recorder(*summary, feature_list_);
  }

  grpc::ServerBidiReactor<RouteNote, RouteNote>* RouteChat(CallbackServerContext* context) override {
    class Chatter : public grpc::ServerBidiReactor<RouteNote, RouteNote> {
     public:
      Chatter(std::mutex& mu, std::vector<RouteNote>& received_notes)
          : mu_(mu), received_notes_(received_notes) {
        logger_.info("ENTER    |");
        StartRead(&note_);
      }
      void OnDone() override {
        logger_.info("EXIT     | OnDone()");
        delete this;
      }
      void OnReadDone(const bool ok) override {
        if (ok) {
          if (note_.message().empty()) {
            logger_.info("RESPONSE | RouteNote: {}", proto_utils::ToString(note_));
            StartWriteAndFinish(&note_, grpc::WriteOptions(), Status::OK);
            logger_.info("EXIT     | StartWriteAndFinish()");
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
          logger_.info("REQUEST  | RouteNote: {}", proto_utils::ToString(note_));
          std::ranges::copy_if(received_notes_, std::back_inserter(to_send_notes_),
                               [this](const RouteNote& note) {
                                 return (note.location() == note_.location());
                               });
          mu_.unlock();
          notes_iterator_ = to_send_notes_.begin();
          NextWrite();
        } else {
          logger_.info("EXIT     | Pre-Finish()");
          Finish(Status::OK);
          logger_.info("EXIT     | Post-Finish()");
        }
      }
      void OnWriteDone(bool /*ok*/) override { NextWrite(); }

     private:
      void NextWrite() {
        if (notes_iterator_ != to_send_notes_.end()) {
          logger_.info("RESPONSE | RouteNote: {}", proto_utils::ToString(*notes_iterator_));
          StartWrite(&*notes_iterator_);
          ++notes_iterator_;
        } else {
          mu_.lock();
          received_notes_.push_back(note_);
          mu_.unlock();
          logger_.info("         | no more response, waiting for next read");
          StartRead(&note_);
        }
      }
      RouteNote note_;
      spdlog::logger& logger_ = *logger_RouteChat;
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

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  feature_list_ = db_utils::GetDbFileContent();
  RunServer();
  return 0;
}
