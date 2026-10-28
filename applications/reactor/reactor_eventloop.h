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
#include <functional>
#include <string>
#include <thread>
#include <utility>

#include "applications/reactor/coro_task.h"

/// The one file that adapts EventLoop, for both levels of the client: EventConnection for the callback
/// scenarios' handlers, and EventLoopExecutor for the coroutine scenarios. Replacing EventLoop means
/// replacing this file, and the calls the callback scenarios and the applications' main() make directly.
namespace RpcReactor {

/// RAII wrapper for EventLoop::RegisterEvent()/DeregisterEvent().
///
/// EventLoop::RegisterEvent() has no lifetime binding to the caller. The underlying library
/// appends a callback to a per-name list rather than replacing it, so registering the same event
/// name a second time while a prior registration is still active accumulates stale callbacks,
/// and every later TriggerEvent() call for that name then invokes all of them, including
/// callbacks that reference state their original owner has already destroyed.
///
/// An EventConnection ties the registration to whatever scope or object holds it. While it lives,
/// the callback is active. When it is destroyed, EventLoop::DeregisterEvent() runs automatically,
/// so RegisterEvent()/DeregisterEvent() stay symmetric without every call site having to remember.
///
/// Non-copyable and non-movable, since it owns exactly one registration for its own lifetime.
class EventConnection {
 public:
  /// Registers a callback for evt_name for the lifetime of this object.
  /// @param evt_name event name to register with EventLoop
  /// @param callback function invoked by EventLoop::TriggerEvent(evt_name, ...)
  EventConnection(std::string evt_name, const std::function<void(EventLoop::Event*)>& callback)
      : evt_name_(std::move(evt_name)) {
    EventLoop::RegisterEvent(evt_name_, callback);
  }

  /// Deregisters the event, so a later TriggerEvent(evt_name) no longer reaches this callback.
  ~EventConnection() {
    EventLoop::DeregisterEvent(evt_name_);
  }

  EventConnection(const EventConnection&) = delete;
  EventConnection& operator=(const EventConnection&) = delete;
  EventConnection(EventConnection&&) = delete;
  EventConnection& operator=(EventConnection&&) = delete;

 private:
  std::string evt_name_;
};

}  // namespace RpcReactor

namespace RpcReactor::coro {

/// The executor of the applications: resumes every coroutine on the application thread, the one that runs
/// the EventLoop handlers.
///
/// Post() triggers kResumeEvent with the handle's address, from whatever thread calls it, such as a gRPC
/// callback. The one handler for kResumeEvent, registered for the executor's lifetime through an
/// EventConnection, turns the address back into a handle and resumes it. The event queue is FIFO, so a
/// coroutine's resumptions run in the order they were posted, after every event queued before them.
///
/// One per process, created in main() before any coroutine starts, and destroyed after EventLoop::Run()
/// returns: a second registration of kResumeEvent would resume every handle twice, as EventConnection
/// above explains.
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

}  // namespace RpcReactor::coro
