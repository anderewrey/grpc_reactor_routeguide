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

**Library vs Application Responsibilities:**

| Aspect | Definition | This Implementation |
| -------- | ------------------ | --------------------- |
| Guards | Method Requests have `guard()` for synchronization | `AddHold()`/`RemoveHold()` in `ActiveBidiReactor` |
| Servant | Business logic component with state | Application-provided (demo uses logging placeholders) |
| Scope | Full client-server encapsulation | Client-side library (server has separate Servant) |

`ActiveBidiReactor` is the only reactor that guards a message with a hold. The other three reactors push their
response to a callback instead of keeping it for the application to pull, so they need no hold.
`ActiveUnaryReactor` and `ActiveWriteReactor` move their single terminal response into the `done` callback.
`ActiveReadReactor` moves each received message into the `read_ok` callback and re-arms the next read as the last
action of the same read reaction, so the next read cannot complete while that reaction runs.

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
| **Proxy** | `RouteGuideClient::GetFeature()`, `ListFeatures()` | Client-facing API, creates reactors |
| **Method Request** | `ActiveUnaryReactor`, `ActiveReadReactor` | Encapsulates RPC state and context |
| **Scheduler** | `EventLoop::Run()` | Dispatches events to handlers |
| **Activation Queue** | EventLoop internal queue | Holds pending events |
| **Servant** | `EventLoop::RegisterEvent()` handlers | Processes RPC responses (application logic) |
| **Future** | `Status()`, plus `GetResponse()` in `ActiveBidiReactor` alone | Deferred result access |

> **Note:** The "Proxy" terminology describes the component's role in the Active Object pattern, not
> an application of the GoF Proxy design pattern. The demo application uses simple logging as Servant
> logic; production applications implement actual business logic in the event handlers.

### Why two threads?

| Thread | Operations |
| ------------------- | ------------------------------------------------------- |
| Application (main) | Create reactors, process responses, application logic |
| gRPC Thread Pool | Execute callbacks (OnDone, OnReadDone, OnWriteDone) |

See "Why Active Object pattern?" below for why this split exists and what problem it solves. The exact race
conditions the hold mechanism of `ActiveBidiReactor` protects against, and where that protection is incomplete,
are documented in
[reactor_client.md](/applications/reactor/reactor_client.md#hold-semantics-per-rpc-not-per-direction).

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

The Active Object pattern defers response processing to the application thread instead of processing it inside
the callback. The Proxy (`GetFeature()`, `ListFeatures()`) returns immediately without touching application
state. The callback hands the response to the EventLoop, which dispatches it to the Servant
(`EventLoop::RegisterEvent()` handlers) on the single application thread, in sequence with everything else the
application does. `ActiveUnaryReactor`, `ActiveReadReactor`, and `ActiveWriteReactor` put ownership of the message
itself into that dispatch. `ActiveBidiReactor` keeps the response inside the reactor until the application thread
pulls it with `GetResponse()`, and holds the RPC open with `AddHold()`/`RemoveHold()` until that pull happens.

1. **Thread Safety**: Responses processed on application thread, not gRPC thread
2. **Non-blocking**: Client calls return immediately
3. **Separation of Concerns**: Clean boundary between RPC and application logic
4. **Testability**: EventLoop can be mocked for testing

The reactor library provides the Active Object infrastructure; applications provide their own Servant
logic. This separation enables reusable async RPC handling across different applications, instead of every
application re-deriving its own synchronization around gRPC's callbacks.

### Why vcpkg with custom triplets?

1. **Multi-compiler Support**: Same project builds across multiple GCC and Clang versions
2. **ABI Compatibility**: Dependencies built with matching compiler
3. **Reproducibility**: Consistent builds across environments
4. **Debug + Release Mix**: Debug app can link Release libraries on Linux

### Why EventLoop library?

1. **Simple API**: `TriggerEvent()` / `RegisterEvent()` / `Run()`
2. **Thread-safe**: Safe cross-thread event dispatching
3. **Replaceable**: Interface allows swapping with other event systems

### Why three of the four reactors push instead of holding

`ActiveUnaryReactor`, `ActiveWriteReactor`, and `ActiveReadReactor` hand their response to a callback and keep
nothing for the application to pull, where `ActiveBidiReactor` keeps the response until the application pulls it
under a hold. The hold paced reads by the consumer: message N+1 could not be received until the application had
consumed message N, and a consumer that deferred processing stalled the stream. Pushing removes that coupling.
For the single terminal response of a unary or client-streaming RPC it costs nothing, because the RPC is already
over when that response arrives. For a stream of messages it costs two things.

| Cost                    | Cause                                   | Accepted because              |
|-------------------------|-----------------------------------------|-------------------------------|
| The queue is unbounded  | EventLoop has no backpressured delivery | The request bounds the volume |
| `Halt()` leaks messages | `Halt()` discards rather than drains    | The demo's loop never halts   |

Neither is acceptable for a stream whose volume the peer controls. Such a stream needs bounded buffering, which
was evaluated and rejected for an unresolved lost-wakeup race, recorded in
[reactor_client_response_ownership_sketch.md](/applications/reactor/reactor_client_response_ownership_sketch.md).

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
// ActiveBidiReactor is the only reactor that still pulls its response after checking the status.
if (reactor->Status().ok()) {
  ResponseT response;
  reactor->GetResponse(response);
}
```

**Rationale:** Status-based error handling aligns with Google C++ Style Guide (which discourages exceptions),
matches gRPC's native pattern, and works cleanly with the async callback model where exceptions cannot cross
thread boundaries.

### Thread safety patterns

**Atomic flags for cross-thread state:**

```cpp
std::atomic_bool response_ready_{false};   // Set by gRPC thread, read by app thread
std::atomic_bool stream_no_more_{false};   // Set by gRPC thread, read by app thread
```

`response_ready_` exists only in `ActiveBidiReactor`, where it guards `GetResponse()`. `stream_no_more_` guards
the write path of `ActiveWriteReactor`, and both the write path and the read restart of
`ActiveBidiReactor::GetResponse()`. `ActiveUnaryReactor` and `ActiveReadReactor` have neither flag, because
nothing they own is read from the application thread.

**Hold mechanism, `ActiveBidiReactor` read side:**

```cpp
// In OnReadDone - hold before returning to application
this->AddHold();

// In GetResponse - resume after application processes
this->StartRead(&response_);
this->RemoveHold();
```

`ActiveReadReactor` needs no equivalent. It swaps each message into a `std::unique_ptr<ResponseT>`, passes that
pointer to the `read_ok` callback, and calls `StartRead()` as the last statement of the reaction, all on the gRPC
thread. `ActiveUnaryReactor` and `ActiveWriteReactor` need no equivalent either, because their only response
leaves in the `done` callback, at a point where the RPC is already over.

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
