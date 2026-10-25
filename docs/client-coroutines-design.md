# Client coroutines: design draft

> [!NOTE]
> This is a proposal. Nothing it describes exists yet. Once the layer is built and its claims are proven by tests and
> runs, its reasoning moves into [architecture.md](/docs/architecture.md), and this file goes away.

## Goal

The client reactors run every callback on a gRPC thread, and the Active Object design moves each event to the
application thread. That keeps all application state on one thread, without locks. The price is the shape of the
code: one use case is split across `OnReadOk`, `OnWriteDone` and `OnDone` handlers, and the state they share lives in
members, because a handler cannot see what the previous one was doing.

A C++20 coroutine keeps the thread guarantee and removes the split. The use case becomes one function that suspends at
each `co_await` and resumes on the application thread:

```cpp
// Proposed API. Runs on the application thread from start to end.
Task<void> TripRecorder::Run(routeguide::Rectangle rect) {
  auto reader = co::ListFeatures<co::Buffer::kUnbounded>(stub_, rect);
  while (auto report = co_await reader.Read()) {
    Store(*report);  // no lock: the same thread as all application state
  }
  const grpc::Status status = co_await reader.Finish();
}
```

The milestone builds this layer for the client first. The client already has the reactors to build on, tests with an
in-process server, and eleven use cases to port and compare against. The server milestone then reuses the same
coroutine core.

## What the layer keeps, and what it adds

| Kept from the reactor design                         | Added by the coroutine layer                      |
|------------------------------------------------------|---------------------------------------------------|
| Every callback runs on a gRPC thread                 | Each callback resumes a suspended coroutine       |
| Application code runs on the application thread only | A use case is one function, not a set of handlers |
| Events reach the application in FIFO order           | Per-RPC state lives in the coroutine frame        |
| Every hold, and every release obligation             | Pacing becomes a choice of buffer                 |

The layer sits on top of the existing reactors, in its own header, `applications/reactor/reactor_coro.h`. It does not
change `reactor_client.h`. Each awaitable owns one reactor and supplies its callbacks, so the reactor keeps enforcing
its own rules: one write in flight, one release per held message, and the holds that keep the RPC alive. If the layer
needs something the callbacks cannot give, the design is wrong, not the generic layer.

## Decisions

### Where a coroutine resumes: on the application thread

A reactor callback does not resume the coroutine itself. It posts the coroutine handle to the event loop:

```cpp
EventLoop::TriggerEvent(kResume, handle.address());  // from the gRPC thread
```

One handler for `kResume`, registered once for the whole layer, turns the address back into a handle and resumes it,
on the application thread. The layer owns that registration through an `RpcReactor::EventConnection`, created in
`main()` before any coroutine starts.

Resuming inline, on the gRPC thread, would be faster by one queue hop. It would also run application code on a gRPC
thread, which is exactly what the Active Object design exists to prevent. So the layer never resumes inline. A use
case that wants gRPC-thread work, such as the aggregate of `unnamed_places_audit`, keeps using a `kContinuous`
callback for it.

The event queue stays FIFO, so a coroutine's resumptions arrive in the order the reactor produced them, after every
event queued before them.

### The task type: written here, not taken from a library

C++20 has coroutines in the language, but no library types: no `task`, and `std::generator` only arrives in C++23. The
project builds with C++20. The layer therefore defines a small `Task<T>`:

| Property               | Choice                                                                          |
|------------------------|---------------------------------------------------------------------------------|
| Start                  | Lazy: the body starts when the task is awaited or spawned                       |
| Awaiters               | One at most                                                                     |
| Resumption of awaiter  | Symmetric transfer: `await_suspend` returns the handle to resume                |
| Ownership              | The caller owns the frame; destroying the `Task` destroys a finished frame      |
| Root tasks             | `Spawn()` gives a root task to an owner that keeps it until it completes        |
| Exceptions             | To decide, see [Open questions](#open-questions)                                |

Symmetric transfer matters because an awaitable can complete without suspending, for instance a `Read()` on a buffer
that already holds a message. A loop of such reads would otherwise grow the stack with each iteration.

asio-grpc and stdexec solve the same problem with far more generality. Neither is a dependency here, for the reason
the reactors were written in this repository: the mechanics stay visible and testable, and the vcpkg build stays as it
is. Both remain good references.

### What an awaited message is: always owned

A message lent by `kPull`'s `read_ok` lives in the reactor's read target, which the next read overwrites. A reference
to it that survives a `co_await` dangles as soon as the next read completes, and nothing in the code shows it. So every
awaitable hands the coroutine an owned message. `Delivery` stays, because it decides how the message becomes owned, and
what that costs:

| Delivery | How the coroutine gets an owned message                 | Cost per message       | Read-ahead              |
|----------|---------------------------------------------------------|------------------------|-------------------------|
| `kPull`  | `Read()` swaps it out of the reactor with               | No allocation          | `GetResponse()` arms    |
|          | `GetResponse()`                                         |                        | the next read at once   |
| `kPush`  | `Read()` takes the `std::unique_ptr` the callback       | One allocation         | None until the next     |
|          | handed out                                              |                        | `Read()`                |

The read-ahead column is the difference `turn_by_turn_guidance` already turns on. Under `kPull`, `GetResponse()` takes
the message and arms the next read in one call, so the next message can arrive while the coroutine works on this one.
Under `kPush`, the layer calls `ResumeRead()` only when the coroutine asks for the next message, so nothing is read
until then. Only a buffered reader, below, holds several messages at once, so only `kPush` can feed one.

### Pacing as a buffer between the reactor and the coroutine

A coroutine that awaits each read takes one message at a time, which is `kTurnByTurn` by construction. The other
pacings put a buffer between the reactor and the coroutine, and each existing use case maps onto one:

| Reader                       | Reactor underneath            | Buffer policy              | Use case it replaces     |
|------------------------------|-------------------------------|----------------------------|--------------------------|
| `Read()`, no buffer          | `kTurnByTurn`, either         | None: one message in       | `turn_by_turn_guidance`  |
|                              | delivery                      | flight                     |                          |
| `Buffer::kUnbounded`         | `kPush`, `kContinuous`        | Every message queued, the  | `trip_recorder`          |
|                              |                               | stream never waits         |                          |
| `Buffer::kLatest`            | `kPull`, `kContinuous`, with  | One slot, overwritten, the | `live_vehicle_position`  |
|                              | a `LatestSlot`                | stream never waits         |                          |
| `Buffer::kBounded(n)`        | `kPush`, `kPerMessage`        | Holds the stream once `n`  | `offline_map_download`   |
|                              |                               | messages wait              |                          |
| `Read(filter)`               | `kPull`, `kPerMessage`        | None: `filter` runs on the | `place_search`           |
|                              |                               | gRPC thread, declines cost |                          |
|                              |                               | nothing                    |                          |

`kLatest` copies each lent message into the slot inside the callback, as `live_vehicle_position` does today, so it is
the one buffered reader that runs on `kPull`. The filter runs where `read_ok` runs, on the gRPC thread, so it must touch
nothing but the message. That is the one place where user code still runs off the application thread, and the layer
says so in its signature.

### Writers: awaited writes, or a synchronous source

| Submission           | Coroutine side                                              | Fits                             |
|----------------------|-------------------------------------------------------------|----------------------------------|
| `kApplicationPushes` | `co_await writer.Write(point)`, then                        | Points that appear over time,    |
|                      | `co_await writer.Close()`; each write resumes on            | such as `live_route_recording`   |
|                      | `write_done`                                                |                                  |
| `kReactorPulls`      | The writer is given a range or a generator, and the reactor | A source ready at once, such as  |
|                      | pulls from it                                               | `gpx_track_upload`               |

The shape of the code enforces one write in flight: a coroutine cannot start a second `Write()` before the first one
resumes it. A coroutine that must wait for its next request, for a GPS fix or a typed note, needs
`kApplicationPushes`. `next_request` cannot answer "not yet", as
[Why the writing reactors also let the reactor pull](/docs/architecture.md#why-the-writing-reactors-also-let-the-reactor-pull)
explains, so a `kReactorPulls` source must be synchronous: a range, or a `std::generator` once the project moves to
C++23.

### The end of an RPC: `Finish()` is awaited exactly once

Every awaitable reactor offers `co_await Finish()`, which resumes on `OnDone` and returns the status, and the final
response for a unary or writing RPC. A coroutine awaits it exactly once, after its last read or write. The awaitable
destroys its reactor after `OnDone`, in the frame, on the application thread, as the `OnDone` handlers do today.

A bidirectional RPC reads and writes at the same time, so it needs two flows: one coroutine reads, one writes, and a
join awaits both before `Finish()`. The writer ends its stream with `Close()`. The reader ends when `Read()` returns no
message. Only the joined task awaits `Finish()`.

### Teardown and cancellation

A suspended coroutine resumes only through the event loop. `EventLoop::Halt()` discards the events still queued, so a
coroutine whose resumption was queued, or is still to come, never runs again. Its frame, and the reactor in it, must
still be released, and a reactor must never be destroyed before its `OnDone()` has run. The layer follows the rule
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

## How the milestone proves it

The user requires a test or a demonstrable run for every non-trivial claim. Each claim above gets one:

| Claim                                               | Proof                                                           |
|-----------------------------------------------------|-----------------------------------------------------------------|
| `Task<T>` resumes, transfers and destroys correctly | Unit tests of the task type, under ASan and TSan                |
| Every resumption runs on the application thread     | An assert in the resume handler, and in every ported use case   |
| Each buffer policy behaves as its use case does     | Tests against the in-process server of                          |
|                                                     | `continuous_use_support.h`, one per policy                      |
| Teardown never frees a reactor under gRPC           | A test that halts the loop with RPCs in flight, under ASan      |
| The ported use cases do what the originals do       | Both applications run against `route_guide_callback_server`,    |
|                                                     | with the same totals: 64 places shown and 36 left out, every    |
|                                                     | point counted, every note shown                                 |

Each ported use case lives next to its callback version, so `diff` shows what the coroutine changes, as the variants'
diffs show what one parameter changes.

## Suggested order

1. `Task<T>`, `Spawn()` and the resume handler, with their unit tests.
2. The unary awaitable, ported on `whats_here`: the smallest RPC, to settle `Finish()` and ownership.
3. The reader without a buffer, ported on `turn_by_turn_guidance`, which shows the two deliveries' read-ahead.
4. The buffered readers, ported on `trip_recorder`, `live_vehicle_position` and `offline_map_download`.
5. The writers, ported on `live_route_recording` and `gpx_track_upload`.
6. The bidirectional join, ported on `convoy_chat`.
7. Teardown and cancellation tests.

## Open questions

- **Exceptions.** Does `unhandled_exception()` call `std::terminate()`, or store the exception for the awaiter? The
  answer depends on whether the project's code may throw at all, which nothing states today.
- **Where the ported use cases live.** Beside the callback ones in `usecases/`, with a `_coro` suffix, or in a
  directory and an application of their own.
- **Whether the filter reader belongs in the layer.** It is the one place user code runs on a gRPC thread. Keeping
  `place_search` on the callback reactor may be clearer.
- **C++23.** `std::generator` would give `kReactorPulls` writers a standard source type. Moving the project's standard
  is a separate decision.
