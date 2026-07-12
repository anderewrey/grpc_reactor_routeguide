///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>

#include <functional>
#include <string>
#include <utility>

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
  EventConnection(std::string evt_name, std::function<void(EventLoop::Event*)> callback)
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
