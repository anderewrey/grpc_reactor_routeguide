# Client coroutines: design draft

> [!NOTE]
> This is a design draft, built step by step. Steps 1 to 3 of the [suggested order](#suggested-order) exist, and
> [reactor_coro.md](/applications/reactor/reactor_coro.md) is their reference. Once the level is built and its claims
> are proven by tests and runs, this draft's reasoning moves into [architecture.md](/docs/architecture.md), and this
> file goes away.

## Goal

The client reactors run every callback on a gRPC thread, and the Active Object design moves each event to the
application thread. That keeps all application state on one thread, without locks. The price is the shape of the
code: one scenario is split across `OnReadOk`, `OnWriteDone` and `OnDone` handlers, and the state they share lives in
members, because a handler cannot see what the previous one was doing.

A C++20 coroutine keeps the thread guarantee and removes the split. The scenario becomes one function that suspends at
each `co_await` and resumes on the application thread:

```cpp
// Proposed API. Runs on the application thread from start to end.
coro::Task<void> TripRecorder::Run(routeguide::Rectangle rect) {
  using Reactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>;
  coro::Reader<Reactor, coro::Buffer::kUnbounded> reader{executor_, stub_, std::move(rect)};
  while (auto report = co_await reader.Read()) {
    Store(*report);  // no lock: the same thread as all application state
  }
  const grpc::Status status = co_await reader.Finish();
}
```

The milestone builds this level for the client first. The client already has the reactors to build on, tests with an
in-process server, and use cases to port and compare against. The server milestone then reuses the same coroutine
core.

## Where the coroutine level sits

The coroutine level is a new, generic level between the reactors and the application. It changes how the application
consumes a reactor, and nothing below that:

| Level | Files | Active Object component | Changed here |
|---|---|---|---|
| Infrastructure | EventLoop library | Scheduler, Activation Queue | No |
| Generic | `reactor_client.h` | Method Request, Future, Guard | No |
| Service-specific | `reactor_client_routeguide.h` | Method Request, bound to RouteGuide | No |
| Coroutine (new) | `coro_task.h`, `reactor_coro.h`, the executor in `reactor_eventloop.h` | An awaitable Future | New |
| Application | `variants/`, `usecases/`, and their `_coro` twins | Proxy, Servant | New `_coro` files only |

Two Active Object components change shape. The Future becomes something the application awaits, instead of
something a handler queries later with `Status()` or `GetResponse()`. The Servant becomes one function per RPC,
instead of one handler per event. The Scheduler and the Method Request do not change.

The level has one boundary rule: **no user code runs off the application thread.** Every coroutine resumes on the
application thread. The level's own code, such as the copy into a `LatestSlot`, may run inside a callback, but a
scenario's code never does. A scenario whose point is work inside the reaction, on the gRPC thread, therefore stays at
the callback level, see [Scenarios](#scenarios).

### Inside the level

Each of the three headers knows one thing more than the level below it, and nothing about the levels beside it:

- `coro_task.h` knows C++20 coroutines: `Task<T>`, `Spawn()`, an executor that resumes a handle, and the hand-over
  that reaches it. It does not know gRPC, EventLoop, or RouteGuide.
- `reactor_coro.h` knows the generic reactors of `reactor_client.h`, the task core, and the buffers, reusing
  `latest_slot.h`. It does not know EventLoop or RouteGuide.
- `reactor_eventloop.h` knows EventLoop: `EventConnection` for the callback level, and the executor that posts a handle
  and resumes it. It does not know gRPC or RouteGuide.

Every helper is either a private member of the class it serves, such as `Task<T>`'s promise, or a named building block
of the level, such as `ResumeHandover`, which every awaitable uses to hand its coroutine to the executor. There is no
`detail` namespace.

The awaitables take the executor as a parameter, not EventLoop. Their unit tests can then resume coroutines on the
single-threaded `AppLoop` task queue of `continuous_use_support.h`, with no EventLoop and no global state, and the
applications plug in the EventLoop executor.

The executor lives in `reactor_eventloop.h`, beside `EventConnection`, rather than in a header of its own: one file
adapts EventLoop for both levels, so replacing EventLoop replaces one adapter. The callback scenarios and each
application's `main()` still call EventLoop directly, which the coroutine scenarios never do.

The awaitables are templates on the reactor class, such as `routeguide::ListFeatures::ClientReactor<kPush,
kContinuous>`. Each awaitable constructs its reactor from the arguments the service-specific constructors already
take, a stub, a context, a request, and callbacks it supplies itself. So `reactor_client_routeguide.h` stays the one
service-specific file, and the coroutine level has nothing to know about RouteGuide. Should the call sites read too
heavy, a file of aliases beside `reactor_client_routeguide.h` can shorten them, as generatable as that file.

## Decisions

### Where a coroutine resumes: on the application thread

A reactor callback does not resume the coroutine itself. It hands the coroutine handle to the executor, which in the
applications posts it to the event loop:

```cpp
EventLoop::TriggerEvent(kResume, handle.address());  // from the gRPC thread
```

One handler for `kResume`, registered once by the executor, turns the address back into a handle and resumes it,
on the application thread. The executor owns that registration through an `RpcReactor::EventConnection`, created in
`main()` before any coroutine starts.

Resuming inline, on the gRPC thread, would be faster by one queue hop. It would also run user code on a gRPC thread,
which the boundary rule forbids. The event queue stays FIFO, so a coroutine's resumptions arrive in the order the
reactor produced them, after every event queued before them.

### Telling coroutines apart in the logs: the task, not the thread

The callback level tells its two sides apart by the thread id in the log pattern: a gRPC thread runs the callback,
and the application thread runs the handler. Every coroutine resumes on the application thread, so the thread id no
longer tells two coroutines apart. The coroutine applications log the running task instead, as `[task#N]` after the
thread id, or `[task#-]` outside any task.

`Spawn()` numbers each root task, and `CurrentTask` gives the number of the task the thread runs. The hand-over every
awaitable uses records the number when the coroutine suspends, clears it while the thread runs other code, and
restores it when the coroutine resumes. So no executor and no scenario carries the number. A custom spdlog flag prints
it, so the scenarios' log calls do not change.

### The task type: written here, not taken from a library

C++20 has coroutines in the language, but no library types: no `task`, and no `generator`. C++23 adds `std::generator`
but still no task type, so the level's core is written here whatever the standard. The level therefore defines a small
`Task<T>`:

| Property | Choice |
|---|---|
| Start | Lazy: the body starts when the task is awaited or spawned |
| Awaiters | One at most |
| Resumption of awaiter | Symmetric transfer after a suspension only: `await_suspend` returns the handle to resume |
| Ownership | The caller owns the frame; destroying the `Task` destroys a finished frame |
| Root tasks | `Spawn()` gives a root task to an owner that keeps it until it completes |
| Exceptions | Not relied on: `unhandled_exception()` logs and calls `std::terminate()` |

An awaitable can complete without suspending, for instance a `Read()` on a buffer that already holds a message. A loop
of such reads must not grow the stack with each iteration. Symmetric transfer alone does not promise that: GCC 13 and
14 compile it to a nested call below `-O2`, about 128 bytes of stack per await, and a million awaits overflow the
stack of a Debug build. Clang, and GCC at `-O2`, compile it to a jump. So a task that ends without suspending lets its
awaiter go on without any transfer: the awaiter starts the body with a call, and an atomic flag tells whichever of the
two finishes second to resume the awaiter. Only a body that suspended on the way resumes its awaiter by symmetric
transfer, from a resumption the executor started, so the stack stays bounded whatever the compiler does.

asio-grpc and stdexec solve the same problem with far more generality. Neither is a dependency here, for the reason
the reactors were written in this repository: the mechanics stay visible and testable, and the vcpkg build stays as it
is. Both remain good references.

### Errors: values, not exceptions

The level reports every failure as a value, as gRPC does. An RPC's outcome is the `grpc::Status` that `Finish()`
returns, the end of a stream is a `Read()` with no message, and a rejected write is a `Write()` that returns `false`.
No awaitable throws, and every function of the level that cannot fail is `noexcept`.

Exceptions are not excluded from the process: spdlog, protobuf and the standard containers can still throw, allocation
failure included. The level only never relies on one. An exception that escapes a coroutine body is a bug, so
`unhandled_exception()` logs it and calls `std::terminate()`, which ends the process loudly, at the place it happened.
Today an exception escaping an event handler stops `EventLoop::Run()` with a line on stderr, as the EventLoop library
catches it there, so the level is stricter than the handlers it sits beside.

Two consequences follow:

- **The promise type stores no `std::exception_ptr`.** A frame finishes with a value or never finishes, so there is no
  rethrow path to write and test, and teardown has no third case.
- **`noexcept` on a coroutine does not cover its body.** It only covers creating the frame: allocation, and building
  the promise. An exception from the body still goes to `unhandled_exception()`, which is why that function, not the
  `noexcept`, is where the policy lives.

### What an awaited message is: always owned

A message lent by `kPull`'s `read_ok` lives in the reactor's read target, which the next read overwrites. A reference
to it that survives a `co_await` dangles as soon as the next read completes, and nothing in the code shows it. So every
awaitable hands the coroutine an owned message. `Delivery` stays, because it decides how the message becomes owned, and
what that costs:

| Delivery | How the coroutine gets an owned message | Cost per message | Read-ahead |
|---|---|---|---|
| `kPull` | `Read()` swaps it out with `GetResponse()` | No allocation | `GetResponse()` arms the next read at once |
| `kPush` | `Read()` takes the `std::unique_ptr` the callback hands out | One allocation | None until the next `Read()` |

The read-ahead column is the difference `turn_by_turn_guidance` already turns on. Under `kPull`, `GetResponse()` takes
the message and arms the next read in one call, so the next message can arrive while the coroutine works on this one.
Under `kPush`, the level calls `ResumeRead()` only when the coroutine asks for the next message, so nothing is read
until then. Only a buffered reader, below, holds several messages at once, so only `kPush` can feed one.

### Pacing as a buffer between the reactor and the coroutine

A coroutine that awaits each read takes one message at a time, which is `kTurnByTurn` by construction. The other
pacings put a buffer between the reactor and the coroutine:

| Reader | Reactor underneath | Buffer policy |
|---|---|---|
| No buffer | `kTurnByTurn`, either delivery | None: one message in flight |
| `Buffer::kUnbounded` | `kPush`, `kContinuous` | Every message queued, the stream never waits |
| `Buffer::kLatest` | `kPull`, `kContinuous`, with a `LatestSlot` | One slot, overwritten, the stream never waits |
| `Buffer::kBounded` | `kPush`, `kPerMessage` | Holds the stream once a given number of messages wait |

`kLatest` copies each lent message into the slot inside the callback, as `live_vehicle_position` does today. That copy
is the level's own code, so the boundary rule holds, and it makes `kLatest` the one buffered reader that runs on
`kPull`.

The level offers no filter. A decision taken inside `read_ok`, which declines a message at no cost, would run a
scenario's code on the gRPC thread. It would also have to be fixed when the reader is created, since `read_ok` decides
when the message arrives, possibly before the coroutine calls `Read()`. A scenario that needs that decision stays at
the callback level.

### Writers: awaited writes, or a synchronous source

- `kApplicationPushes` fits points that appear over time, such as `live_route_recording`. The coroutine runs
  `co_await writer.Write(point)`, then `co_await writer.Close()`, and each write resumes on `write_done`.
- `kReactorPulls` fits a source ready at once, such as `gpx_track_upload`. The writer is given a range, and the reactor
  pulls from it.

The shape of the code enforces one write in flight: a coroutine cannot start a second `Write()` before the first one
resumes it. A coroutine that must wait for its next request, for a GPS fix or a typed note, needs
`kApplicationPushes`. `next_request` cannot answer "not yet", as
[Why the writing reactors also let the reactor pull](/docs/architecture.md#why-the-writing-reactors-also-let-the-reactor-pull)
explains, so a `kReactorPulls` source must be synchronous. The level takes any range, such as the prepared `std::vector`
`gpx_track_upload` sends today. That covers every source the use cases need, and a lazy one would need only a small
generator type beside `Task<T>`.

### The end of an RPC: `Finish()` is awaited exactly once

Every awaitable offers `co_await Finish()`, which resumes on `OnDone` and returns the status, and the final response
for a unary or writing RPC. A coroutine awaits it exactly once, after its last read or write. The awaitable destroys
its reactor after `OnDone`, in the frame, on the application thread, as the `OnDone` handlers do today.

A bidirectional RPC reads and writes at the same time, so it needs two flows: one coroutine reads, one writes, and a
join awaits both before `Finish()`. The writer ends its stream with `Close()`. The reader ends when `Read()` returns no
message. Only the joined task awaits `Finish()`.

### Teardown and cancellation

A suspended coroutine resumes only through the event loop. `EventLoop::Halt()` discards the events still queued, so a
coroutine whose resumption was queued, or is still to come, never runs again. Its frame, and the reactor in it, must
still be released, and a reactor must never be destroyed before its `OnDone()` has run. The level follows the rule
`TearDownReactor()` applies today:

1. Each awaitable's destructor calls `CancelAndWait()` on a reactor whose RPC is still in flight, and destroys the
   reactor only once `OnDone()` has run.
2. A root task's owner destroys its suspended frames at shutdown, which runs those destructors.
3. Should `OnDone()` never come, the reactor is leaked rather than freed under gRPC.

Cancelling an RPC while it runs goes through `context->TryCancel()`, as today. The suspended `Read()` then resumes with
no message, from `read_nok`, and `Finish()` returns `CANCELLED`. A cancellation token, such as `std::stop_token`, can
trigger it from the application thread.

### Traps the tooling can catch

- **Dangling coroutine parameters.** A reference parameter, or a lambda capture, outlives its referent once the
  coroutine suspends. clang-tidy has `cppcoreguidelines-avoid-reference-coroutine-parameters` and
  `cppcoreguidelines-avoid-capturing-lambda-coroutines`, to add to `.clang-tidy`. Coroutines take their arguments by
  value, as `Run(routeguide::Rectangle rect)` does above.
- **Data races.** A frame is touched by the gRPC thread that posts its handle and by the application thread that
  resumes it. The event queue orders the two. TSan, already in CI, checks it.
- **Use after free.** A frame destroyed while a gRPC operation still refers to it. ASan, already in CI, checks it, and
  the teardown rule above prevents it.

## Scenarios

The callback level is demonstrated by `variants/` and `usecases/`, written with callbacks, and they stay as they are.
The coroutine level is demonstrated by its own scenarios, written with awaitables. Each level's scenarios use that
level's own vocabulary, and none reaches across another level.

### Placement and naming

A coroutine scenario is a file next to its parent, the callback scenario whose reactor configuration it wraps, with a
`_coro` suffix. Its flavour is named by the filename and the namespace, never by the class, as for every scenario:

| Coroutine scenario | Namespace | Class |
|---|---|---|
| `variants/list_features_push_continuous_coro` | `rg_demo::push::continuous::coro` | `ListFeatures` |
| `usecases/trip_recorder_coro` | `rg_demo::usecase::coro` | `TripRecorder` |

Their parents are `variants/list_features_push_continuous` and `usecases/trip_recorder`.

Every coroutine scenario then has two diffs, each along one axis:

- **Against its parent, the style changes.** The same reactor configuration, or the same situation, consumed by
  callbacks, then by a coroutine.
- **A coroutine use case against its coroutine variant, the use changes.** The coroutine counterpart of the diffs the
  use cases' README lists today.

### Applications

The coroutine scenarios run in two applications of their own, so each run stays comparable with its parent's:

| Application                              | Runs                       | Compared with                       |
|------------------------------------------|----------------------------|-------------------------------------|
| `route_guide_active_reactor_client_coro` | The coroutine variants     | `route_guide_active_reactor_client` |
| `route_guide_reactor_usecases_coro`      | The coroutine use cases    | `route_guide_reactor_usecases`      |

Both link `rg_service` and EventLoop, compile `demo_support.cpp`, and join the llvm-cov step's object list.

### Coroutine variants: one per awaitable

The coroutine variants are not one per callback variant: pacing becomes a buffer, and `Delivery` matters to the reader
without a buffer only. One variant per awaitable covers the level, each next to the callback variant it wraps:

| Coroutine variant                               | Awaitable it shows                                   |
|-------------------------------------------------|------------------------------------------------------|
| `get_feature_pull_coro`                         | Unary `co_await Finish()`                            |
| `list_features_pull_turn_by_turn_coro`          | `Read()` without a buffer, with `kPull`'s read-ahead |
| `list_features_push_turn_by_turn_coro`          | `Read()` without a buffer, reading only when asked   |
| `list_features_push_continuous_coro`            | The unbounded buffer                                 |
| `list_features_pull_continuous_coro`            | The latest-only buffer                               |
| `list_features_push_per_message_coro`           | The bounded buffer                                   |
| `record_route_push_coro`                        | Awaited writes                                       |
| `record_route_pull_producer_coro`               | A range source                                       |
| `route_chat_push_continuous_coro`               | The bidirectional join                               |

### Coroutine use cases: one per eligible use case

A use case gets a coroutine twin unless its point is work done inside the reaction, on the gRPC thread, which the
boundary rule keeps at the callback level:

| Use case                | Coroutine twin                | Cloned from coroutine variant          |
|-------------------------|-------------------------------|----------------------------------------|
| `whats_here`            | `whats_here_coro`             | `get_feature_pull_coro`                |
| `turn_by_turn_guidance` | `turn_by_turn_guidance_coro`  | `list_features_push_turn_by_turn_coro` |
| `trip_recorder`         | `trip_recorder_coro`          | `list_features_push_continuous_coro`   |
| `live_vehicle_position` | `live_vehicle_position_coro`  | `list_features_pull_continuous_coro`   |
| `offline_map_download`  | `offline_map_download_coro`   | `list_features_push_per_message_coro`  |
| `live_route_recording`  | `live_route_recording_coro`   | `record_route_push_coro`               |
| `gpx_track_upload`      | `gpx_track_upload_coro`       | `record_route_pull_producer_coro`      |
| `convoy_chat`           | `convoy_chat_coro`            | `route_chat_push_continuous_coro`      |
| `unnamed_places_audit`  | None                          | Counts inside `read_ok`                |
| `place_search`          | None                          | Declines inside `read_ok`              |
| `guided_tour_briefing`  | None                          | Declines repeated replies in `read_ok` |

## How the milestone proves it

Every non-trivial claim needs a test or a demonstrable run:

- **`Task<T>` resumes, transfers and destroys correctly.** Unit tests of the task type, under ASan and TSan.
- **Every resumption runs on the application thread.** An assert in the executor's resume handler, and in every
  coroutine scenario.
- **Each buffer behaves as its callback parent does.** Tests on the `AppLoop` task queue, against the in-process server
  of `continuous_use_support.h`, one per buffer.
- **Teardown never frees a reactor under gRPC.** A test that halts the loop with RPCs in flight, under ASan.
- **Each twin does what its parent does.** Each pair of applications runs against a fresh `route_guide_callback_server`,
  with the same totals: every fix, report, place, point and note counted as by the parent.

## Suggested order

1. `Task<T>`, `Spawn()`, the executor interface and the EventLoop executor, with their unit tests. Done, in the
   `RpcReactor::coro` namespace: `Spawn()` is a member of `RootTasks`, the owner of the root tasks, and
   `EventLoopExecutor`, in `reactor_eventloop.h`, takes the application thread as a parameter, the calling thread by
   default, so its tests can run EventLoop on a thread of their own.
   [testing.md](/docs/testing.md#coroutine-core-coverage) lists what the tests prove.
2. The unary awaitable: `get_feature_pull_coro`, then `whats_here_coro`, to settle `Finish()` and ownership. Done:
   `coro::Unary` in `reactor_coro.h`, with both applications. Its context is an optional last constructor argument,
   for a deadline, and its `Finish()` returns a `UnaryResult` of the status and the owned response.
   [testing.md](/docs/testing.md#unary-awaitable-coverage) lists what the tests prove.
3. The reader without a buffer: both `turn_by_turn_coro` variants, then `turn_by_turn_guidance_coro`. Done:
   `coro::Reader` in `reactor_coro.h`. Its `Read()` gives `std::optional<ResponseT>` under `kPull` and
   `std::unique_ptr<ResponseT>` under `kPush`, both empty at the end of the stream, and its `Finish()` gives the
   `grpc::Status`, once `Read()` has given nothing: while the reactor holds a message, the RPC cannot end. To stop
   early, a coroutine calls `TryCancel()`, then reads until `Read()` gives nothing.
   `ResumeHandover` became re-armable rather than one per read: whatever ends a read on a gRPC thread must find the
   hand-over already in place, so the reader would keep one per read as a member anyway, and one re-armed with
   `Rearm()`, which asserts that the last read has ended, says the same with less. It is re-armed before the call that
   arms the next read, `GetResponse()` or `ResumeRead()`, since that read can end on another thread before the call
   returns. `OnDone()` has a hand-over of its own, as it can come before the coroutine has resumed from the last read.
   `turn_by_turn_guidance_coro` waits for its driver, which the callback twin does with an EventLoop timer. Since a
   scenario never calls EventLoop, `EventLoopExecutor` gained `After(delay)`, a static awaitable over that timer,
   which `live_route_recording_coro` will reuse. The `Executor` interface is unchanged: a scenario that waits for
   time names the EventLoop adapter, and only that.
   [testing.md](/docs/testing.md#reader-coverage) lists what the tests prove.
4. The buffered readers: their three variants, then `trip_recorder_coro`, `live_vehicle_position_coro` and
   `offline_map_download_coro`.
5. The writers: their two variants, then `live_route_recording_coro` and `gpx_track_upload_coro`.
6. The bidirectional join: `route_chat_push_continuous_coro`, then `convoy_chat_coro`.
7. Teardown and cancellation tests.

## The language standard: C++20

The level stays on C++20, the project's standard. C++23 would add little to it: `std::generator` only serves
`kReactorPulls` sources, which a range already covers, and C++23 still has no task type. It would also cost the
project its GCC 13 presets, `vcpkg-gcc13-debug` and `vcpkg-gcc13-release`, since GCC 13's standard library has no
`std::generator`, and a check of every CI compiler's C++23 library support. A move to C++23 is a separate change,
proven by CI on its own. If it happens, `std::move_only_function` is the feature worth adopting first: the reactor
callbacks are `std::function`, which cannot hold a callback that owns a move-only value.
