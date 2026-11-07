///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <utility>

namespace RpcReactor {

/// Keeps only the newest value of a stream for a consumer slower than its producer, such as a display of a
/// realtime sensor: each value is worth handling only until a newer one exists.
///
/// An application-side helper, used by no reactor: the reactors deliver every message, and dropping the
/// stale ones is the application's choice. It pairs with ReadPacing::kContinuous, where the stream never
/// waits for the application, so the producer side runs inside the read_ok callback on a gRPC thread and
/// must not wait either. Put() only takes a short lock, never waits on the consumer, and reports whether
/// the consumer needs a notification, so a burst of values queues at most one event:
///
///     cbs.read_ok = [this](auto*, const Reading& reading) {      // gRPC thread
///       if (latest_.Put(reading)) EventLoop::TriggerEvent(kOnLatest);
///     };
///     void OnLatest(const EventLoop::Event*) {                     // application thread
///       if (auto reading = latest_.Take()) Display(*reading);
///     }
///
/// The notification carries no payload: the value waits in the slot, so the event owns nothing, and an
/// event dropped or handled twice leaks or frees nothing. Every value put is either taken or replaced, and
/// the newest is never lost: it stays in the slot until a Take(), so a consumer that also takes once the
/// stream has ended, from its done handler for instance, always ends on the last value.
///
/// Thread-safe: Put() and Take() may run concurrently, from any threads.
/// @tparam T value type, movable
template <class T>
class LatestSlot {
 public:
  /// Stores a value as the newest, replacing the previous one if nobody took it yet.
  /// @param value the new value
  /// @return true when the slot was empty, so the consumer has no notification pending and must be sent
  ///         one; false when a value was replaced, whose notification is still pending and now covers this
  ///         one
  [[nodiscard]] bool Put(T value) {
    const std::lock_guard lock(mutex_);
    const bool was_empty = !value_.has_value();
    if (!was_empty) ++replaced_;
    value_ = std::move(value);
    return was_empty;
  }

  /// Takes the newest value, leaving the slot empty, so the next Put() asks for a notification again.
  /// @return the newest value, or std::nullopt when none was put since the last Take()
  std::optional<T> Take() {
    const std::lock_guard lock(mutex_);
    return std::exchange(value_, std::nullopt);
  }

  /// @return how many values were replaced before anyone took them
  [[nodiscard]] std::uint64_t Replaced() const {
    const std::lock_guard lock(mutex_);
    return replaced_;
  }

 private:
  mutable std::mutex mutex_;
  std::optional<T> value_;
  std::uint64_t replaced_{0};
};

}  // namespace RpcReactor
