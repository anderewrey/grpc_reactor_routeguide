# Reactor implementation of gRPC clients

## Design overview

A gRPC thread invokes the reactor callbacks and the application thread is notified to process the response event.
Each reactor arms its next read from inside the reaction that just completed, so a read is only ever issued while
gRPC's own state is provably alive and a concurrent `OnDone()` cannot destroy it mid-call. The application thread
issues writes, which is the one direction that reaches gRPC from outside a reaction, and the residual race that
leaves is described under [Stream completion tracking](#stream-completion-tracking). The elimination of locks on
application state comes from the single-threaded event dispatch described below.

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

- **Method Request encapsulation**: `ActiveUnaryReactor`, `ActiveReadReactor`, `ActiveWriteReactor`, and
  `ActiveBidiReactor` classes
- **Scheduler integration**: Callbacks trigger `EventLoop::TriggerEvent()`
- **Message ownership transfer**: every message leaves its reactor as an owned object, so no response outlives a
  callback inside the reactor and nothing has to be guarded against concurrent access
- **Future-like access**: `Status()` for deferred status retrieval

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
  const std::unique_ptr<routeguide::Feature> response{static_cast<routeguide::Feature*>(event->getData())};
  app_state.UpdateFeatureCache(*response);      // Business logic
  ui_controller.NotifyFeatureLoaded(*response); // State changes
  metrics.RecordFeatureQuery(*response);        // Application concerns
});
```

This separation is intentional: the library handles the concurrency complexity, applications handle the domain logic.

### Pattern components

This implementation follows the Active Object pattern, combining it with the Reactor pattern for event-driven
processing.

**Components mapping:**

| Active Object Component | This Implementation                        | Notes                                        |
|-------------------------|--------------------------------------------|----------------------------------------------|
| Proxy                   | `GetFeature()`, `ListFeatures()`,          | Creates Method Requests, returns immediately |
|                         | `RecordRoute()`, `RouteChat()` methods     |                                              |
| Method Request          | The four `Active*Reactor` classes          | Encapsulates RPC state                       |
| Activation Queue        | EventLoop internal queue                   | Holds pending notifications, and the owned   |
|                         |                                            | messages the reactors push                   |
| Scheduler               | `EventLoop::Run()`                         | Dispatches events to handlers                |
| Servant                 | `EventLoop::RegisterEvent()` handlers      | Application-provided business logic          |
| Future                  | `Status()`                                 | Deferred status access                       |
| Guards                  | The reaction boundary                      | No message is shared across threads, so none |
|                         |                                            | needs guarding                               |

**Library vs application responsibilities:**

| Aspect  | Definition                                         | This Implementation                                   |
|---------|----------------------------------------------------|-------------------------------------------------------|
| Guards  | Method Requests have `guard()` for synchronization | The reaction boundary, see the Guards component       |
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
| `StartRead(&response_)` | Begins async read operation      | Specialized constructor, then `OnReadDone()`      |
| `StartWrite()`          | Begins async write operation     | Inside `SendRequest()`                            |
| `StartWriteLast()`      | Write + implied close, one op    | Inside `SendLastRequest()`                        |
| `StartWritesDone()`     | Signals end of client writes     | Inside `CloseRequestStream()`                     |

### gRPC callbacks (protected overrides)

These callbacks are invoked by gRPC and handled internally by the reactor classes:

| gRPC Callback               | When it fires                        | Invokes user callback          |
|-----------------------------|--------------------------------------|--------------------------------|
| `OnReadDone(bool ok)`       | Read operation completed             | `cbs_.read_ok`/`cbs_.read_nok` |
| `OnWriteDone(bool ok)`      | Write operation completed            | `cbs_.write_done`              |
| `OnWritesDoneDone(bool ok)` | Explicit StartWritesDone() completed | none (internal only)           |
| `OnDone(Status)`            | RPC terminated                       | `cbs_.done`                    |

`ActiveReadReactor` and `ActiveBidiReactor` name these two slots identically, so a reader of either finds the
same `read_ok`/`read_nok` pair.

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
  after a call already checked it. Closing that race fully would need a hold covering the write flow, taken
  before `StartCall()` and released once when that flow conclusively ends, which is the `UseMultipleHolds()`
  sketch both classes carry commented out.

### Application-facing API

These methods are exposed to application code with naming that reflects application-level semantics:

| Method                 | Purpose                                      | Notes                                        |
|------------------------|----------------------------------------------|----------------------------------------------|
| `SendRequest()`        | Send a request message on the stream         | Takes ownership; caller gives up the request |
| `SendLastRequest()`    | Send the final request and close, atomically | For a known-last message                     |
| `CloseRequestStream()` | Signal end of client requests                | Returns false if rejected (see below)        |
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
| **Unary**         | (constructor)    | N/A                    | done callback,   | `TryCancel()`  | `Status()` |
|                   |                  |                        | owned response   |                |            |
| **Server-Stream** | (constructor)    | N/A                    | ok callback,     | `TryCancel()`  | `Status()` |
|                   |                  |                        | owned message    |                |            |
| **Client-Stream** | `SendRequest()`  | `CloseRequestStream()` | done callback,   | `TryCancel()`  | `Status()` |
|                   |                  |                        | owned response   |                |            |
| **Bidi**          | `SendRequest()`  | `CloseRequestStream()` | read_ok callback,| `TryCancel()`  | `Status()` |
|                   |                  |                        | owned message    |                |            |

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
  |  read_ok callback, owned message  |
  |<------------------SendResponse()--|
  |  read_ok callback, owned message  |
  |<-----------CloseResponseStream()--|
  |  read_nok callback                |
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
| Proxy              | Client methods creating reactors               | `GetFeature()`, `ListFeatures()`,              |
|                    |                                                | `RecordRoute()`, `RouteChat()`                 |
| Scheduler          | Event loop dispatching to app thread           | `EventLoop::Run()`, `TriggerEvent()`           |
| Activation Queue   | Event loop queue of notifications and messages | EventLoop internal queue                       |
| Method Request     | Reactor instances encapsulating RPC state      | The four `Active*Reactor` classes              |
| Servant            | Application business logic handlers            | `RegisterEvent()` handlers                     |
| Future             | Reactor handle for retrieving results          | `Status()`                                     |
| Guards             | Prevents concurrent access during processing   | The reaction boundary, no gRPC hold            |

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

The queue carries the message itself rather than a notification that one is ready. The resulting obligations are
listed under [Message ownership](#message-ownership), and the trade-offs they come from are in
[architecture.md](/docs/architecture.md).

### Message ownership

Every reactor hands its messages to a callback as owned objects, and the callback typically releases them into
the event queue. That makes the application responsible for each message. Four obligations follow, each from a
property of the [EventLoop library][eventloop-lib]:

| Obligation                              | Why                                      | Cost                 |
|-----------------------------------------|------------------------------------------|----------------------|
| Reclaim the pointer into a `unique_ptr` | The queue carries a bare `void*`         | Leak                 |
| Reclaim it in exactly one handler       | Every callback under one name is invoked | Double free          |
| Reclaim before anything can throw       | Nothing else owns it meanwhile           | Leak                 |
| Drain the queue before `Halt()`         | `Halt()` discards, it does not drain     | Queued messages leak |

The event data is one pointer, so an application running several concurrent RPCs of the same method cannot tell
which reactor produced a message. It must use a distinct event name per reactor instance, or pass the reactor
pointer the callback received alongside the message.

### Method Request component

The Method Request component encapsulates an RPC invocation with all necessary state: `ClientContext`, request message,
response message, and callbacks. The four reactor instances implement this component.

### Servant component

The Servant component contains application-provided business logic. In this client-side implementation, the Servant is
implemented via `EventLoop::RegisterEvent()` handlers that process RPC results on the application thread.

The response handling provides two execution strategies:

1. **Immediate processing**: Early callbacks (`OnDoneCallback`, `OnReadDoneOkCallback`) execute on gRPC threads for
   quick decisions or lightweight processing. The decision available there is whether to hand the owned message on
   to the application thread or to drop it, which costs nothing more than dropping the `std::unique_ptr`
2. **Deferred processing**: Handlers registered via `EventLoop::RegisterEvent()` execute on the application thread for
   response processing after Scheduler dispatch

The demo application uses simple logging as placeholder Servant logic. Production applications implement actual
business logic (state updates, UI notifications, domain processing) in these handlers.

### Future component

The Future component provides asynchronous access to RPC results. The reactor instance acts as the Future,
exposing `Status()` to check operation success or failure. No reactor exposes a response accessor: each message
leaves through a callback as an owned object, so there is nothing left in the reactor to retrieve later.

### Guards component

The Guards component keeps `OnDone()` from concluding an RPC, and destroying its underlying gRPC-bound state,
while an operation on that state is still being issued. gRPC provides `AddHold()`/`RemoveHold()` for this, and the
library calls neither: the read direction is structured so the window a hold would protect never opens, and the
write direction carries a documented residual race instead.

#### Why the read side needs no hold

The first `StartRead()` is issued by the specialized constructor, before `StartCall()`, so the RPC has not started
yet and no `OnDone()` can be in flight. Every later one is issued as the last statement of `OnReadDone(true)`,
from inside the reaction. A reaction in progress is itself proof that the gRPC-bound state still exists, which is
exactly the guarantee a hold buys, so the read path obtains it structurally rather than by asking for it.

Re-arming after the callback also bounds the reaction to one at a time, since a read cannot complete before it is
started. Messages therefore reach the event loop in network order.

`Status()` and `TryCancel()` are safe from any thread at any time, because neither touches gRPC's internal
callback object: `Status()` reads a member of the reactor, and `TryCancel()` goes through `ClientContext`.

#### Where the write side is exposed

`SendRequest()`, `SendLastRequest()`, and `CloseRequestStream()` run on the application thread and reach gRPC from
outside any reaction, which is precisely the case gRPC's [hold mechanism][grpc-hold-pr] exists for. They guard
themselves with the `stream_no_more_` flag described under
[Stream completion tracking](#stream-completion-tracking), which narrows the window without closing it.

Closing it needs one hold covering the whole write flow, acquired before `StartCall()` and released exactly once
when that flow conclusively ends. Both write-capable reactors carry that design as the commented-out
`UseMultipleHolds()` sketch. One hold is enough because gRPC's hold count is a single counter for the entire RPC
rather than one per direction, per its documented contract in `grpcpp/support/client_callback.h`.

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
using OnDoneCallback =
    std::function<void(grpc::ClientUnaryReactor* reactor, const grpc::Status&, std::unique_ptr<ResponseT>)>;
````

- the pointer to the active reactor instance (i.e. `this`)
- a reference to the `grpc::Status` content
- the response, whose ownership transfers to the callback

The response is never null. A failed RPC yields an empty message, because gRPC writes no response, so the status is
what tells the callback whether the content is meaningful. See [Message ownership](#message-ownership) for the
obligations that come with handing the response on to an event queue.

For the sake of the gRPC processing, it is strongly discouraged to process the response during that callback event.
The callback exists to hand the response to the application thread, or to discard it by dropping the pointer.

#### Class functions

Two public functions can be called by the application side:

- `const grpc::Status& Status()`
- `void TryCancel()`

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
- 3.4 : the `cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void GetFeature(routeguide::Point point) {
  using routeguide::GetFeature::ClientReactor;
  using routeguide::GetFeature::Callbacks;
  using routeguide::GetFeature::ResponseT;
  using routeguide::GetFeature::RpcKey;
  Callbacks cbs;
  cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
    // Signal OnDoneCallback from gRPC thread, handing the response ownership to the queue
    EventLoop::TriggerEvent(kGetFeatureOnDone, response.release());
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

The handler reclaims the response the gRPC-thread callback released into the queue, and reads `Status()` from the
reactor it already holds. The event data is the response, so the reactor pointer no longer rides with it.

From the PlantUML sequence diagram, the corresponding points are from 3.5 to 3.7.

````cpp
EventLoop::RegisterEvent(kGetFeatureOnDone, [&reactor_ = reactor_map_[GetFeature::RpcKey]](const Event* event) {
  const std::unique_ptr<routeguide::Feature> response{static_cast<routeguide::Feature*>(event->getData())};
  if (reactor_->Status().ok()) {
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

box "Application side" #powderblue
boundary    app      as "Application\nside"
queue       equeue   as "EventLoop\nQueue"
end box
control     reactor  as "ClientUnaryReactor"
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
    reactor -[#darkblue]> reactor : <i>internal read target ready
    reactor -[#darkblue]\ grpc : async RPC service call\nstub.async()->RpcMethod()
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
    &grpc -[#darkgreen]> reactor : <i>writes response
    activate reactor #gold
    deactivate grpc
    reactor <[#darkgreen]- grpc : OnDone
    deactivate grpc
group #lightgreen (grpc-thread callback) ondone
    {start3} equeue /[#darkgreen]- reactor : TriggerEvent : OnDone\n+ owned response
    activate equeue #blue
    deactivate reactor
end
    ...
    {end3} equeue -[#darkblue]> app : ProceedEvent: OnDone\n+ owned response
    {start3} <-> {end3} : Eventloop
    deactivate equeue
group #lightblue (app-thread callback) ondone
    app -[#darkblue]> app : <i>update application with response
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

#### Class functions

Two public functions can be called by the application side:

- `const grpc::Status& Status()`
- `void TryCancel()`

The reactor exposes no response accessor. Its internal `response_` read target keeps a stable address across
every read, and each message is swapped out of it by pointer, without a deep-copy, before the next read is armed.

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
- 2.5 : the `cbs.read_ok = [](auto*, std::unique_ptr<ResponseT> response) {...}` lines
- 4.3 : the `cbs.read_nok = [](auto* reactor) {...}` lines
- 4.6 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void ListFeatures(routeguide::Rectangle rect) {
  using routeguide::ListFeatures::ClientReactor;
  using routeguide::ListFeatures::Callbacks;
  using routeguide::ListFeatures::RpcKey;
  Callbacks cbs;
  cbs.read_ok = [](auto*, std::unique_ptr<routeguide::Feature> response) {
    // Signal OnReadDoneOkCallback from gRPC thread, handing the message ownership to the queue
    EventLoop::TriggerEvent(kListFeaturesOnReadDoneOk, response.release());
  };
  cbs.read_nok = [](auto* reactor) {
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

Only the asynchronous method is provided.

### ActiveWriteReactor class

Inherit from `grpc::ClientWriteReactor`, this class implements the Method Request component of the
[Active Object pattern][active-object-pattern] and uses the [Reactor pattern][reactor-pattern] for event handling.
Its threading properties match the two reactors above: the constructor runs on the caller thread, the gRPC
callbacks run on a thread from the gRPC pool, and the callback slots exist to hand the work to the application's
event loop rather than to process it in place.

The write direction is what distinguishes it. The application drives that direction, so `SendRequest()` issues a
gRPC write from the application thread, outside any reaction. gRPC allows one write in flight at a time, which
makes the write flow a turn-by-turn exchange: send one request, wait for `OnWriteDone`, send the next.

A client-streaming RPC receives exactly one response, and gRPC writes it as part of completing the call rather
than through a read operation. There is therefore no `OnReadDone` here, and the terminal response is delivered
with the status through `OnDoneCallback`.

The two callbacks are `OnWriteDoneCallback` and `OnDoneCallback`.

````cpp
using OnWriteDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>* reactor, bool ok)>;
````

When `ClientWriteReactor::OnWriteDone` is handled in the active reactor, the `OnWriteDoneCallback` callback
function is called with the following arguments:

- the pointer to the active reactor instance (i.e. `this`)
- the `ok` flag, false when the write failed and no further operation will succeed

````cpp
using OnDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>* reactor,
                                          const grpc::Status&,
                                          std::unique_ptr<ResponseT>)>;
````

When `ClientWriteReactor::OnDone` is handled in the active reactor, the `OnDoneCallback` callback function is
called with the following arguments:

- the pointer to the active reactor instance (i.e. `this`)
- a reference to the `grpc::Status` content
- the terminal response, whose ownership transfers to the callback

The response is never null, on the same basis as the unary case: a failed RPC yields an empty message, so the
status is what tells the callback whether the content is meaningful. See
[Message ownership](#message-ownership) for the obligations that come with handing it on to an event queue.

#### Class functions

Five public functions can be called by the application side:

- `bool SendRequest(RequestT&&)`
- `bool SendLastRequest(RequestT&&)`
- `bool CloseRequestStream()`
- `const grpc::Status& Status()`
- `void TryCancel()`

`SendRequest()` takes ownership of the request: the caller passes a temporary or an explicit `std::move()`, and
must not keep using the object afterwards. It returns false without side effects when the reactor cannot accept
the write, which leaves the caller free to retry the same object after the next `OnWriteDone`.

`SendLastRequest()` sends the final request and closes the request stream in one operation, which avoids the gap a
separate `SendRequest()` then `CloseRequestStream()` pair leaves open. It still produces an `OnWriteDone` event,
so a handler that pumps the next request must check whether any request remains rather than sending
unconditionally.

`CloseRequestStream()` signals the end of the request stream on its own, for the case where the last request is
not known in advance.

`Status()` and `TryCancel()` behave as described for the unary reactor.

### Code snippet

#### Instantiation of the ActiveWriteReactor class

The following snippet instances an `ActiveWriteReactor` dedicated to the `RecordRoute` RPC of the `routeguide`
API. The `done` callback releases the terminal response into the queue, while `write_done` passes the reactor
pointer, because there is no message to carry.

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.6 : the `cbs.write_done = [](auto* reactor, bool) {...}` lines
- 4.4 : the `cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void RecordRoute(std::vector<routeguide::Point> points) {
  using routeguide::RecordRoute::Callbacks;
  using routeguide::RecordRoute::ClientReactor;
  using routeguide::RecordRoute::ResponseT;
  using routeguide::RecordRoute::RpcKey;
  record_route_pending_ = std::move(points);
  Callbacks cbs;
  cbs.write_done = [](auto* reactor, bool) {
    EventLoop::TriggerEvent(kRecordRouteOnWriteDone, reactor);  // Signal OnWriteDoneCallback from gRPC thread
  };
  cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
    // Signal OnDoneCallback from gRPC thread, handing the response ownership to the queue
    EventLoop::TriggerEvent(kRecordRouteOnDone, response.release());
  };
  reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), std::move(cbs));
  SendNextRecordRoutePoint();  // Kick the write flow with the first request
}
````

#### Pumping the request stream

The write flow advances one request per `OnWriteDone`. The pump sends the final request through
`SendLastRequest()`, so no separate close is needed.

From the PlantUML sequence diagram, the corresponding points are 2.1 and 2.8.

````cpp
void SendNextRecordRoutePoint() {
  auto* reactor = static_cast<RecordRoute::ClientReactor*>(reactor_map_[RecordRoute::RpcKey].get());
  auto point = std::move(record_route_pending_.front());
  record_route_pending_.erase(record_route_pending_.begin());
  if (record_route_pending_.empty()) {
    reactor->SendLastRequest(std::move(point));  // Last request also closes the stream
  } else {
    reactor->SendRequest(std::move(point));
  }
}

EventLoop::RegisterEvent(kRecordRouteOnWriteDone, [this](const Event*) {
  // The request sent via SendLastRequest() also triggers OnWriteDone, so check what remains
  if (!record_route_pending_.empty()) {
    SendNextRecordRoutePoint();
  }
});
````

#### OnDoneCallback

> [!IMPORTANT]
> The last action to do when handling that event is to delete the reactor instance: that instance can't be
> reused by gRPC and a new instance must be allocated for a new usage.

The handler reclaims the terminal response the gRPC-thread callback released into the queue, and reads `Status()`
from the reactor it already holds.

From the PlantUML sequence diagram, the corresponding points are from 4.5 to 4.7.

````cpp
EventLoop::RegisterEvent(kRecordRouteOnDone, [&reactor_ = reactor_map_[RecordRoute::RpcKey]](const Event* event) {
  const std::unique_ptr<routeguide::RouteSummary> response{
      static_cast<routeguide::RouteSummary*>(event->getData())};
  if (reactor_->Status().ok()) {
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

title gRPC Client reactor for client-side stream RPC

box "Application side" #powderblue
boundary    app      as "Application\nside"
queue       equeue   as "EventLoop\nQueue"
end box
control     reactor  as "ClientWriteReactor"
entity      grpc     as "gRPC\nClientCallbackWriter"

legend top center
  <font color=blue><b>blue</b>: main application thread
  <font color=green><b>green</b>: gRPC thread pool
endlegend

activate app #lightblue

autonumber 1.1
== 1. Stream establishment ==
    app -[#darkblue]> reactor : Create Reactor
    activate reactor
    reactor -[#darkblue]> reactor : <i>internal response target ready
    reactor -[#darkblue]\ grpc : async RPC service call\nstub.async()->RpcMethod(response_.get())
    activate grpc #lightgreen
    reactor -[#darkblue]\ grpc : StartCall()
    activate grpc #green

...
autonumber 2.1
== 2. Request streaming ==
loop one request at a time, the last one via SendLastRequest()
    app -[#darkblue]> reactor : SendRequest()
    reactor -[#darkblue]> reactor : <i>moves request into\nthe write buffer
    reactor -[#darkblue]\ grpc : StartWrite()
    & grpc --\? : send Request\nto server
group #lightgreen (grpc-thread callback) onwritedone
    reactor <[#darkgreen]- grpc : OnWriteDone : true
    activate reactor #gold
    {start2} equeue /[#darkgreen]- reactor : TriggerEvent : OnWriteDone
    activate equeue #blue
    deactivate reactor
end
    {end2} equeue -[#darkblue]> app : ProceedEvent: OnWriteDone
    {start2} <-> {end2} : Eventloop
    deactivate equeue
group #lightblue (app-thread callback) onwritedone
    app -[#darkblue]> app : <i>sends the next request,\nif any remains
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
== 4. RPC completion ==
    grpc <--? : receive Response\nand RPC termination
    &grpc -[#darkgreen]> reactor : <i>writes terminal response
    activate reactor #gold
    deactivate grpc
    reactor <[#darkgreen]- grpc : OnDone
    deactivate grpc
group #lightgreen (grpc-thread callback) ondone
    {start4} equeue /[#darkgreen]- reactor : TriggerEvent : OnDone\n+ owned response
    activate equeue #blue
    deactivate reactor
end
    ...
    {end4} equeue -[#darkblue]> app : ProceedEvent: OnDone\n+ owned response
    {start4} <-> {end4} : Eventloop
    deactivate equeue
group #lightblue (app-thread callback) ondone
    app -[#darkblue]> app : <i>update application with response
    activate app #blue
    deactivate app
    app -[#darkblue]> reactor : Destroy Reactor
    destroy reactor
end
```

## Bidirectional streaming RPC client

gRPC API keywords: ClientBidiReactor, ClientCallbackReaderWriter

Only the asynchronous method is provided.

### ActiveBidiReactor class

Inherit from `grpc::ClientBidiReactor`, this class implements the Method Request component of the
[Active Object pattern][active-object-pattern] and uses the [Reactor pattern][reactor-pattern] for event handling.
It combines the two directions already described: its read side behaves as `ActiveReadReactor`'s, and its write
side as `ActiveWriteReactor`'s.

The two directions are independent. The application drives the write side one request at a time, gRPC drives the
read side, and neither waits for the other. A bidirectional RPC carries no terminal response, so `OnDoneCallback`
delivers only the status, unlike the client-streaming case.

The four callbacks are `OnReadDoneOkCallback`, `OnReadDoneNOkCallback`, `OnWriteDoneCallback`, and
`OnDoneCallback`. The read pair and the write callback carry the same signatures and the same meaning as in the
reactors above:

````cpp
using OnReadDoneOkCallback =
    std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, std::unique_ptr<ResponseT>)>;
using OnReadDoneNOkCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor)>;
using OnWriteDoneCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, bool ok)>;
using OnDoneCallback =
    std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, const grpc::Status&)>;
````

Each received message is swapped out of the internal read target and handed to `OnReadDoneOkCallback` as an owned
object, and the next read is armed as the reaction's last statement. Processing the message inside that callback
stays discouraged, because it runs on a gRPC thread: the callback exists to hand the message to the application
thread, or to discard it by dropping the `std::unique_ptr`.

`OnReadDoneNOkCallback` reports the end of the response stream. Unlike the server-streaming case, it also means
the RPC itself is ending, because a server closes a bidirectional response stream only by finishing the call.

#### Class functions

Five public functions can be called by the application side:

- `bool SendRequest(RequestT&&)`
- `bool SendLastRequest(RequestT&&)`
- `bool CloseRequestStream()`
- `const grpc::Status& Status()`
- `void TryCancel()`

They behave exactly as described for `ActiveWriteReactor`. Like the three other reactors, `ActiveBidiReactor`
exposes no response accessor: its `response_` read target keeps a stable address across every read, and each
message is swapped out of it by pointer, without a deep-copy, before the next read is armed.

### Code snippet

#### Instantiation of the ActiveBidiReactor class

The following snippet instances an `ActiveBidiReactor` dedicated to the `RouteChat` RPC of the `routeguide` API.
Only `read_ok` carries a message; the three other callbacks pass the reactor pointer.

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.6 : the `cbs.write_done = [](auto* reactor, bool) {...}` lines
- 3.5 : the `cbs.read_ok = [](auto*, std::unique_ptr<ResponseT> response) {...}` lines
- 5.4 : the `cbs.read_nok = [](auto* reactor) {...}` lines
- 5.7 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void RouteChat(std::vector<routeguide::RouteNote> notes) {
  using routeguide::RouteChat::Callbacks;
  using routeguide::RouteChat::ClientReactor;
  using routeguide::RouteChat::ResponseT;
  using routeguide::RouteChat::RpcKey;
  route_chat_pending_ = std::move(notes);
  Callbacks cbs;
  cbs.read_ok = [](auto*, std::unique_ptr<ResponseT> response) {
    // Signal OnReadDoneOkCallback from gRPC thread, handing the message ownership to the queue
    EventLoop::TriggerEvent(kRouteChatOnReadDoneOk, response.release());
  };
  cbs.read_nok = [](auto* reactor) {
    EventLoop::TriggerEvent(kRouteChatOnReadDoneNOk, reactor);  // Signal OnReadDoneNOkCallback from gRPC thread
  };
  cbs.write_done = [](auto* reactor, bool) {
    EventLoop::TriggerEvent(kRouteChatOnWriteDone, reactor);  // Signal OnWriteDoneCallback from gRPC thread
  };
  cbs.done = [](auto* reactor, const grpc::Status&) {
    EventLoop::TriggerEvent(kRouteChatOnDone, reactor);  // Signal OnDoneCallback from gRPC thread
  };
  reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), std::move(cbs));
  SendNextRouteChatNote();  // Kick the write flow with the first request
}
````

#### OnReadDoneOkCallback

The handler reclaims the message the gRPC-thread callback released into the queue. It needs no access to the
reactor: there is no response left to extract and no read to restart, because the reactor already armed its next
read before this handler ran.

The reclaim must precede any statement that can throw or return early, and exactly one handler may reclaim it.
Both obligations are the ones listed under [Message ownership](#message-ownership).

From the PlantUML sequence diagram, the corresponding points are 3.8 and 3.9.

````cpp
EventLoop::RegisterEvent(kRouteChatOnReadDoneOk, [](const Event* event) {
  const std::unique_ptr<routeguide::RouteNote> response{static_cast<routeguide::RouteNote*>(event->getData())};
  /**  proceeding of the content of response **/
});
````

#### OnWriteDoneCallback

The write flow advances one request per event, exactly as in the client-streaming case, and the final note is
sent through `SendLastRequest()`. The server may keep sending responses after that close.

From the PlantUML sequence diagram, the corresponding points are 2.1 and 2.8.

````cpp
EventLoop::RegisterEvent(kRouteChatOnWriteDone, [this](const Event*) {
  if (!route_chat_pending_.empty()) {
    SendNextRouteChatNote();
  }
});
````

#### OnDoneCallback

> [!IMPORTANT]
> The last action to do when handling that event is to delete the reactor instance: that instance can't be
> reused by gRPC and a new instance must be allocated for a new usage.

From the PlantUML sequence diagram, the corresponding points are 5.11 and 5.12.

````cpp
EventLoop::RegisterEvent(kRouteChatOnDone, [&reactor_ = reactor_map_[RouteChat::RpcKey]](const Event*) {
  const auto status = reactor_->Status();
  /**  proceeding of the status **/
  reactor_.reset();
});
````

### PlantUML sequence flow

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for bidirectional stream RPC

box "Application side" #powderblue
boundary    app      as "Application\nside"
queue       equeue   as "EventLoop\nQueue"
end box
control     reactor  as "ClientBidiReactor"
entity      grpc     as "gRPC\nClientCallbackReaderWriter"

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

...
note across
  Sections 2 and 3 are independent. The write side is driven by the application
  thread, the read side by gRPC, and neither waits for the other.
end note

autonumber 2.1
== 2. Request streaming (write side) ==
loop one request at a time, the last one via SendLastRequest()
    app -[#darkblue]> reactor : SendRequest()
    reactor -[#darkblue]> reactor : <i>moves request into\nthe write buffer
    reactor -[#darkblue]\ grpc : StartWrite()
    & grpc --\? : send Request\nto server
group #lightgreen (grpc-thread callback) onwritedone
    reactor <[#darkgreen]- grpc : OnWriteDone : true
    activate reactor #gold
    {start2} equeue /[#darkgreen]- reactor : TriggerEvent : OnWriteDone
    activate equeue #blue
    deactivate reactor
end
    {end2} equeue -[#darkblue]> app : ProceedEvent: OnWriteDone
    {start2} <-> {end2} : Eventloop
    deactivate equeue
group #lightblue (app-thread callback) onwritedone
    app -[#darkblue]> app : <i>sends the next request,\nif any remains
    activate app #blue
    deactivate app
end
end

...
autonumber 3.1
== 3. Response streaming (read side) ==
loop
    grpc <--? : receive Response\nfrom server
    &grpc -[#darkgreen]> reactor : <i>writes response
    activate reactor #gold
group #lightgreen (grpc-thread callback) onreaddoneok
    reactor <[#darkgreen]- grpc : OnReadDone : true
    reactor -[#darkgreen]> reactor : <i>extracts response\ninto owned message
    {start3} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ owned message
    activate equeue #blue
    deactivate reactor
    reactor -[#darkgreen]\ grpc : StartRead()
end
    grpc -> grpc : reading continues\n(no wait, no hold)
group #lightblue (app-thread callback) onreaddoneok
    {end3} equeue -[#darkblue]> app : ProceedEvent: OnReadDoneOk\n+ owned message
    {start3} <-> {end3} : Eventloop
    deactivate equeue
    app -[#darkblue]> app : <i>processes owned message
    activate app #blue
    deactivate app
end
end

...
autonumber 4.1
== 4. Client-side cancellation ==
    app -[#darkblue]> reactor : TryCancel()
    & reactor -[#darkblue]\ grpc : TryCancel()
    & grpc --\? : RPC termination\nbehest

autonumber 5.1
== 5. Stream termination ==
    grpc <--? : depleted stream
    & reactor <[#darkgreen]- grpc : OnReadDone : false
    deactivate grpc
group #lightgreen (grpc-thread callback) onreaddonenok
    reactor -[#darkgreen]> reactor : <i>no more read or write\n(stream_no_more_)
    {start5} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneNok
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
    app -[#darkblue]> app : <i>update application
    activate app #blue
    deactivate app
end
    {end5} equeue -[#darkblue]> app : ProceedEvent: OnDone
group #lightblue (app-thread callback) ondone
    app -[#darkblue]> app : <i>update application with status
    activate app #blue
    deactivate app
    deactivate equeue
    {start5} <-> {end5} : Eventloop
    app -[#darkblue]> reactor : Destroy Reactor
    destroy reactor
end
```

<!-- Reference links -->
[active-object-pattern]: https://www.modernescpp.com/index.php/active-object/
[reactor-pattern]: https://www.modernescpp.com/index.php/reactor/
[eventloop-lib]: https://github.com/amoldhamale1105/EventLoop
[grpc-callback-tutorial]: https://grpc.io/docs/languages/cpp/callback/
[grpc-hold-pr]: https://github.com/grpc/grpc/pull/18072
