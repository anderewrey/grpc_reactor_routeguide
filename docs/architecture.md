# Architecture documentation

This repository is an educational sandbox for mastering gRPC's asynchronous callback API and, more broadly, for
working out how to structure an application around it. This document is the design rationale: which patterns this
client-side implementation is built on, why they were chosen over the alternatives, and how the pieces fit together.
For the implementation mechanics (class-by-class API, race conditions, sequence diagrams), see
[reactor_client.md](/applications/reactor/reactor_client.md). For building and testing, see
[developing.md](/docs/developing.md) and [testing.md](/docs/testing.md).

## Active Object pattern

This implementation follows the Active Object pattern, combining it with the Reactor pattern for event-driven
processing. The reactor library provides the pattern infrastructure; applications provide business logic in the
Servant handlers.

Each pattern contributes a different half. The Active Object pattern supplies the components (Proxy, Method
Request, Activation Queue, Scheduler, Servant, Future, Guards) and the rule that responses are processed on one
application thread. The Reactor pattern supplies the event handling: gRPC events such as `OnDone` and
`OnReadDone` are demultiplexed to their handlers, handled without blocking, and event detection stays separate
from event handling.

**Library vs Application Responsibilities:**

| Aspect | Definition | This Implementation |
| -------- | ------------------ | --------------------- |
| Guards | Method Requests have `guard()` for synchronization | The reaction boundary, plus holds where it is crossed |
| Servant | Business logic component with state | Application-provided (demo uses logging placeholders) |
| Scope | Full client-server encapsulation | Client-side library (server has separate Servant) |

Every reactor pushes its response to a callback rather than keeping it for the application to pull.
`ActiveUnaryReactor` and `ActiveWriteReactor` move their single terminal response into the `done` callback.
`ActiveReadReactor` and `ActiveBidiReactor` move each received message into the `read_ok` callback.

What differs between the two reading reactors' modes is who arms the read that follows. Under the default
`ReadPacing::kContinuous` the reactor re-arms as the last action of the same reaction, so the next read cannot
complete while that reaction runs, and no hold is needed. Under `ReadPacing::kTurnByTurn` the application arms it
through `ResumeRead()`, from outside any reaction, and a hold is what keeps the RPC alive across that gap.

The implementation follows this component structure:

```text
┌────────────────────────────────────────────────────────────────────┐
│                        Application Thread                          │
│  ┌─────────┐    ┌──────────────────┐    ┌───────────────────────┐  │
│  │  Proxy  │───▶│ Activation Queue │───▶│  Response Handlers    │  │
│  │(Client) │    │  (EventLoop)     │    │  (Adapted Servant)    │  │
│  └─────────┘    └──────────────────┘    └───────────────────────┘  │
│       │                  ▲                          │              │
│       │                  │                          │              │
│       ▼                  │                          ▼              │
│  ┌─────────────────────────────────────────────────────────────┐   │
│  │                    Method Request (Reactor)                 │   │
│  │              Encapsulates RPC state and context             │   │
│  └─────────────────────────────────────────────────────────────┘   │
└────────────────────────────────────────────────────────────────────┘
                                │
                                ▼
┌────────────────────────────────────────────────────────────────────┐
│                        gRPC Thread Pool                            │
│                   Executes callbacks (OnDone, OnReadDone)          │
└────────────────────────────────────────────────────────────────────┘
```

A request crosses this diagram in eight steps: the application creates a `Point` or `Rectangle` message (Proxy);
Protobuf serializes it and gRPC sends it over HTTP/2; the server processes it and sends back a `Feature` or a
stream of responses; a gRPC thread invokes `OnDone`/`OnReadDone`, which triggers an EventLoop event (Activation
Queue); the corresponding Response Handler then runs on the application thread.

| Component | Implementation | Role |
| ----------- | ---------------- | ------ |
| **Proxy** | The four `RouteGuideClient` RPC methods | Client-facing API, creates reactors |
| **Method Request** | The four `Active*Reactor` classes | Encapsulates RPC state and context |
| **Scheduler** | `EventLoop::Run()` | Dispatches events to handlers |
| **Activation Queue** | EventLoop internal queue | Holds pending events, and the messages reactors push |
| **Servant** | `EventLoop::RegisterEvent()` handlers | Processes RPC responses (application logic) |
| **Future** | `Status()` | Deferred result access |
| **Guards** | The reaction boundary, plus holds where crossed | Keeps `OnDone()` from concluding an RPC mid-call |

> **Note:** The "Proxy" terminology describes the component's role in the Active Object pattern, not
> an application of the GoF Proxy design pattern. The demo application uses simple logging as Servant
> logic; production applications implement actual business logic in the event handlers.

### Why two threads?

| Thread | Operations |
| ------------------- | ------------------------------------------------------- |
| Application (main) | Create reactors, process responses, application logic |
| gRPC Thread Pool | Execute callbacks (OnDone, OnReadDone, OnWriteDone) |

See "Why Active Object pattern?" below for why this split exists and what problem it solves. Which operations need
a hold is tabulated in
[reactor_client.md](/applications/reactor/reactor_client.md#hold-requirements), and the reasoning behind each row,
including where the write side remains unprotected, is under
[Why one rule decides every hold](#why-one-rule-decides-every-hold).

## Technology stack

gRPC and Protobuf aren't listed here as "choices." Using them is the point of this project, not a decision made
among alternatives. What follows are the choices made *around* them.

| Layer | Technology | Purpose |
| ------- | ------------ | --------- |
| Language | C++20 | Core language |
| Event System | EventLoop | Cross-thread event dispatching |
| Logging | spdlog | Structured logging |
| Build | CMake + Ninja | Build orchestration |
| Dependencies | vcpkg | Multi-compiler package management |

## Design decisions

### Why Active Object pattern?

The gRPC reactor callbacks (`OnDone`, `OnReadDone`) execute on threads from gRPC's internal thread pool, and the
application cannot control which thread executes a given callback. A direct callback implementation processes
the response immediately inside that callback, which requires synchronization for every piece of application
state the callback can reach. That synchronization requirement propagates through the whole application: all
mutable state must be protected at every access point, not just where the callback fires, and the application
loses its single-threaded execution guarantees. Long processing inside a callback also blocks a thread gRPC needs
for other RPCs.

`RouteChat`'s server-side callback implementation shows the cost concretely.
[route_guide_callback_server.cpp](/applications/callback/route_guide_callback_server.cpp) cannot hold a single
lock around the shared notes vector for the whole exchange, unlike the non-reactor equivalent in
[route_guide_sync_server.cpp](/applications/blocking/route_guide_sync_server.cpp), because the reactor's read and
write callbacks can run on different gRPC threads between a read and a write. It locks twice instead: once to copy
the notes to send, and once to append the newly received note.

The Active Object pattern defers response processing to the application thread instead of processing it inside
the callback. The Proxy (`GetFeature()`, `ListFeatures()`) returns immediately without touching application
state. The callback hands the response to the EventLoop, which dispatches it to the Servant
(`EventLoop::RegisterEvent()` handlers) on the single application thread, in sequence with everything else the
application does. All four reactors put ownership of the message itself into that dispatch.

1. **Thread Safety**: Responses processed on application thread, not gRPC thread
2. **Non-blocking**: Client calls return immediately
3. **Separation of Concerns**: Clean boundary between RPC and application logic
4. **Testability**: EventLoop can be mocked for testing

The reactor library provides the Active Object infrastructure; applications provide their own Servant
logic. This separation enables reusable async RPC handling across different applications, instead of every
application re-deriving its own synchronization around gRPC's callbacks.

The split falls here:

| Provided by | What |
| ------------- | ------ |
| The library | Method Request encapsulation, the four `Active*Reactor` classes |
| The library | Scheduler integration: callbacks trigger `EventLoop::TriggerEvent()` |
| The library | Message ownership transfer, so no response outlives a callback inside a reactor |
| The library | Future-like access through `Status()` |
| The application | Servant business logic, in `EventLoop::RegisterEvent()` handlers |
| The application | Domain processing that turns RPC responses into application state changes |

The demo application uses logging as placeholder Servant logic, where a production application would put real
work:

```cpp
// Demo: logging only
EventLoop::RegisterEvent(kGetFeatureOnDone, [](const Event* event) {
  logger.info("RESPONSE | {}", response);  // Placeholder Servant logic
});

// Production: real Servant logic
EventLoop::RegisterEvent(kGetFeatureOnDone, [&app_state](const Event* event) {
  const std::unique_ptr<routeguide::Feature> response{static_cast<routeguide::Feature*>(event->getData())};
  app_state.UpdateFeatureCache(*response);      // Business logic
  ui_controller.NotifyFeatureLoaded(*response); // State changes
  metrics.RecordFeatureQuery(*response);        // Application concerns
});
```

### Why vcpkg with custom triplets?

1. **Multi-compiler Support**: Same project builds across multiple GCC and Clang versions
2. **ABI Compatibility**: Dependencies built with matching compiler
3. **Reproducibility**: Consistent builds across environments
4. **Debug + Release Mix**: Debug app can link Release libraries on Linux

### Why EventLoop library?

1. **Simple API**: `TriggerEvent()` / `RegisterEvent()` / `Run()`
2. **Thread-safe**: Safe cross-thread event dispatching
3. **Replaceable**: Interface allows swapping with other event systems

### Why the reading reactors offer two pacing modes

Every reactor hands its response to a callback and keeps nothing for the application to pull. For the single
terminal response of a unary or client-streaming RPC that is the whole story, because the RPC is already over
when that response arrives. For a stream of messages it leaves one question open: who arms the next read, and
therefore where the backpressure goes.

`ReadPacing::kContinuous`, the default, has the reactor re-arm inside the reaction. The stream never waits for the
application, at the cost of two things:

| Cost                    | Cause                                   | Accepted because              |
|-------------------------|-----------------------------------------|-------------------------------|
| The queue is unbounded  | EventLoop has no backpressured delivery | The request bounds the volume |
| `Halt()` leaks messages | `Halt()` discards rather than drains    | The demo's loop never halts   |

Neither is acceptable for a stream whose volume the peer controls, which is what `ReadPacing::kTurnByTurn` is for.
There the application arms the next read through `ResumeRead()`, so exactly one message is ever in flight and
the stream stalls for as long as the consumer is slow. The cost moves rather than disappearing: an application
that drops a message without resuming strands the RPC, since the hold suppresses `OnDone()` and no outstanding
read is left to report the stream ending.

Both modes deliver the message the same way, so the choice does not change the callback signature or the
surrounding application wiring. A third option, bounded buffering inside the reactor, was evaluated and rejected
for an unresolved lost-wakeup race, recorded in
[reactor_client_response_ownership_sketch.md](/applications/reactor/reactor_client_response_ownership_sketch.md).

### Why one rule decides every hold

A hold keeps `OnDone()` from concluding an RPC, and destroying its underlying gRPC-bound state, while an operation
on that state is still being issued. One rule decides where the library needs one:

> An operation issued from inside a reaction needs no hold. One issued from outside a reaction needs one.

A reaction in progress is itself proof that the gRPC-bound state still exists, which is exactly the guarantee a
hold buys. Which operation falls on which side is tabulated in
[reactor_client.md](/applications/reactor/reactor_client.md#hold-requirements); the subsections below are
the reasoning behind each row.

The rule says a hold is needed, not when it must be taken. Those are separate questions, and conflating them is
what made the write side look unsolvable for as long as it did. A hold protecting an application-thread call has to
be reserved earlier, either before `StartCall()` or from inside some reaction, because taking one needs the state
whose liveness is in question. What decides whether a design works is therefore not the operation it protects but
the trigger that releases it.

### Why kContinuous needs no hold

The first `StartRead()` is issued by the specialized constructor, before `StartCall()`, so the RPC has not started
yet and no `OnDone()` can be in flight. Every later one is issued as the last statement of `OnReadDone(true)`,
from inside the reaction, so the read path obtains the guarantee structurally rather than by asking for it.

Re-arming after the callback also bounds the reaction to one at a time, since a read cannot complete before it is
started. Messages therefore reach the event loop in network order.

### Why kTurnByTurn needs one

`ResumeRead()` runs on the application thread, after `OnReadDone(true)` has already returned. That is precisely
the window a hold exists for: with no read armed and no reaction in progress, gRPC's outstanding-callback count
can otherwise reach zero, fire `OnDone()`, and invalidate the state `StartRead()` is about to go through.

The ordering within the pair is what makes it correct:

- `AddHold()` runs before the `read_ok` callback, because that callback may resume inline, or hand the message to
  a thread that resumes before the reaction returns.
- `StartRead()` runs before `RemoveHold()`, because `StartRead()` raises the count first, so the count never
  reaches zero between the two calls.
- `RemoveHold()` runs whether or not a read was armed, because gRPC requires exactly one release per hold, and a
  hold left outstanding stalls the RPC forever.

### Why the bidi write side holds while idle

`SendRequest()`, `SendLastRequest()`, and `CloseRequestStream()` run on the application thread and reach gRPC from
outside any reaction, which is precisely the case gRPC's [hold mechanism][grpc-hold-pr] exists for.
`ActiveBidiReactor` keeps one hold outstanding for exactly the window where the write stream is idle, and makes
claiming that hold the permission to write. Every write therefore happens with a hold provably outstanding, and
the flag test that used to guard it disappears rather than being reinforced.

Making the hold the permission, rather than a separate protection around it, is what removes the race instead of
narrowing it. A flag test can go stale between the read and the act; a compare-exchange cannot, because the winner
is the only thread that proceeds. The same claim also enforces gRPC's one-write-in-flight rule, so the previous
`write_pending_` flag became redundant and was deleted.

The `stream_no_more_` test survives, moved to after the claim, because the two answer different questions. The
claim proves the state is alive, which is what makes the call safe. The flag reports that the stream is dead, which
is what stops a doomed operation from being issued. Dropping the flag test was tried and reverted: a write accepted
into a terminating RPC is safe but may never complete, and an operation gRPC never completes holds the
outstanding-callback count above zero. Safety and futility are separate properties, and the design needs a guard
for each.

The hold is never taken from the application thread. It is reserved before `StartCall()`, where nothing can be in
flight yet, and re-taken in `OnWriteDone()`, from inside a reaction. Both are positions the rule already permits.

`OnReadDone(false)` is what releases a hold the application never claims, and it is the piece that makes the design
work where the whole-RPC version failed. gRPC's own contract states that a failure on either direction means
neither will succeed any more, so the end of the read stream is a reliable end-of-RPC signal regardless of which
direction failed. Under `ReadPacing::kContinuous` a read is always armed, from the constructor
onward, so the signal always arrives. Under `kTurnByTurn` it does not: between a delivered message and the matching
`ResumeRead()` nothing is armed, so the idle write hold waits on the same `ResumeRead()` the application already
owes for the read hold. That obligation now covers both holds rather than one.

Two reactions can race to change the hold's state, since gRPC may run `OnWriteDone()` and `OnReadDone(false)`
concurrently. Both sides therefore publish their own flag before testing the other's, which guarantees at least one
of them observes the other and exactly one of them releases. That symmetry is the reason neither atomic may be
weakened to a relaxed ordering.

### Why the write side of ActiveWriteReactor has no hold

`ActiveWriteReactor` cannot use the design above, because the release trigger it depends on does not exist there.
A client-streaming RPC has no read stream, so an RPC ending without an application write action, a deadline
expiring or the server cancelling, produces no reaction that could free an idle hold. It keeps the
`stream_no_more_` flag test, and with it the residual race.

The `UseMultipleHolds()` sketch it still carries proposes one hold covering the whole write flow, acquired before
`StartCall()` and released when that flow conclusively ends. That was prototyped and rejected, and the reason is
worth recording because it is not obvious from the gRPC documentation. Such a hold suppresses `OnDone()` for the
entire life of the RPC, so only the application can conclude it, and passive terminations give the application
nothing to release on. `RouteChat_DeadlineExceeded_PropagatesStatus` and `RouteChat_ServerClosesFirst_ClientContinues`
both hung for their full timeouts, while every test that closed its request stream passed.

The idle-window hold differs from that one in scope and in trigger, not in mechanism. It stands only while no write
is in flight, and it is released by a reaction rather than by an application action. What the bidi case supplies,
and the write-only case cannot, is a reaction guaranteed to arrive when the RPC ends.

Releasing a whole-flow hold from a gRPC-thread reaction does not rescue the write-only case either, since it
reopens the race it was meant to close: an application thread can enter `SendRequest()`, pass every guard, and
still be short of its `StartWrite()` when that reaction removes the hold. The claim-based design has no such gap,
because the thread that will write is the one holding the claim.

The remaining candidate for `ActiveWriteReactor` is to stop issuing writes from the application thread at all, by
draining an application-side queue from inside `OnWriteDone()`. Its unsolved edge is the first write of an idle
stream, where no reaction exists yet to issue from. Reads do not have that edge, because a read can be armed
speculatively before any data exists and the constructor does exactly that.

## Component structure

### Libraries

| Library | Type | Purpose |
| --------- | ------ | --------- |
| `protobuf_utils` | Static | Protobuf message utilities |
| `rg_proto` | Object | Generated protobuf/gRPC code |
| `rg_service` | Static | RouteGuide business logic |

### Dependency graph

```text
route_guide_active_reactor_client
    ├── rg_service
    │   ├── rg_proto
    │   │   ├── protobuf::libprotobuf
    │   │   └── gRPC::grpc++
    │   ├── protobuf_utils
    │   │   └── protobuf::libprotobuf
    │   ├── gflags::gflags
    │   └── spdlog::spdlog
    └── EventLoop::EventLoop
```

## Extending the reactor library

This section defines patterns for consistent implementation across contributors.

### Style guide reference

All code follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with these
project-specific settings:

- Line limit: 120 characters (configured in `.clang-format`)
- C++ standard: C++20
- Pre-commit hooks enforce cpplint and clang-format

### Reactor class hierarchy

**Generic Base Classes** (`applications/reactor/reactor_client.h`):

| Class | RPC Pattern | Template Parameters |
| ------- | ------------- | --------------------- |
| `ActiveUnaryReactor<ResponseT>` | Unary | Response type |
| `ActiveReadReactor<ResponseT>` | Server-streaming | Response type |
| `ActiveWriteReactor<RequestT, ResponseT>` | Client-streaming | Request and response types |
| `ActiveBidiReactor<RequestT, ResponseT>` | Bidirectional | Request and response types |

**Service-Specific Adapters** (`applications/reactor/reactor_client_routeguide.h`):

| Generic Class | RouteGuide Adapter | RPC Method |
| --------------- | ------------------- | ------------ |
| `ActiveUnaryReactor<Feature>` | `routeguide::GetFeature::ClientReactor` | `GetFeature` |
| `ActiveReadReactor<Feature>` | `routeguide::ListFeatures::ClientReactor` | `ListFeatures` |
| `ActiveWriteReactor<Point, RouteSummary>` | `routeguide::RecordRoute::ClientReactor` | `RecordRoute` |
| `ActiveBidiReactor<RouteNote, RouteNote>` | `routeguide::RouteChat::ClientReactor` | `RouteChat` |

### Callback struct patterns

Each reactor type has a corresponding callback struct:

**Unary** (`ActiveUnaryCallbacks<ResponseT>`):

- `done`: called on RPC completion, receives the status and ownership of the response as a
  `std::unique_ptr<ResponseT>`

**Server-streaming** (`ActiveReadCallbacks<ResponseT>`):

- `read_ok`: called on successful read, receives ownership of the message as a `std::unique_ptr<ResponseT>`
- `read_nok`: called when stream ends (no more reads)
- `done`: called on RPC completion

**Client-streaming** (`ActiveWriteCallbacks<RequestT, ResponseT>`):

- `write_done`: called after each write completes
- `done`: called on RPC completion, receives the status and ownership of the terminal response as a
  `std::unique_ptr<ResponseT>`

**Bidirectional** (`ActiveBidiCallbacks<RequestT, ResponseT>`):

- `read_ok`: called on successful read
- `read_nok`: called when read stream ends
- `write_done`: called after each write completes
- `done`: called on RPC completion

### File organization

**New reactor implementations:**

| File Type | Location | Naming |
| ----------- | ---------- | -------- |
| Generic base class | `applications/reactor/reactor_client.h` | Add to existing file |
| Service adapter | `applications/reactor/reactor_client_routeguide.h` | Add to existing file |
| Unit tests | `applications/reactor/tests/` | `{reactor_name}_test.cpp` |

**Test file naming:**

- `active_write_reactor_test.cpp`: client-streaming reactor tests
- `active_bidi_reactor_test.cpp`: bidirectional reactor tests

### Error handling pattern

Use `grpc::Status` passthrough instead of custom exceptions:

```cpp
// Callback receives the status, and ownership of the response where the RPC has a terminal one
cbs.done = [](auto* reactor, const grpc::Status& status, std::unique_ptr<ResponseT> response) {
  if (status.ok()) {
    // Process *response
  } else {
    // Handle error via status.error_code(), status.error_message()
    logger.error("RPC failed: {} - {}",
                 status.error_code(), status.error_message());
  }
};

// The response pointer is never null, so no consumer needs a null check: a failed RPC yields an
// empty message, and the status is what tells the consumer whether the content is meaningful.
```

**Rationale:** Status-based error handling aligns with Google C++ Style Guide (which discourages exceptions),
matches gRPC's native pattern, and works cleanly with the async callback model where exceptions cannot cross
thread boundaries.

### Thread safety patterns

**Atomic flags for cross-thread state:**

```cpp
std::atomic_bool read_held_{false};        // Set by gRPC thread, cleared by whoever resumes
std::atomic_bool write_idle_{false};       // Bidi only: an unclaimed idle-window hold
std::atomic_bool stream_no_more_{false};   // Set by gRPC thread, read by app thread
```

`read_held_` exists in the two reading reactors, where it records that a `ReadPacing::kTurnByTurn` hold is
outstanding and makes a duplicate or spurious `ResumeRead()` a rejected no-op. `write_idle_` exists in
`ActiveBidiReactor` only, where winning it by compare-exchange is the permission to write, which is why that class
needs no write-pending flag. `stream_no_more_` reports that no further read or write can succeed: on
`ActiveWriteReactor` it is the only guard the write path has, and on `ActiveBidiReactor` it is re-read after the
hold claim is won rather than before, so a stale read cannot let a doomed operation through.
`ActiveUnaryReactor` has none of the three, because it issues nothing from the application thread and owns no
stream.

**Hold mechanism, `ReadPacing::kTurnByTurn` read side:**

```cpp
// In OnReadDone, before invoking the callback, since the callback may resume inline
this->AddHold();
read_held_ = true;

// In ResumeRead, once the application is done with the message
this->StartRead(&response_);   // raises the count first, so it never reaches zero
this->RemoveHold();            // exactly one release per hold, armed or not
```

`ReadPacing::kContinuous` needs no equivalent, and neither do `ActiveUnaryReactor` and `ActiveWriteReactor`. See
[Why one rule decides every hold](#why-one-rule-decides-every-hold) above for why each of them is safe without.

### Feature to component mapping

| Feature | Component | File |
| ------------- | ----------- | ------ |
| Unary RPC | `ActiveUnaryReactor` | `reactor_client.h` |
| Server-streaming RPC | `ActiveReadReactor` | `reactor_client.h` |
| Client-streaming RPC | `ActiveWriteReactor` | `reactor_client.h` |
| Bidirectional RPC | `ActiveBidiReactor` | `reactor_client.h` |
| EventLoop integration | Callback triggers `TriggerEvent()` | Application code |
| Cancellation | `TryCancel()` method | All reactor classes |
| Status check | `Status()` method | All reactor classes |
| Service adapters | `routeguide::*::ClientReactor` | `reactor_client_routeguide.h` |
| Testing | googletest suite | `applications/reactor/tests/` |

<!-- Reference links -->
[grpc-hold-pr]: https://github.com/grpc/grpc/pull/18072
