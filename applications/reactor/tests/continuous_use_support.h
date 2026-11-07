///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <grpcpp/grpcpp.h>

#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

#include "rg_service/route_guide_service.h"
#include "rg_service/rg_utils.h"

/// Support for the tests of what each ReadPacing::kContinuous delivery is best at: keeping only the newest
/// reading under Delivery::kPull, and journaling every reading under Delivery::kPush. Both run a reactor
/// against a server streaming readings back to back, to a consumer on an event loop of its own.
namespace continuous_use_test {

/// The number of readings ReadingService streams, back to back.
inline constexpr int kReadings = 200;

/// A single-threaded task queue standing in for the application's event loop: tasks run in the order they
/// were posted, one at a time, on the queue's own thread, as EventLoop runs events on the application thread.
class AppLoop {
 public:
  AppLoop() : thread_([this] { Run(); }) {}
  ~AppLoop() {
    Post(nullptr);
    thread_.join();
  }

  AppLoop(const AppLoop&) = delete;
  AppLoop& operator=(const AppLoop&) = delete;
  AppLoop(AppLoop&&) = delete;
  AppLoop& operator=(AppLoop&&) = delete;

  /// Queues a task, or the loop's end with nullptr.
  void Post(std::function<void()> task) {
    {
      const std::lock_guard lock(mutex_);
      tasks_.push_back(std::move(task));
    }
    cv_.notify_one();
  }

 private:
  void Run() {
    for (;;) {
      std::function<void()> task;
      {
        std::unique_lock lock(mutex_);
        cv_.wait(lock, [this] { return !tasks_.empty(); });
        task = std::move(tasks_.front());
        tasks_.pop_front();
      }
      if (!task) return;
      task();
    }
  }

  std::mutex mutex_;
  std::condition_variable cv_;
  std::deque<std::function<void()>> tasks_;
  std::thread thread_;
};

/// Streams kReadings features back to back, numbered from 1 in their longitude and their name, then finishes.
class ReadingService final : public routeguide::RouteGuide::CallbackService {
 public:
  grpc::ServerWriteReactor<routeguide::Feature>* ListFeatures(grpc::CallbackServerContext* /*context*/,
                                                              const routeguide::Rectangle* /*rect*/) override {
    class Reactor : public grpc::ServerWriteReactor<routeguide::Feature> {
     public:
      Reactor() { Next(); }
      void OnWriteDone(bool ok) override {
        if (!ok) {
          Finish(grpc::Status(grpc::StatusCode::UNKNOWN, "write failed"));
          return;
        }
        Next();
      }
      void OnDone() override { delete this; }

     private:
      void Next() {
        if (sent_ == kReadings) {
          Finish(grpc::Status::OK);
          return;
        }
        ++sent_;
        feature_ = rg_utils::MakeFeature("reading " + std::to_string(sent_), 1, sent_);
        StartWrite(&feature_);
      }
      routeguide::Feature feature_;
      int sent_ = 0;
    };
    return new Reactor();
  }
};

}  // namespace continuous_use_test
