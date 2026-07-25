# Reactor implementation of gRPC clients

## Design overview

A gRPC thread invokes the reactor callbacks and the application thread is notified to process the response event.
A reactor that lets the application thread issue a `StartRead()` of its own uses the hold mechanism
(`AddHold()`/`RemoveHold()`) to prevent a concurrent `OnDone()` from destroying the reactor mid-call, which would
otherwise segfault. `ActiveBidiReactor` is the only reactor that needs it, because `ActiveReadReactor` re-arms its
reads from inside its own reaction and never leaves such a window open. The elimination of locks on application
state comes from the single-threaded event dispatch described below, not from the hold mechanism itself.

### The problem

The gRPC reactor callbacks (e.g. `OnDone`, `OnReadDone`) are executed on threads from the gRPC internal thread pool, but
the application cannot control which thread executes the callback. Client applications that use a single main thread or
event loop require response processing to occur on a thread which is managed by the application to maintain thread-safe
access with the software components of the main application.

### Comparison with direct callbacks

A direct callback implementation processes responses immediately within the gRPC thread callback and requires
synchronization mechanisms when accessing shared application state. The synchronization requirement propagates throughout
the application codebase, and all mutable state must be protected at every access point. The application loses
single-threaded execution guarantees and must handle concurrent access everywhere. Long processing times block threads
from the gRPC thread pool and reduce available concurrency.

For example, `RouteChat`'s server-side callback implementation
([route_guide_callback_server.cpp](/applications/callback/route_guide_callback_server.cpp)) cannot hold a single
lock around the shared notes vector for the whole exchange, unlike the non-reactor examples
([route_guide_sync_server.cpp](/applications/blocking/route_guide_sync_server.cpp)), because the reactor's read and
write callbacks can run on different gRPC threads between a read and a write. It instead locks twice: once to copy
the notes to send, and once to append the newly received note.

### The solution: Active Object pattern

This implementation uses the [Active Object pattern][active-object-pattern] to address the threading problem. The
result is single-threaded response processing without synchronization primitives. The pattern defers response
processing to the application thread through event notification. All responses are handled sequentially on a single
application thread.

In this implementation, the main application thread serves as the Active Object's thread. It actively runs the EventLoop
(Scheduler) which continuously processes queued events. gRPC reactor callbacks execute on gRPC internal threads and
enqueue events, and the response handlers process responses on the main application thread. This ensures all application
logic executes on a single thread without synchronization primitives.

### Library architecture

This reactor library implements the Active Object pattern infrastructure, designed for use by applications that provide
their own business logic.

**What the library provides:**

- **Method Request encapsulation**: `ActiveUnaryReactor` and `ActiveReadReactor` classes
- **Scheduler integration**: Callbacks trigger `EventLoop::TriggerEvent()`
- **Guard mechanism**: `AddHold()`/`RemoveHold()` prevents concurrent access during response processing, needed by
  `ActiveBidiReactor` only
- **Future-like access**: `Status()` for deferred status retrieval, and `GetResponse()` for the reactors that keep
  their response until the application pulls it. `ActiveReadReactor` instead pushes each message to its callback
  as an owned object

**What applications provide:**

- **Servant (business logic)**: Implemented in `EventLoop::RegisterEvent()` handlers
- **Domain-specific processing**: Transform RPC responses into application state changes

**Demo vs production:**

The demo application (`route_guide_active_reactor_client.cpp`) uses simple logging as Servant logic:

```cpp
// Demo: Logging only
EventLoop::RegisterEvent(kGetFeatureOnDone, [](const Event* event) {
  logger.info("RESPONSE | {}", response);  // Placeholder Servant logic
});
```

Production applications implement actual business logic:

```cpp
// Production: Real Servant logic
EventLoop::RegisterEvent(kGetFeatureOnDone, [&app_state](const Event* event) {
  routeguide::Feature response;
  if (reactor->GetResponse(response)) {
    app_state.UpdateFeatureCache(response);      // Business logic
    ui_controller.NotifyFeatureLoaded(response); // State changes
    metrics.RecordFeatureQuery(response);        // Application concerns
  }
});
```

This separation is intentional: the library handles the concurrency complexity, applications handle the domain logic.

### Pattern components

This implementation follows the Active Object pattern, combining it with the Reactor pattern for event-driven
processing.

**Components mapping:**

| Active Object Component | This Implementation                       | Notes                                        |
|-------------------------|-------------------------------------------|----------------------------------------------|
| Proxy                   | `GetFeature()`, `ListFeatures()` methods  | Creates Method Requests, returns immediately |
| Method Request          | `ActiveUnaryReactor`, `ActiveReadReactor` | Encapsulates RPC state                       |
| Activation Queue        | EventLoop internal queue                  | Holds pending notifications, and the owned   |
|                         |                                           | messages pushed by `ActiveReadReactor`       |
| Scheduler               | `EventLoop::Run()`                        | Dispatches events to handlers                |
| Servant                 | `EventLoop::RegisterEvent()` handlers     | Application-provided business logic          |
| Future                  | `GetResponse()`, `Status()`               | Deferred result access, `Status()` alone in  |
|                         |                                           | `ActiveReadReactor`                          |
| Guards                  | `AddHold()`/`RemoveHold()`                | Prevents concurrent access during processing,|
|                         |                                           | in `ActiveBidiReactor` only                  |

**Library vs application responsibilities:**

| Aspect  | Definition                                         | This Implementation                                   |
|---------|----------------------------------------------------|-------------------------------------------------------|
| Guards  | Method Requests have `guard()` for synchronization | `AddHold()`/`RemoveHold()`, `ActiveBidiReactor` only  |
| Servant | Business logic component with state                | Application-provided (demo uses logging placeholders) |
| Scope   | Full client-server encapsulation                   | Client-side library (server has separate Servant)     |

**Combined pattern characteristics:**

- **Active Object**: Proxy, Method Request, Activation Queue, Scheduler, Servant, Future, Guards
- **Reactor pattern**: Event demultiplexing, event handlers, non-blocking I/O

Reactor Pattern (Event-driven concurrency):

- Events from gRPC (`OnDone`, `OnReadDone`) are received and demultiplexed
- Events are handled asynchronously without blocking
- Event detection is separated from event handling

## API naming conventions

The reactor library exposes a high-level application API that abstracts gRPC's internal terminology. The naming
reflects client/server semantics rather than gRPC implementation details.

### Client vs server message semantics

| Role       | Sends     | Receives  |
|------------|-----------|-----------|
| **Client** | Requests  | Responses |
| **Server** | Responses | Requests  |

### gRPC internal API (hidden from application)

These gRPC functions are called internally by the reactor classes and are not exposed to application code:

| gRPC Function           | Purpose                          | Called by                                         |
|-------------------------|----------------------------------|---------------------------------------------------|
| `StartCall()`           | Initiates the RPC                | Specialized constructor                           |
| `StartRead(&response_)` | Begins async read operation      | Constructor, `OnReadDone()`, and `GetResponse()`  |
|                         |                                  | for `ActiveBidiReactor`                           |
| `StartWrite()`          | Begins async write operation     | Inside `SendRequest()`                            |
| `StartWriteLast()`      | Write + implied close, one op    | Inside `SendLastRequest()`                        |
| `StartWritesDone()`     | Signals end of client writes     | Inside `CloseRequestStream()`                     |
| `AddHold()`             | Prevents OnDone until RemoveHold | Inside `ActiveBidiReactor::OnReadDone()`          |
| `RemoveHold()`          | Releases hold, allows OnDone     | Inside `ActiveBidiReactor::GetResponse()`         |

### gRPC callbacks (protected overrides)

These callbacks are invoked by gRPC and handled internally by the reactor classes:

| gRPC Callback               | When it fires                        | Invokes user callback             |
|-----------------------------|--------------------------------------|-----------------------------------|
| `OnReadDone(bool ok)`       | Read operation completed             | `cbs_.ok`/`cbs_.nok`              |
| `OnWriteDone(bool ok)`      | Write operation completed            | `cbs_.write_done`                 |
| `OnWritesDoneDone(bool ok)` | Explicit StartWritesDone() completed | none (internal only)              |
| `OnDone(Status)`            | RPC terminated                       | `cbs_.done`                       |

`ActiveBidiReactor` uses `cbs_.read_ok`/`cbs_.read_nok` instead of `cbs_.ok`/`cbs_.nok` for `OnReadDone`.

### Stream completion tracking

`ActiveWriteReactor` and `ActiveBidiReactor` track whether further read/write operations are still valid via an
internal `stream_no_more_` flag. `ActiveReadReactor` needs no such flag, because it only ever issues a
`StartRead()` from inside a reaction, where the `ok` flag it just received already answers the same question:

- Set by `OnReadDone(false)`, `OnWriteDone(false)`, `OnWritesDoneDone()`, and `OnDone()` (whichever apply to that
  reactor's direction).
- Checked before issuing a new `StartRead()`, `StartWrite()`, `StartWriteLast()`, or `StartWritesDone()`.
- `ActiveBidiReactor` uses a single flag for both directions instead of one per direction. Per gRPC's own
  contract (`grpcpp/support/client_callback.h`), a failure on either read or write means no new read/write
  operation will succeed, so tracking the two directions separately would not add information.
- `OnWritesDoneDone()` fires only for an explicit `StartWritesDone()` (i.e. `CloseRequestStream()`), not for a
  close implied via `StartWriteLast()` (i.e. `SendLastRequest()`). This is per gRPC's own documented distinction.
- This narrows, but does not fully close, a race with a concurrent `OnDone()`: the flag can flip to true right
  after a call already checked it. Closing that race fully would need the same `AddHold()`/`RemoveHold()`
  protection `ActiveBidiReactor::OnReadDone()` already uses for its read direction.

### Application-facing API

These methods are exposed to application code with naming that reflects application-level semantics:

| Method                 | Purpose                                      | Notes                                        |
|------------------------|----------------------------------------------|----------------------------------------------|
| `SendRequest()`        | Send a request message on the stream         | Takes ownership; caller gives up the request |
| `SendLastRequest()`    | Send the final request and close, atomically | For a known-last message                     |
| `CloseRequestStream()` | Signal end of client requests                | Returns false if rejected (see below)        |
| `GetResponse()`        | Extract a received response via swap         | Not on `ActiveReadReactor`, which pushes     |
|                        |                                              | each message to its ok callback instead      |
| `TryCancel()`          | Cancel the entire RPC                        | Thread-safe, any thread                      |
| `Status()`             | Get final RPC status                         | Valid after `OnDone`                         |

`CloseRequestStream()` returns `false` (does nothing) if already closed, if a write is still in flight, or if the
RPC has already failed/finished. Callers should wait for `OnWriteDone()` and retry, or use `SendLastRequest()`
instead when the last message is known in advance.

### Future server-side API

For server-side reactors, the naming will mirror client-side with role reversal:

| Method                  | Purpose                               | Notes                            |
|-------------------------|---------------------------------------|----------------------------------|
| `SendResponse()`        | Send a response message on the stream | Server sends responses           |
| `CloseResponseStream()` | Signal end of server responses        | Stream remains open for requests |
| `GetRequest()`          | Extract a received request via swap   | Server receives requests         |
| `Finish(status)`        | Complete the RPC with status          | Server-initiated termination     |

### Complete API matrix

#### Client-side reactors

| RPC Type          | Send             | Close Stream           | Receive          | Cancel         | Status     |
|-------------------|------------------|------------------------|------------------|----------------|------------|
| **Unary**         | (constructor)    | N/A                    | `GetResponse()`  | `TryCancel()`  | `Status()` |
| **Server-Stream** | (constructor)    | N/A                    | ok callback,     | `TryCancel()`  | `Status()` |
|                   |                  |                        | owned message    |                |            |
| **Client-Stream** | `SendRequest()`  | `CloseRequestStream()` | `GetResponse()`  | `TryCancel()`  | `Status()` |
| **Bidi**          | `SendRequest()`  | `CloseRequestStream()` | `GetResponse()`  | `TryCancel()`  | `Status()` |

#### Server-side reactors (future)

| RPC Type          | Receive        | Send               | Close Stream            | Finish           |
|-------------------|----------------|--------------------|-------------------------|------------------|
| **Unary**         | (params)       | (via `Finish()`)   | N/A                     | `Finish(status)` |
| **Server-Stream** | (params)       | `SendResponse()`   | `CloseResponseStream()` | `Finish(status)` |
| **Client-Stream** | `GetRequest()` | (via `Finish()`)   | N/A                     | `Finish(status)` |
| **Bidi**          | `GetRequest()` | `SendResponse()`   | `CloseResponseStream()` | `Finish(status)` |

### Bidirectional stream message flow

```text
Client                              Server
  |                                   |
  |---SendRequest()----------------->|  GetRequest()
  |---SendRequest()----------------->|  GetRequest()
  |---CloseRequestStream()---------->|  (read stream ends)
  |                                   |
  |<------------------SendResponse()--|
  |  GetResponse()                    |
  |<------------------SendResponse()--|
  |  GetResponse()                    |
  |<-----------CloseResponseStream()--|
  |  (read stream ends)               |
```

## Component implementation

The implementation is organized across three architectural layers, mapping Active Object concepts to concrete code.

| File                                    | Components                      | Layer                |
|-----------------------------------------|---------------------------------|----------------------|
| `reactor_client.h`                      | Method Request, Future, Guards  | Generic (reusable)   |
| `reactor_client_routeguide.h`           | Method Request (specialized)    | Service-specific     |
| `route_guide_active_reactor_client.cpp` | Proxy, Servant                  | Application          |
| EventLoop library (external)            | Scheduler, Activation Queue     | Infrastructure       |

| Pattern Component  | Description                                    | Code Elements                                  |
|--------------------|------------------------------------------------|------------------------------------------------|
| Proxy              | Client methods creating reactors               | `GetFeature()`, `ListFeatures()`               |
| Scheduler          | Event loop dispatching to app thread           | `EventLoop::Run()`, `TriggerEvent()`           |
| Activation Queue   | Event loop queue of notifications and messages | EventLoop internal queue                       |
| Method Request     | Reactor instances encapsulating RPC state      | `ActiveUnaryReactor`, `ActiveReadReactor`      |
| Servant            | Application business logic handlers            | `RegisterEvent()` handlers                     |
| Future             | Reactor handle for retrieving results          | `GetResponse()`, `Status()`. `Status()` alone  |
|                    |                                                | in `ActiveReadReactor`                         |
| Guards             | Prevents concurrent access during processing   | `AddHold()`, `RemoveHold()`, in                |
|                    |                                                | `ActiveBidiReactor` only                       |

### Proxy component

The Proxy component provides the client-facing interface that applications call to initiate RPC operations. Proxy
methods run on the client application thread and create reactor instances (Method Requests) without blocking.

> **Note on terminology:** This component shares characteristics with the GoF Proxy pattern (providing a surrogate
> interface) but serves a different purpose. In Active Object, the Proxy transforms synchronous method calls into
> asynchronous Method Requests, whereas GoF Proxy primarily controls access to an object. The term "proxy" here
> describes the component's role in the Active Object pattern, not a direct application of the GoF Proxy pattern.

The Proxy method constructs the reactor, configures callbacks to notify the Scheduler (EventLoop), and returns control
immediately to the caller.

### Scheduler & Activation Queue components

The Scheduler dispatches queued events to the application thread, and the Activation Queue is the internal queue holding
pending notifications. These components are provided by the [EventLoop library][eventloop-lib].

The Scheduler bridges gRPC threads to the application thread. gRPC callbacks invoke `TriggerEvent()`, which enqueues
notifications in the Activation Queue. The Scheduler dequeues and dispatches them to response handlers on the
application thread, maintaining single-threaded execution.

For `ActiveReadReactor`, the queue carries the message itself rather than a notification that one is ready. The
resulting obligations are listed under [Message ownership](#message-ownership), and the trade-offs they come from
are in [architecture.md](/docs/architecture.md).

### Method Request component

The Method Request component encapsulates an RPC invocation with all necessary state: `ClientContext`, request message,
response message, and callbacks. The reactor instances (e.g. `ActiveUnaryReactor`, `ActiveReadReactor`) implement this
component.

### Servant component

The Servant component contains application-provided business logic. In this client-side implementation, the Servant is
implemented via `EventLoop::RegisterEvent()` handlers that process RPC results on the application thread.

The response handling provides two execution strategies:

1. **Immediate processing**: Early callbacks (`OnDoneCallback`, `OnReadDoneOkCallback`) execute on gRPC threads for
   quick decisions or lightweight processing. For `ActiveReadReactor`, the immediate decision available is whether
   to hand the owned message on to the application thread or to drop it
2. **Deferred processing**: Handlers registered via `EventLoop::RegisterEvent()` execute on the application thread for
   response processing after Scheduler dispatch

The demo application uses simple logging as placeholder Servant logic. Production applications implement actual
business logic (state updates, UI notifications, domain processing) in these handlers.

### Future component

The Future component provides asynchronous access to RPC results. The reactor instance acts as the Future, exposing
`GetResponse()` to retrieve response data and `Status()` to check operation success or failure. `ActiveReadReactor`
is the exception: it exposes `Status()` only, because it pushes each message to its callback instead of holding it
for a later retrieval.

### Guards component

The Guards component prevents `OnDone()` from concluding an RPC, and destroying its underlying gRPC-bound state,
while the application still has an operation pending outside of any gRPC reaction. It is implemented via gRPC's
`AddHold()`/`RemoveHold()` mechanism, and `ActiveBidiReactor` is the only reactor of this library that needs it.

#### Why ActiveBidiReactor holds before returning from OnReadDone

When `ActiveBidiReactor`'s `OnReadDoneOkCallback` returns true, the application thread will call `StartRead()`
later, from `GetResponse()`, not immediately inside the gRPC callback. Between that return and the later
`StartRead()` call, a concurrent `OnDone()` could still arrive and destroy the reactor's underlying gRPC-bound
callback state. Calling `StartRead()` at that point would forward through that now-destroyed object and segfault.
Other reactor operations, `Status()` and `TryCancel()`, do not touch gRPC's internal object at all, so they remain
safe to call regardless of hold state; only the deferred `StartRead()` needs the hold's protection. `AddHold()`
prevents `OnDone()` from firing during this window, and `RemoveHold()` in `GetResponse()` releases it once the new
`StartRead()` is issued (or skipped, if the stream is done). This is the gap gRPC's own callback API leaves open
by design, and why it added the [hold mechanism][grpc-hold-pr].

#### Why ActiveReadReactor needs no hold

`ActiveReadReactor` closes that window instead of protecting it. Its `OnReadDone()` re-arms the read itself, as the
reaction's last action, so no `StartRead()` is ever issued from outside a reaction. A reaction in progress is
itself proof that the gRPC-bound state still exists, which is the guarantee the hold provided.

Re-arming after the callback also bounds the reaction to one at a time, since a read cannot complete before it is
started. Messages therefore reach the event loop in network order.

#### Hold semantics per RPC, not per direction

gRPC's hold count (`AddHold()`/`RemoveHold()`) is a single counter shared by the entire RPC, not one counter per
read/write direction, per gRPC's documented public contract (`grpcpp/support/client_callback.h`, not vendored in
this repository). This has a direct consequence for `ActiveBidiReactor`:

- A hold added in `ActiveBidiReactor::OnReadDone()` (protecting a response held for later `GetResponse()`) does not
  block other, independent reactions from firing. In particular, `OnWriteDone(false)` on an unrelated in-flight
  write can still fire and set `stream_no_more_` while that hold is outstanding. The hold only gates `OnDone()`,
  not other callbacks.
- Consequently, `GetResponse()` must call `RemoveHold()` unconditionally, whether or not it restarts reading.
  Only the restart is conditional on `stream_no_more_`. Skipping `RemoveHold()` when `stream_no_more_` happens to
  already be true would leak the hold and stall the RPC (`OnDone()` would never fire) rather than fail loudly.

## Unary RPC client

gRPC API keywords: ClientUnaryReactor, ClientCallbackUnary

Both synchronous (blocking) and asynchronous methods are possible; the reactor is an asynchronous variant.

### ActiveUnaryReactor class

Inherit from `grpc::ClientUnaryReactor`, this class implements the Method Request component of the
[Active Object pattern][active-object-pattern] and uses the [Reactor pattern][reactor-pattern] for event handling:

- This class is meant to run on concurrent threads.
- Its constructor is executed by the caller and once the task is done, the caller must destroy it. The
  `ClientUnaryReactor::OnDone` event is about that situation.
- All variables needed for the task execution (e.g. proto messages, gRPC context) belongs to this object instance and
  their lifetime is implicitly bound to the instance life.
- Thread-safety: The scheduling of its execution comes mainly from gRPC events: it implies there's no concurrent gRPC
  threads to take care at the same time.
- Thread-unsafe: the gRPC callback (i.e. `ClientUnaryReactor::OnDone`) is not executed on client thread but on a
  random one (from gRPC thread pool). It is the responsibility of the user to provide a thread-safe callback function.
  To ease that situation, the `ActiveUnaryReactor` class provides a callback slot (i.e. `OnDoneCallback`)
  which is called when `ClientUnaryReactor::OnDone` is signaled by the ClientCallbackUnary. That callback is meant to be
  assigned by the client side to notify its eventloop to allocate a shared time on its scheduler and proceeding with the
  active reactor instance.

When `ClientUnaryReactor::OnDone` event is handled in the active reactor, the arguments of the `OnDoneCallback` callback
function are:

````cpp
using OnDoneCallback = std::function<void(grpc::ClientUnaryReactor* reactor, const grpc::Status&, const ResponseT&)>;
````

- the pointer to the active reactor instance (i.e. `this`)
- a reference to the `grpc::Status` content
- a reference to the `Response` content

For the sake of the gRPC processing, it is strongly discouraged to process the response during that callback event. They
are provided to ease some early-reaction logics or to select situations where the response handling is worthy or not.

#### Class functions

Three public functions can be called by the application side:

- `bool GetResponse(ResponseT& response)`
- `const grpc::Status& Status()`
- `void TryCancel()`

`GetResponse()` function swaps the underlying data storage of the response object. The swap mechanism is important to
avoid a deep-copy of the content of the response. For the `ActiveUnaryReactor`, having the response swapped is acceptable
because the unary RPC is meant to one response only, so the swap is a good technique to speed up the response proceeding
time. The function will return true when the returned response is valid.

`Status()` function simply returns a reference to the `grpc::Status` of the active reactor. Initialized as `Status::OK`,
its content is updated once `ClientUnaryReactor::OnDone` event is received. It means calling `Status()` at any other
moments is meaningless.

`TryCancel()` function simply sends a best-effort out-of-band cancel to the RPC. That signal is thread-safe and can be
sent anytime from any thread. The goal of that signal is to provoke (immediately or later) the
`ClientUnaryReactor::OnDone` event.

### Code snippet

On purpose, the following examples are coming from a sandbox code using a 3rdparty eventloop library.

- [route_guide_active_reactor_client.cpp](/applications/reactor/route_guide_active_reactor_client.cpp)
- [EventLoop][eventloop-lib]

For the sanity of the reader, some passages are removed and the code logic reduced to its maximum. It is recommended
to read the original code from the route_guide_active_reactor_client.cpp file.

#### Instantiation of the ActiveUnaryReactor class

The following snippet instances a `ActiveUnaryReactor` dedicated to the `GetFeature` RPC of the `routeguide` API. It
fills a callback structure with the related `OnDoneCallback`. In this example, the callback is bound to the eventloop
notification function as an `kGetFeatureOnDone` event.

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 3.4 : the `cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void GetFeature(routeguide::Point point) {
  using routeguide::GetFeature::ClientReactor;
  using routeguide::GetFeature::Callbacks;
  using routeguide::GetFeature::ResponseT;
  using routeguide::GetFeature::RpcKey;
  Callbacks cbs;
  cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
    EventLoop::TriggerEvent(kGetFeatureOnDone, reactor);  // Signal OnDoneCallback event from gRPC thread
  };
  reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_,
                                                         std::move(CreateClientContext()),
                                                         std::move(point),
                                                         std::move(cbs));
}
````

#### OnDoneCallback

> [!IMPORTANT]
> The last action to do when handling that event is to delete the reactor instance: that instance can't be
> reused by gRPC and a new instance must be allocated for a new usage. In typical gRPC examples, the reactors are
> self-destroying (explicit `delete this` or on class d-tor execution) once that event is done:
> [Asynchronous Callback API Tutorial][grpc-callback-tutorial].

The following snippet is an example code of the application-side callback. That code is meant to be executed on the main
application thread, scheduled by the eventloop of the application. In this example, that lambda is used as the callback
and given to the eventloop when the `kGetFeatureOnDone` is notified.

From the PlantUML sequence diagram, the corresponding points are from 3.6 to 3.8.

````cpp
EventLoop::RegisterEvent(kGetFeatureOnDone, [&reactor_ = reactor_map_[GetFeature::RpcKey]](const Event*) {
  if (reactor_->Status().ok()) {
    routeguide::Feature response;
    bool valid = reactor_->GetResponse(response);
    /**  proceeding of the content of response **/
  }
  reactor_.reset();
});
````

### PlantUML sequence flow

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50
title gRPC Client reactor for unary RPC

boundary    app      as "Application\nside"
box "ActiveUnaryReactor" #beige
collections data     as "Data"
control     reactor  as "ClientUnaryReactor"
end box
entity      grpc     as "gRPC\nClientCallbackUnary"

legend top center
  <font color=blue><b>blue</b>: main application thread
  <font color=green><b>green</b>: gRPC thread pool
endlegend

activate app #lightblue

autonumber 1.1
== 1. RPC establishment ==
    app -[#darkblue]> reactor : Create Reactor
    activate reactor
    data o-[#darkblue]->> reactor : Response holder
    & reactor -[#darkblue]\ grpc : async RPC service call\nstub.async()->RpcMethod()
    activate grpc #lightgreen
    reactor -[#darkblue]\ grpc : StartCall()
    activate grpc #green
    & grpc --\? : send Request\nto server

...
autonumber 2.1
== 2. Client-side cancellation ==
    app -[#darkblue]> reactor : TryCancel()
    & reactor -[#darkblue]\ grpc : TryCancel()
    & grpc --\? : RPC termination\nbehest

autonumber 3.1
== 3. RPC completion ==
    grpc <--? : RPC termination
    &grpc -[#darkgreen]> data : <i>writes response
    activate data #yellow
    deactivate grpc
    reactor <[#darkgreen]- grpc : OnDone
    deactivate grpc
group #lightgreen (grpc-thread callback) ondone
    {start3} app /[#darkgreen]- reactor : TriggerEvent : OnDone
end
    ...
    {end3} app -[#darkblue]> reactor : ProceedEvent: OnDone
group #lightblue (app-thread callback) ondone
    {start3} <-> {end3} : Eventloop
    data o-[#darkblue]> reactor : <i>extracts response
    deactivate data
    reactor -[#darkblue]> app : <i>update application with response
    activate app #blue
    deactivate app
    app -[#darkblue]> reactor : Destroy Reactor
    destroy reactor
end
```

## Server-side streaming RPC client

gRPC API keywords: ClientReadReactor, ClientCallbackReader

Only the asynchronous method is provided.

### ActiveReadReactor class

Inherit from `grpc::ClientReadReactor`, this class implements the Method Request component of the
[Active Object pattern][active-object-pattern] and uses the [Reactor pattern][reactor-pattern] for event handling:

- This class is meant to run on concurrent threads.
- Its constructor is executed by the caller and once the task is done, the caller must destroy it. The
  `ClientReadReactor::OnDone` event is about that situation.
- All variables needed for the task execution (e.g. proto messages, gRPC context) belongs to this object instance and
  their lifetime is implicitly bound to the instance life.
- Thread-safety: The scheduling of its execution comes mainly from gRPC events: it implies there's no concurrent gRPC
  threads to take care at the same time.
- Thread-unsafe: the gRPC callback (i.e. `ClientReadReactor::OnDone`) is not executed on client thread but on a
  random one (from gRPC thread pool). It is the responsibility of the user to provide a thread-safe callback function.
  To ease that situation, the `ActiveReadReactor` class provides different callback slots which are called when
  the related event is signaled by the ClientReadReactor. That callback is meant to be assigned by the client side to
  notify its eventloop to allocate a shared time on its scheduler and proceeding with the active reactor instance.

The three callbacks are: `OnReadDoneOkCallback`, `OnReadDoneNOkCallback`, and `OnDoneCallback`.

````cpp
using OnDoneCallback = std::function<void(grpc::ClientReadReactor<ResponseT>* reactor, const grpc::Status&)>;
````

When `ClientReadReactor::OnDone` event is handled in the active reactor, the `OnDoneCallback` callback function is called
with the following arguments:

- the pointer to the active reactor instance (i.e. `this`)
- a reference to the `grpc::Status` content

````cpp
using OnReadDoneNOkCallback = std::function<void(grpc::ClientReadReactor<ResponseT>* reactor)>;
````

When `ClientReadReactor::OnReadDone` event with a negative OK is handled in the active reactor, the
`OnReadDoneNOkCallback` callback function is called with the following argument:

- the pointer to the active reactor instance (i.e. `this`)

````cpp
using OnReadDoneOkCallback =
    std::function<void(grpc::ClientReadReactor<ResponseT>* reactor, std::unique_ptr<ResponseT>)>;
````

When `ClientReadReactor::OnReadDone` event with a positive OK is handled in the active reactor, the
`OnReadDoneOkCallback` callback function is called with the following argument:

- the pointer to the active reactor instance (i.e. `this`)
- the received message, whose ownership transfers to the callback

The ownership transfer distinguishes that callback from the two others. The reactor moves the message out of its
read target before the call and re-arms the read after it, so it never refers to that message again.

Processing the message inside the callback stays discouraged, because the callback runs on a gRPC thread. The
callback exists to hand the message to the application thread, or to discard it: dropping the `std::unique_ptr` is
all a discard takes.

#### Message ownership

Handing the message to an event queue makes the application responsible for it. Four obligations follow, each from
a property of the [EventLoop library][eventloop-lib]:

| Obligation                              | Why                                      | Cost                 |
|-----------------------------------------|------------------------------------------|----------------------|
| Reclaim the pointer into a `unique_ptr` | The queue carries a bare `void*`         | Leak                 |
| Reclaim it in exactly one handler       | Every callback under one name is invoked | Double free          |
| Reclaim before anything can throw       | Nothing else owns it meanwhile           | Leak                 |
| Drain the queue before `Halt()`         | `Halt()` discards, it does not drain     | Queued messages leak |

The event data is one pointer, so an application reading several concurrent streams of the same RPC cannot tell
which reactor produced a message. It must use a distinct event name per reactor instance, or pass the reactor
pointer the callback received alongside the message.

#### Class functions

Two public functions can be called by the application side:

- `const grpc::Status& Status()`
- `void TryCancel()`

`ActiveReadReactor` exposes no `GetResponse()`, because nothing is left in the reactor to pull. Its internal
`response_` read target keeps a stable address across every read, and each message is swapped out of it by
pointer, without a deep-copy, before the next read is armed.

`Status()` function simply returns a reference to the `grpc::Status` of the active reactor. Initialized as `Status::OK`,
its content is updated once `ClientReadReactor::OnDone` event is received. It means calling `Status()` at any other
moments is meaningless.

`TryCancel()` function simply sends a best-effort out-of-band cancel to the RPC. That signal is thread-safe and can be
sent anytime from any thread. The goal of that signal is to provoke (immediately or later) the
`ClientReadReactor::OnDone` event.

### Code snippet

On purpose, the following examples are coming from a sandbox code using a 3rdparty eventloop library.

- [route_guide_active_reactor_client.cpp](/applications/reactor/route_guide_active_reactor_client.cpp)
- [EventLoop][eventloop-lib]

For the sanity of the reader, some passages are removed and the code logic reduced to its maximum. It is recommended
to read the original code from the route_guide_active_reactor_client.cpp file.

#### Instantiation of the ActiveReadReactor class

The following snippet instances a `ActiveReadReactor` dedicated to the `ListFeatures` RPC of the `routeguide` API. It
fills a callback structure with the related bound functions. In this example, the callbacks are bound to the eventloop
notification function as an `kListFeaturesOnReadDoneOk`, `kListFeaturesOnReadDoneNOk`, or `kListFeaturesOnDone` event.

The implementation of the `OnReadDoneOkCallback` is different, because it receives the message as an owned object
and releases that ownership into the event queue. The two other callbacks pass the reactor pointer instead.

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.5 : the `cbs.ok = [](auto*, std::unique_ptr<ResponseT> response) {...}` lines
- 4.3 : the `cbs.nok = [](auto* reactor) {...}` lines
- 4.6 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void ListFeatures(routeguide::Rectangle rect) {
  using routeguide::ListFeatures::ClientReactor;
  using routeguide::ListFeatures::Callbacks;
  using routeguide::ListFeatures::RpcKey;
  Callbacks cbs;
  cbs.ok = [](auto*, std::unique_ptr<routeguide::Feature> response) {
    // Signal OnReadDoneOkCallback from gRPC thread, handing the message ownership to the queue
    EventLoop::TriggerEvent(kListFeaturesOnReadDoneOk, response.release());
  };
  cbs.nok = [](auto* reactor) {
    EventLoop::TriggerEvent(kListFeaturesOnReadDoneNOk, reactor);  // Signal OnReadDoneNOkCallback from gRPC thread
  };
  cbs.done = [](auto* reactor, const grpc::Status&) {
    EventLoop::TriggerEvent(kListFeaturesOnDone, reactor);  // Signal OnDoneCallback event from gRPC thread
  };
  reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_,
                                                         std::move(CreateClientContext()),
                                                         std::move(rect),
                                                         std::move(cbs));
}
````

#### OnDoneCallback

> [!IMPORTANT]
> The last action to do when handling that event is to delete the reactor instance: that instance can't be
> reused by gRPC and a new instance must be allocated for a new usage. In typical gRPC examples, the reactors are
> self-destroying (explicit `delete this` or on class d-tor execution) once that event is done:
> [Asynchronous Callback API Tutorial][grpc-callback-tutorial].

The following snippet is an example code of the application-side callback. That code is meant to be executed on the main
application thread, scheduled by the eventloop of the application. In this example, that lambda is used as the callback
and given to the eventloop when the `kListFeaturesOnDone` is notified. The main goal of that callback is to destroy
the active reactor instance.

From the PlantUML sequence diagram, the corresponding points are 4.10 and 4.11.

````cpp
EventLoop::RegisterEvent(kListFeaturesOnDone, [&reactor_ = reactor_map_[ListFeatures::RpcKey]](const Event*) {
  const auto status = reactor_->Status();
  /**  proceeding of the status **/
  reactor_.reset();
});
````

#### OnReadDoneNOkCallback

The following snippet is an example code of the application-side callback. That code is meant to be executed on the main
application thread, scheduled by the eventloop of the application. In this example, that lambda is used as the callback
and given to the eventloop when the `kListFeaturesOnReadDoneNOk` is notified. The interest of that callback is to take
care of the situation when the stream reading is done (no more upcoming responses), but the RPC is still active.

From the PlantUML sequence diagram, the corresponding point is 4.8

````cpp
EventLoop::RegisterEvent(kListFeaturesOnReadDoneNOk, [](const Event*) {
  /** proceeding of the end of the stream reading **/
});
````

#### OnReadDoneOkCallback

The following snippet is an example code of the application-side callback. That code is meant to be executed on the main
application thread, scheduled by the eventloop of the application. In this example, that lambda is used as the callback
and given to the eventloop when the `kListFeaturesOnReadDoneOk` is notified. The main goal of that callback is to
reclaim the ownership of the message the gRPC-thread callback released into the queue, and then to process it. The
handler needs no access to the reactor, because there is no response left to extract from it and no read to
restart: the reactor already armed its next read before this handler ran.

The reclaim must precede any statement that can throw or return early, so the message is freed on those paths too.
Exactly one handler may reclaim it: EventLoop invokes every callback registered under one event name, so a second
handler for the same name would free the same message twice.

From the PlantUML sequence diagram, the corresponding points are 2.8 and 2.9.

````cpp
EventLoop::RegisterEvent(kListFeaturesOnReadDoneOk, [](const Event* event) {
  const std::unique_ptr<routeguide::Feature> response{static_cast<routeguide::Feature*>(event->getData())};
  /**  proceeding of the content of response **/
});
````

### PlantUML sequence flow

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for server-side stream RPC

box "Application side" #powderblue
boundary    app      as "Application\nside"
queue       equeue   as "EventLoop\nQueue"
end box
control     reactor  as "ClientReadReactor"
entity      grpc     as "gRPC\nClientCallbackReader"

legend top center
  <font color=blue><b>blue</b>: main application thread
  <font color=green><b>green</b>: gRPC thread pool
endlegend

activate app #lightblue

autonumber 1.1
== 1. Stream establishment ==
    app -[#darkblue]> reactor : Create Reactor
    activate reactor
    & reactor -[#darkblue]\ grpc : async RPC service call\nstub.async()->RpcMethod()
    activate grpc #lightgreen
    reactor -[#darkblue]> reactor : <i>internal read target ready
    reactor -[#darkblue]\ grpc : StartRead()
    reactor -[#darkblue]\ grpc : StartCall()
    activate grpc #green
    & grpc --\? : send Request\nto server

...
autonumber 2.1
== 2. Stream reading ==
loop
    grpc <--? : receive Response\nfrom server
    &grpc -[#darkgreen]> reactor : <i>writes response
    activate reactor #gold
group #lightgreen (grpc-thread callback) onreaddoneok
    reactor <[#darkgreen]- grpc : OnReadDone : true
    reactor -[#darkgreen]> reactor : <i>extracts response\ninto owned message
    {start1} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ owned message
    activate equeue #blue
    deactivate reactor
    reactor -[#darkgreen]\ grpc : StartRead()
end
    grpc -> grpc : reading continues\n(no wait, no hold)
group #lightblue (app-thread callback) onreaddoneok
    {end1} equeue -[#darkblue]> app : ProceedEvent: OnReadDoneOk\n+ owned message
    {start1} <-> {end1} : Eventloop
    deactivate equeue
    app -[#darkblue]> app : <i>processes owned message
    activate app #blue
    deactivate app
end
end
...
autonumber 3.1
== 3. Client-side cancellation ==
    app -[#darkblue]> reactor : TryCancel()
    & reactor -[#darkblue]\ grpc : TryCancel()
    & grpc --\? : RPC termination\nbehest

autonumber 4.1
== 4. Stream termination ==
    grpc <--? : depleted stream
    & reactor <[#darkgreen]- grpc : OnReadDone : false
    deactivate grpc
group #lightgreen (grpc-thread callback) onreaddonenok
    {start3} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneNok
    activate equeue #blue
end

    grpc <--? : RPC termination
    & reactor <[#darkgreen]- grpc : OnDone
    deactivate grpc
group #lightgreen (grpc-thread callback) ondone
    equeue /[#darkgreen]- reactor : TriggerEvent : OnDone
end
...
    equeue -[#darkblue]> app : ProceedEvent: OnReadDoneNok
group #lightblue (app-thread callback) onreaddonenok
    reactor -[#darkblue]> app : <i>update application
    activate app #blue
    deactivate app
end
    {end3} equeue -[#darkblue]> app : ProceedEvent: OnDone
group #lightblue (app-thread callback) ondone
    reactor -[#darkblue]> app : <i>update application with status
    activate app #blue
    deactivate app
    deactivate equeue
    {start3} <-> {end3} : Eventloop
    app -[#darkblue]> reactor : Destroy Reactor
    destroy reactor
end
```

## Client-side streaming RPC client

gRPC API keywords: ClientWriteReactor, ClientCallbackWriter

Implemented and unit-tested: `ActiveWriteReactor` (Method Request, Future) in
[reactor_client.h](/applications/reactor/reactor_client.h), specialized as `RecordRoute::ClientReactor` in
[reactor_client_routeguide.h](/applications/reactor/reactor_client_routeguide.h). Test coverage:
[active_write_reactor_test.cpp](/applications/reactor/tests/active_write_reactor_test.cpp).

No example client wiring (Proxy method, EventLoop handlers) exists yet in
[route_guide_active_reactor_client.cpp](/applications/reactor/route_guide_active_reactor_client.cpp); that
application still only calls `GetFeature()` and `ListFeatures()`.

## Bidirectional streaming RPC client

gRPC API keywords: ClientBidiReactor, ClientCallbackReaderWriter

Implemented and unit-tested: `ActiveBidiReactor` (Method Request, Future) in
[reactor_client.h](/applications/reactor/reactor_client.h), specialized as `RouteChat::ClientReactor` in
[reactor_client_routeguide.h](/applications/reactor/reactor_client_routeguide.h). Test coverage:
[active_bidi_reactor_test.cpp](/applications/reactor/tests/active_bidi_reactor_test.cpp).

No example client wiring exists yet in
[route_guide_active_reactor_client.cpp](/applications/reactor/route_guide_active_reactor_client.cpp), same as the
client-streaming case above.

<!-- Reference links -->
[active-object-pattern]: https://www.modernescpp.com/index.php/active-object/
[reactor-pattern]: https://www.modernescpp.com/index.php/reactor/
[eventloop-lib]: https://github.com/amoldhamale1105/EventLoop
[grpc-callback-tutorial]: https://grpc.io/docs/languages/cpp/callback/
[grpc-hold-pr]: https://github.com/grpc/grpc/pull/18072
