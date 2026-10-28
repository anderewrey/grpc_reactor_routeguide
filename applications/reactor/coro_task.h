///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <atomic>
#include <cassert>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <type_traits>
#include <unordered_set>
#include <utility>

/// The core of the coroutine level: a task type, an owner for root tasks, the interface of the executor
/// that resumes them, and the hand-over every awaitable uses to reach that executor. It knows C++20
/// coroutines only, not gRPC, EventLoop or RouteGuide, so the server side can reuse it as it is.
///
/// Errors are values: no function here throws on purpose, and an exception that escapes a coroutine body
/// is a bug, which ends the process at the place it happened, see Task's unhandled_exception().
namespace RpcReactor::coro {

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

class RootTasks;
class ResumeHandover;

/// Which root task the calling thread runs, for the logs: on the application thread, the task whose code runs
/// now, or kNone between tasks. Spawn() numbers each root task from 1, and the task's number follows it across
/// every suspension made through ResumeHandover, and every task it awaits, since they run in its frame chain.
///
///     spdlog::info("task#{}", coro::CurrentTask::Get());
class CurrentTask {
 public:
  using Id = std::uint64_t;
  /// No task: the thread runs code outside any coroutine, such as an event handler.
  static constexpr Id kNone = 0;

  /// @return the task the calling thread runs, or kNone
  [[nodiscard]] static Id Get() noexcept { return current_; }

 private:
  friend RootTasks;
  friend ResumeHandover;

  /// Sets the current task until it is destroyed, then restores the one before.
  class Scope {
   public:
    explicit Scope(Id task) noexcept : previous_(std::exchange(current_, task)) {}
    ~Scope() { current_ = previous_; }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
    Scope(Scope&&) = delete;
    Scope& operator=(Scope&&) = delete;

   private:
    Id previous_;
  };

  static void Set(Id task) noexcept { current_ = task; }

  static inline thread_local Id current_ = kNone;
};

/// Hands a suspended coroutine from the thread that ends an operation, such as a gRPC callback, to the
/// executor, or lets it go on without suspending when the operation ended first. The building block of
/// every awaitable whose operation ends on another thread. Lock-free: the slot holds nothing, the waiting
/// coroutine's address, or kEnded.
///
/// It also carries the current task across the suspension, for the logs: Suspend() records it, and clears it
/// when the coroutine suspends, since the thread goes back to code outside the task. Resumed() restores it.
///
/// Single-shot: once ended, it stays ended.
class ResumeHandover {
 public:
  /// Called by the awaitable's await_suspend(), on the application thread.
  /// @param waiter the coroutine that awaits the operation
  /// @return true when the coroutine suspends until Complete() posts it; false when the operation has
  ///         already ended, so it goes on
  bool Suspend(std::coroutine_handle<> waiter) noexcept {
    task_ = CurrentTask::Get();
    void* expected = nullptr;  // NOLINT(misc-const-correctness): compare_exchange_strong() writes it
    const bool suspended = slot_.compare_exchange_strong(expected, waiter.address(), std::memory_order_acq_rel);
    if (suspended) CurrentTask::Set(CurrentTask::kNone);
    return suspended;
  }

  /// Called by the awaitable's await_resume(), first, on the application thread: the coroutine runs its task
  /// again.
  void Resumed() const noexcept { CurrentTask::Set(task_); }

  /// Called once the operation has ended, from whatever thread ends it, once what it records is in place:
  /// posts the waiting coroutine, if any, to the executor.
  void Complete(Executor& executor) {
    void* waiter = slot_.exchange(kEnded, std::memory_order_acq_rel);
    if (waiter != nullptr && waiter != kEnded) executor.Post(std::coroutine_handle<>::from_address(waiter));
  }

  /// Called by the awaitable's destructor, before it cancels its operation: an end that comes from then on
  /// posts nothing, since the coroutine it would resume is being destroyed.
  void Abandon() noexcept { slot_.exchange(kEnded, std::memory_order_acq_rel); }

 private:
  // Any address no coroutine frame can have.
  static inline char kEndedTag = 0;
  static inline void* const kEnded = &kEndedTag;
  std::atomic<void*> slot_{nullptr};
  CurrentTask::Id task_ = CurrentTask::kNone;  // written and read on the application thread only
};

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
  class Promise;

 public:
  /// The promise the language looks up. Its type is private: only the language and Task use it.
  using promise_type = Promise;

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
      std::coroutine_handle<Promise> handle;
      static bool await_ready() noexcept { return false; }
      bool await_suspend(std::coroutine_handle<> awaiter) const noexcept {
        return handle.promise().Start(handle, awaiter);
      }
      T await_resume() const noexcept(noexcept(handle.promise().TakeValue())) { return handle.promise().TakeValue(); }
    };
    assert(handle_ && !handle_.done() && "a Task is awaited once at most");
    return Awaiter{handle_};
  }

 private:
  /// What the promise has whatever T: lazy start, the one awaiter, and the exception policy.
  class PromiseBase {
   public:
    /// Lazy: the body starts only when the task is awaited, or spawned.
    static std::suspend_always initial_suspend() noexcept { return {}; }

    /// Resumes the awaiter once the body has finished: by symmetric transfer when the body suspended on the
    /// way, or by letting the awaiter go on without suspending when the body finished first, see Start().
    struct FinalAwaiter {
      static bool await_ready() noexcept { return false; }
      static std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> self) noexcept {
        PromiseBase& promise = self.promise();
        // The second of the body and Start() to get here resumes the awaiter. When the body is second, the
        // awaiter is suspended, and gets the thread by symmetric transfer.
        if (promise.second_.exchange(true, std::memory_order_acq_rel)) return promise.awaiter_;
        return std::noop_coroutine();  // the awaiter is still in Start(), which goes on without suspending
      }
      static void await_resume() noexcept {}
    };
    static FinalAwaiter final_suspend() noexcept { return {}; }

    /// An exception escaping a coroutine body is a bug: logs it on stderr, then ends the process. The
    /// promise stores no std::exception_ptr, so a frame finishes with a value or never finishes.
    [[noreturn]] static void unhandled_exception() noexcept {
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

  /// The common part, and how a body that co_returns a value hands it over.
  template <class U>
  class ReturnValue : public PromiseBase {
   public:
    void return_value(U value) noexcept(std::is_nothrow_move_constructible_v<U>) { value_.emplace(std::move(value)); }

    /// @return the value the body returned, moved out
    U TakeValue() noexcept(std::is_nothrow_move_constructible_v<U>) {
      assert(value_.has_value());
      return std::move(*value_);
    }

   private:
    std::optional<U> value_;
  };

  /// The common part, and how a body that co_returns nothing ends.
  class ReturnVoid : public PromiseBase {
   public:
    static void return_void() noexcept {}
    static void TakeValue() noexcept {}
  };

  /// The promise: the common part, and return_value() or return_void() as T asks. A base chosen by T, rather
  /// than a Promise<void> specialization, which GCC refuses inside a class.
  class Promise : public std::conditional_t<std::is_void_v<T>, ReturnVoid, ReturnValue<T>> {
   public:
    Task get_return_object() noexcept { return Task{std::coroutine_handle<Promise>::from_promise(*this)}; }
  };

  explicit Task(std::coroutine_handle<Promise> handle) noexcept : handle_(handle) {}

  void Release() noexcept {
    if (handle_) std::exchange(handle_, nullptr).destroy();
  }

  std::coroutine_handle<Promise> handle_;
};

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
    // Taken out first, so a destructor run by a frame's destruction never sees a half-cleared set.
    const auto roots = std::exchange(roots_, {});
    for (const auto root : roots) root.destroy();
  }

  RootTasks(const RootTasks&) = delete;
  RootTasks& operator=(const RootTasks&) = delete;
  RootTasks(RootTasks&&) = delete;
  RootTasks& operator=(RootTasks&&) = delete;

  /// Starts a task at once, on the calling thread, up to its first suspension, and keeps it until it
  /// completes. Not noexcept: the root frame is allocated.
  /// @param task a task not started yet
  /// @return the task's number, which CurrentTask gives while its code runs
  CurrentTask::Id Spawn(Task<> task) {
    const auto root = Root(std::move(task)).handle;
    root.promise().owner_ = this;
    roots_.insert(root);
    const CurrentTask::Id id = last_id_.fetch_add(1, std::memory_order_relaxed) + 1;
    const CurrentTask::Scope scope(id);
    root.resume();
    return id;
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
    static std::suspend_always initial_suspend() noexcept { return {}; }
    static auto final_suspend() noexcept {
      struct SelfDestroy {
        static bool await_ready() noexcept { return false; }
        static void await_suspend(RootHandle self) noexcept {
          self.promise().owner_->roots_.erase(self);
          self.destroy();
          CurrentTask::Set(CurrentTask::kNone);  // the thread goes back to code outside the task
        }
        static void await_resume() noexcept {}
      };
      return SelfDestroy{};
    }
    static void return_void() noexcept {}
    // Root() only awaits a Task, whose own promise ends the process first, so this never runs.
    [[noreturn]] static void unhandled_exception() noexcept { std::terminate(); }

    RootTasks* owner_ = nullptr;
  };

  static RootFrame Root(Task<> task) { co_await std::move(task); }

  std::unordered_set<RootHandle> roots_;
  // Shared by every owner, so task numbers are unique in the process, whichever thread spawns.
  static inline std::atomic<CurrentTask::Id> last_id_{CurrentTask::kNone};
};

}  // namespace RpcReactor::coro
