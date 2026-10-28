///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>

#include <atomic>
#include <cassert>
#include <coroutine>
#include <thread>

#include "applications/reactor/coro_task.h"
#include "applications/reactor/reactor_eventloop.h"

namespace RpcReactor::co {

/// The executor of the applications: resumes every coroutine on the application thread, the one that runs
/// the EventLoop handlers.
///
/// Post() triggers kResumeEvent with the handle's address, from whatever thread calls it, such as a gRPC
/// callback. The one handler for kResumeEvent, registered for the executor's lifetime through an
/// EventConnection, turns the address back into a handle and resumes it. The event queue is FIFO, so a
/// coroutine's resumptions run in the order they were posted, after every event queued before them.
///
/// One per process, created in main() before any coroutine starts, and destroyed after EventLoop::Run()
/// returns: a second registration of kResumeEvent would resume every handle twice, which EventConnection
/// documents.
///
/// Non-copyable and non-movable: the handler refers to the executor.
class EventLoopExecutor final : public Executor {
 public:
  /// The event that carries a handle to resume.
  static constexpr const char* kResumeEvent = "coro::resume";

  /// Registers the resume handler.
  /// @param app_thread the thread that runs the EventLoop handlers: the calling thread by default, as in
  ///        main() under EventLoop's default mode, where EventLoop::Run() runs the handlers on main()'s thread
  explicit EventLoopExecutor(std::thread::id app_thread = std::this_thread::get_id())
      : app_thread_(app_thread) {
    [[maybe_unused]] const bool was_live = live_.exchange(true);
    assert(!was_live && "one EventLoopExecutor per process");
  }

  ~EventLoopExecutor() override { live_.store(false); }

  EventLoopExecutor(const EventLoopExecutor&) = delete;
  EventLoopExecutor& operator=(const EventLoopExecutor&) = delete;
  EventLoopExecutor(EventLoopExecutor&&) = delete;
  EventLoopExecutor& operator=(EventLoopExecutor&&) = delete;

  /// Queues the handle on the event loop. Callable from any thread.
  void Post(std::coroutine_handle<> handle) override { EventLoop::TriggerEvent(kResumeEvent, handle.address()); }

 private:
  void Resume(const EventLoop::Event* event) const noexcept {
    assert(std::this_thread::get_id() == app_thread_ && "a coroutine resumes on the application thread only");
    std::coroutine_handle<>::from_address(event->getData()).resume();
  }

  static inline std::atomic<bool> live_{false};

  std::thread::id app_thread_;
  EventConnection connection_{kResumeEvent, [this](const EventLoop::Event* event) { Resume(event); }};
};

}  // namespace RpcReactor::co
