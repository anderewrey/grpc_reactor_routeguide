///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <list>
#include <optional>
#include <type_traits>
#include <utility>

/// The core of the coroutine level: a task type, an owner for root tasks, and the interface of the executor
/// that resumes them. It knows C++20 coroutines only, not gRPC, EventLoop or RouteGuide, so the server
/// side can reuse it as it is.
///
/// Errors are values: no function here throws on purpose, and an exception that escapes a coroutine body
/// is a bug, which ends the process at the place it happened, see PromiseBase::unhandled_exception().
namespace RpcReactor::co {

/// Resumes coroutine handles later, on a thread of its own choosing: the application thread, for every
/// executor of this level.
///
/// An awaitable takes the executor as a parameter and hands it the suspended coroutine's handle, from
/// whatever thread completes the operation, such as a gRPC callback. It never resumes the handle itself.
class Executor {
 public:
  virtual ~Executor() = default;

  /// Queues a handle for resumption. Callable from any thread. Not noexcept: queueing may allocate.
  /// @param handle a suspended coroutine, which must stay suspended and alive until the executor resumes it
  virtual void Post(std::coroutine_handle<> handle) = 0;

 protected:
  Executor() = default;
  Executor(const Executor&) = default;
  Executor& operator=(const Executor&) = default;
  Executor(Executor&&) = default;
  Executor& operator=(Executor&&) = default;
};

template <class T>
class Task;

namespace detail {

/// What Task<T>'s promise has whatever T: lazy start, the one awaiter, and the exception policy.
class PromiseBase {
 public:
  /// Lazy: the body starts only when the task is awaited, or spawned.
  std::suspend_always initial_suspend() const noexcept { return {}; }

  /// Resumes the awaiter once the body has finished: by symmetric transfer when the body suspended on the
  /// way, or by letting the awaiter go on without suspending when the body finished first, see Start().
  struct FinalAwaiter {
    bool await_ready() const noexcept { return false; }
    template <class Promise>
    std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) const noexcept {
      PromiseBase& promise = self.promise();
      // The second of the body and Start() to get here resumes the awaiter. When the body is second, the
      // awaiter is suspended, and gets the thread by symmetric transfer.
      if (promise.second_.exchange(true, std::memory_order_acq_rel)) return promise.awaiter_;
      return std::noop_coroutine();  // the awaiter is still in Start(), which goes on without suspending
    }
    void await_resume() const noexcept {}
  };
  FinalAwaiter final_suspend() const noexcept { return {}; }

  /// An exception escaping a coroutine body is a bug: logs it on stderr, then ends the process. The
  /// promise stores no std::exception_ptr, so a frame finishes with a value or never finishes.
  [[noreturn]] void unhandled_exception() const noexcept {
    try {
      throw;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "coroutine: unhandled exception: %s\n", e.what());
    } catch (...) {
      std::fputs("coroutine: unhandled exception of unknown type\n", stderr);
    }
    std::terminate();
  }

  /// Runs the body for its one awaiter, up to its first suspension or its end.
  ///
  /// A body that finishes without suspending, such as a read from a buffer that already holds a message,
  /// lets the awaiter go on without suspending at all: no handle is resumed, and the stack is back where
  /// it was. A loop of such awaits therefore runs in constant stack depth whatever the compiler does.
  /// Symmetric transfer alone does not promise that: GCC compiles it to a nested call below -O2, which
  /// adds about 128 bytes of stack per await.
  /// @param self the task's own frame
  /// @param awaiter the coroutine awaiting the task
  /// @return true when the body suspended, so the awaiter suspends until the body resumes it; false when
  ///         the body has already finished, so the awaiter goes on
  template <class Promise>
  bool Start(std::coroutine_handle<Promise> self, std::coroutine_handle<> awaiter) noexcept {
    assert(!awaiter_ && "a Task is awaited once at most");
    awaiter_ = awaiter;
    self.resume();
    return !second_.exchange(true, std::memory_order_acq_rel);
  }

 private:
  std::coroutine_handle<> awaiter_;
  // Set by the first of the body's end and Start()'s return, so the second knows the first has passed.
  // Atomic for an executor that resumes the body on another thread while Start() still runs.
  std::atomic<bool> second_{false};
};

template <class T>
class Promise : public PromiseBase {
 public:
  Task<T> get_return_object() noexcept;

  void return_value(T value) noexcept(std::is_nothrow_move_constructible_v<T>) { value_.emplace(std::move(value)); }

  /// @return the value the body returned, moved out
  T TakeValue() noexcept(std::is_nothrow_move_constructible_v<T>) {
    assert(value_.has_value());
    return std::move(*value_);
  }

 private:
  std::optional<T> value_;
};

template <>
class Promise<void> : public PromiseBase {
 public:
  Task<void> get_return_object() noexcept;

  void return_void() const noexcept {}
  void TakeValue() const noexcept {}
};

}  // namespace detail

/// A lazily started coroutine that produces one T, or nothing for Task<void>, for the one coroutine that
/// awaits it.
///
/// | Property              | Choice                                                             |
/// |-----------------------|--------------------------------------------------------------------|
/// | Start                 | Lazy: the body starts when the task is awaited or spawned          |
/// | Awaiters              | One at most: `co_await std::move(task)`, or a temporary            |
/// | Resumption of awaiter | Symmetric transfer after a suspension, none without, so no stack   |
/// |                       | grows when a body never suspends                                   |
/// | Ownership             | The Task owns its frame, and destroying it destroys the frame,     |
/// |                       | finished, not started, or suspended                                |
///
/// Destroying a suspended frame destroys its locals, and with them any Task it was awaiting, so a whole
/// chain of suspended frames is released from its root. A frame must never be destroyed while it runs, or
/// while an executor still holds its handle.
///
/// Move-only.
/// @tparam T the value the body co_returns, movable, not a reference
template <class T = void>
class [[nodiscard]] Task {
  static_assert(!std::is_reference_v<T>, "a Task returns an owned value, never a reference");

 public:
  using promise_type = detail::Promise<T>;

  Task(Task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}
  Task& operator=(Task&& other) noexcept {
    if (this != &other) {
      Release();
      handle_ = std::exchange(other.handle_, nullptr);
    }
    return *this;
  }
  Task(const Task&) = delete;
  Task& operator=(const Task&) = delete;
  ~Task() { Release(); }

  /// Starts the body, and resumes the awaiting coroutine with its value once the body co_returns.
  auto operator co_await() && noexcept {
    struct Awaiter {
      std::coroutine_handle<promise_type> handle;
      bool await_ready() const noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> awaiter) const noexcept {
        return handle.promise().Start(handle, awaiter);
      }
      T await_resume() const noexcept(noexcept(handle.promise().TakeValue())) { return handle.promise().TakeValue(); }
    };
    assert(handle_ && !handle_.done() && "a Task is awaited once at most");
    return Awaiter{handle_};
  }

 private:
  friend promise_type;
  explicit Task(std::coroutine_handle<promise_type> handle) noexcept : handle_(handle) {}

  void Release() noexcept {
    if (handle_) std::exchange(handle_, nullptr).destroy();
  }

  std::coroutine_handle<promise_type> handle_;
};

namespace detail {

template <class T>
Task<T> Promise<T>::get_return_object() noexcept {
  return Task<T>{std::coroutine_handle<Promise<T>>::from_promise(*this)};
}

inline Task<void> Promise<void>::get_return_object() noexcept {
  return Task<void>{std::coroutine_handle<Promise<void>>::from_promise(*this)};
}

}  // namespace detail

/// Owns the root tasks: the ones no coroutine awaits, such as one scenario per RPC. Spawn() starts a root
/// task, and the owner keeps its frame until the task completes, then destroys it. Its destructor destroys
/// the frames still suspended, which releases everything they own.
///
/// Not thread-safe: Spawn(), the resumptions of the root tasks, and the owner's destruction all run on the
/// application thread. Destroy the owner only once no executor holds a handle of its tasks any more, after
/// the event loop has stopped for instance.
///
/// Non-copyable and non-movable: each root frame refers back to its owner.
class RootTasks {
 public:
  RootTasks() = default;
  ~RootTasks() {
    // Taken out first, so a destructor run by a frame's destruction never sees a half-cleared list.
    auto roots = std::exchange(roots_, {});
    for (auto root : roots) root.destroy();
  }

  RootTasks(const RootTasks&) = delete;
  RootTasks& operator=(const RootTasks&) = delete;
  RootTasks(RootTasks&&) = delete;
  RootTasks& operator=(RootTasks&&) = delete;

  /// Starts a task at once, on the calling thread, up to its first suspension, and keeps it until it
  /// completes. Not noexcept: the root frame is allocated.
  /// @param task a task not started yet
  void Spawn(Task<> task) {
    const auto root = Root(std::move(task)).handle;
    root.promise().owner_ = this;
    root.promise().position_ = roots_.insert(roots_.end(), root);
    root.resume();
  }

  /// @return how many root tasks have started and not completed yet
  [[nodiscard]] std::size_t Size() const noexcept { return roots_.size(); }

 private:
  struct RootPromise;
  using RootHandle = std::coroutine_handle<RootPromise>;

  /// A root frame: awaits the spawned task, then removes itself from its owner and destroys itself.
  struct RootFrame {
    using promise_type = RootPromise;
    RootHandle handle;
  };

  struct RootPromise {
    RootFrame get_return_object() noexcept { return RootFrame{RootHandle::from_promise(*this)}; }
    std::suspend_always initial_suspend() const noexcept { return {}; }
    auto final_suspend() const noexcept {
      struct SelfDestroy {
        bool await_ready() const noexcept { return false; }
        void await_suspend(RootHandle self) const noexcept {
          const RootPromise& promise = self.promise();
          promise.owner_->roots_.erase(promise.position_);
          self.destroy();
        }
        void await_resume() const noexcept {}
      };
      return SelfDestroy{};
    }
    void return_void() const noexcept {}
    // Root() only awaits a Task, whose own promise ends the process first, so this never runs.
    [[noreturn]] void unhandled_exception() const noexcept { std::terminate(); }

    RootTasks* owner_ = nullptr;
    std::list<RootHandle>::iterator position_;
  };

  static RootFrame Root(Task<> task) { co_await std::move(task); }

  std::list<RootHandle> roots_;
};

}  // namespace RpcReactor::co
