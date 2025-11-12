# Reactor implementation of gRPC clients

## Design Overview

A gRPC thread invokes the reactor callbacks and the application thread is notified to process the response event. To
avoid race conditions, the hold mechanism prevents the RPC from completing while the application thread processes the
response. This approach eliminates the need for locks when accessing application state.

### The Problem

The gRPC reactor callbacks (e.g. `OnDone`, `OnReadDone`) are executed on threads from the gRPC internal thread pool, but
the application cannot control which thread executes the callback. Client applications that use a single main thread or
event loop require response processing to occur on a thread which is managed by the application to maintain thread-safe
access with the software components of the main application.

### Comparison with Direct Callbacks

A direct callback implementation processes responses immediately within the gRPC thread callback and requires
synchronization mechanisms when accessing shared application state. The synchronization requirement propagates throughout
the application codebase, and all mutable state must be protected at every access point. The application loses
single-threaded execution guarantees and must handle concurrent access everywhere. Long processing times block threads
from the gRPC thread pool and reduce available concurrency.

### The Solution: Proxy-Reactor Pattern

This implementation combines two design patterns to address the threading problem. The result is single-threaded
response processing without synchronization primitives. The proxy-reactor pattern defers response processing
to the application thread through event notification. All responses are handled sequentially on a single application
thread.

Reactor Pattern (Event-driven concurrency):
- Events from gRPC (`OnDone`, `OnReadDone`) are received and demultiplexed
- Events are handled asynchronously without blocking
- Event detection is separated from event handling

Proxy Pattern (Indirection layer):
- The proxy acts as an intermediary between gRPC's threading model and the application thread
- RPC state (context, request, response) is held by the proxy instance with lifetime bound to the RPC
- Thread-safe callbacks are provided to bridge gRPC threads to the application event loop
- The proxy instance is destroyed by the application when the RPC is done and cannot be reused

## Unary RPC client

gRPC API keywords: ClientUnaryReactor, ClientCallbackUnary

Both synchronous (blocking) and asynchronous methods are possible; the reactor is an asynchronous variant.

### ProxyUnaryReactor class

Inherit from `grpc::ClientUnaryReactor`, this class follows some points of both [proxy][proxy-pattern] and
[reactor][reactor-pattern] design patterns:

- This class is meant to run on concurrent threads.
- Its constructor is executed by the caller and once the task is done, the caller must destroy it. The
  `ClientUnaryReactor::OnDone` event is about that situation.
- All variables needed for the task execution (e.g. proto messages, gRPC context) belongs to this object instance and
  their lifetime is implicitly bound to the instance life.
- Thread-safety: The scheduling of its execution comes mainly from gRPC events: it implies there's no concurrent gRPC
  threads to take care at the same time.
- Thread-unsafe: the gRPC callback (i.e. `ClientUnaryReactor::OnDone`) is not executed on client thread but on a
  random one (from gRPC thread pool). It is the responsibility of the user to provide a thread-safe callback function.
  To ease that situation, the `ProxyUnaryReactor` class provides a callback slot (i.e. `OnDoneCallback`)
  which is called when `ClientUnaryReactor::OnDone` is signaled by the ClientCallbackUnary. That callback is meant to be
  assigned by the client side to notify its eventloop to allocate a shared time on its scheduler and proceeding with the
  proxy reactor instance.

When `ClientUnaryReactor::OnDone` event is handled in the proxy reactor, the arguments of the `OnDoneCallback` callback
function are:

````cpp
using OnDoneCallback = std::function<void(grpc::ClientUnaryReactor* reactor, const grpc::Status&, const ResponseT&)>;
````

- the pointer to the proxy reactor instance (i.e. `this`)
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
avoid a deep-copy of the content of the response. For the `ProxyUnaryReactor`, having the response swapped is acceptable
because the unary RPC is meant to one response only, so the swap is a good technique to speed up the response proceeding
time. The function will return true when the returned response is valid.

`Status()` function simply returns a reference to the `grpc::Status` of the proxy reactor. Initialized as `Status::OK`,
its content is updated once `ClientUnaryReactor::OnDone` event is received. It means calling `Status()` at any other
moments is meaningless.

`TryCancel()` function simply sends a best-effort out-of-band cancel to the RPC. That signal is thread-safe and can be
sent anytime from any thread. The goal of that signal is to provoke (immediately or later) the
`ClientUnaryReactor::OnDone` event.

### Code snippet

On purpose, the following examples are coming from a sandbox code using a 3rdparty eventloop library.

- [route_guide_proxy_callback_client.cpp](/applications/reactor/route_guide_proxy_callback_client.cpp)
- [EventLoop][eventloop-lib]

For the sanity of the reader, some passages are removed and the code logic reduced to its maximum. It is recommended
to read the original code from the route_guide_proxy_callback_client.cpp file.

#### Instantiation of the ProxyUnaryReactor class

The following snippet instances a `ProxyUnaryReactor` dedicated to the `GetFeature` RPC of the `routeguide` API. It
fills a callback structure with the related `OnDoneCallback`. In this example, the callback is bound to the eventloop
notification function as an `kGetFeatureOnDone` event.

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 3.4 : the `cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {...}` lines

````cpp
#include "api_routeguide.h"
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
box "ProxyUnaryReactor" #beige
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

### ProxyReadReactor class

Inherit from `grpc::ClientReadReactor`, this class follows some points of both [proxy][proxy-pattern] and
[reactor][reactor-pattern] design patterns:

- This class is meant to run on concurrent threads.
- Its constructor is executed by the caller and once the task is done, the caller must destroy it. The
  `ClientReadReactor::OnDone` event is about that situation.
- All variables needed for the task execution (e.g. proto messages, gRPC context) belongs to this object instance and
  their lifetime is implicitly bound to the instance life.
- Thread-safety: The scheduling of its execution comes mainly from gRPC events: it implies there's no concurrent gRPC
  threads to take care at the same time.
- Thread-unsafe: the gRPC callback (i.e. `ClientReadReactor::OnDone`) is not executed on client thread but on a
  random one (from gRPC thread pool). It is the responsibility of the user to provide a thread-safe callback function.
  To ease that situation, the `ProxyReadReactor` class provides different callback slots which are called when
  the related event is signaled by the ClientReadReactor. That callback is meant to be assigned by the client side to
  notify its eventloop to allocate a shared time on its scheduler and proceeding with the proxy reactor instance.

The three callbacks are: `OnReadDoneOkCallback`, `OnReadDoneNOkCallback`, and `OnDoneCallback`.

````cpp
using OnDoneCallback = std::function<void(grpc::ClientReadReactor<ResponseT>* reactor, const grpc::Status&)>;
````

When `ClientReadReactor::OnDone` event is handled in the proxy reactor, the `OnDoneCallback` callback function is called
with the following arguments:

- the pointer to the proxy reactor instance (i.e. `this`)
- a reference to the `grpc::Status` content

````cpp
using OnReadDoneNOkCallback = std::function<void(grpc::ClientReadReactor<ResponseT>* reactor)>;
````

When `ClientReadReactor::OnReadDone` event with a negative OK is handled in the proxy reactor, the
`OnReadDoneNOkCallback` callback function is called with the following argument:

- the pointer to the proxy reactor instance (i.e. `this`)

````cpp
using OnReadDoneOkCallback = std::function<bool(grpc::ClientReadReactor<ResponseT>* reactor, const ResponseT&)>;
````

When `ClientReadReactor::OnReadDone` event with a positive OK is handled in the proxy reactor, the
`OnReadDoneOkCallback` callback function is called with the following argument:

- the pointer to the proxy reactor instance (i.e. `this`)
- a reference to the `Response` content

For the sake of the gRPC processing, it is strongly discouraged to process the response during that callback event. It
is provided to ease some early-reaction logics or to select situations where the response handling is worthy or not.
One difference is that callback requires a return value: a boolean flag.

When it returns:

- true, the proxy reactor will hold the RPC and no more concurrent gRPC events are possible. That way, it is
thread-safe to access the reactor from a different processing thread without concurrent events (e.g. RPC termination).
Once the proceeding of the response is done, the RPC must be signaled to start a new read and then the hold can be
removed.
- false, the proxy reactor will immediately start a new response reading without waiting. That case can be useful when
the responses are put on a queue or when it is discarded.

#### Class functions

Three public functions can be called by the application side:

- `bool GetResponse(ResponseT& response)`
- `const grpc::Status& Status()`
- `void TryCancel()`

`GetResponse()` function (badly named, sorry) does two important things: it swaps the underlying data storage of the
response variable and then (if the stream allows it) it triggers immediately a new read on the stream and resume the
RPC. The swap mechanism is important to avoid a deep-copy of the content of the response. For the `ProxyReadReactor`,
the content of that response is not valuable because it overwrites it on each reading, so the swap is a good technique
to speed up the response proceeding time. The function will return true when the returned response is valid. For
thread-safe reading, a hold must be added over the reactor if the response handling is done out of the
`ClientReadReactor::OnReadDone` event.

`Status()` function simply returns a reference to the `grpc::Status` of the proxy reactor. Initialized as `Status::OK`,
its content is updated once `ClientReadReactor::OnDone` event is received. It means calling `Status()` at any other
moments is meaningless.

`TryCancel()` function simply sends a best-effort out-of-band cancel to the RPC. That signal is thread-safe and can be
sent anytime from any thread. The goal of that signal is to provoke (immediately or later) the
`ClientReadReactor::OnDone` event.

### Code snippet

On purpose, the following examples are coming from a sandbox code using a 3rdparty eventloop library.

- [route_guide_proxy_callback_client.cpp](/applications/reactor/route_guide_proxy_callback_client.cpp)
- [EventLoop][eventloop-lib]

For the sanity of the reader, some passages are removed and the code logic reduced to its maximum. It is recommended
to read the original code from the route_guide_proxy_callback_client.cpp file.

#### Instantiation of the ProxyReadReactor class

The following snippet instances a `ProxyReadReactor` dedicated to the `ListFeatures` RPC of the `routeguide` API. It
fills a callback structure with the related bound functions. In this example, the callbacks are bound to the eventloop
notification function as an `kListFeaturesOnReadDoneOk`, `kListFeaturesOnReadDoneNOk`, or `kListFeaturesOnDone` event.

The implementation of the `OnReadDoneOkCallback` is different, because it requires a return value

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.4 : the `cbs.ok = [](auto* reactor, const ResponseT&) -> bool {...}` lines
- 4.3 : the `cbs.nok = [](auto* reactor) {...}` lines
- 4.6 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "api_routeguide.h"
void ListFeatures(routeguide::Rectangle rect) {
  using routeguide::ListFeatures::ClientReactor;
  using routeguide::ListFeatures::Callbacks;
  using routeguide::ListFeatures::RpcKey;
  Callbacks cbs;
  cbs.ok = [](auto* reactor, const routeguide::Feature&) -> bool {
    EventLoop::TriggerEvent(kListFeaturesOnReadDoneOk, reactor);  // Signal OnReadDoneOkCallback from gRPC thread
    return true;  // true: hold the RPC until the application proceeded the response
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
the proxy reactor instance.

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
and given to the eventloop when the `kListFeaturesOnReadDoneOk` is notified. The main goal of that callback is to read
the response from the proxy reactor and starting a new read operation on the stream.

From the PlantUML sequence diagram, the corresponding points are from 2.8 to 2.11.

````cpp
EventLoop::RegisterEvent(kListFeaturesOnReadDoneOk,
    [&reactor_ = reactor_map_[ListFeatures::RpcKey]](const Event* event) {
      routeguide::Feature response;
      bool valid = reactor_->GetResponse(response);  // when called, a new stream reading is automatically triggered
      /**  proceeding of the content of response **/
    }
);
````

### PlantUML sequence flow

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for server-side stream RPC

boundary    app      as "Application\nside"
box "ProxyReadReactor" #beige
collections data     as "Data"
control     reactor  as "ClientReadReactor"
end box
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
    data o-[#darkblue]->> reactor : Response holder
    & reactor -[#darkblue]\ grpc : StartRead()
    reactor -[#darkblue]\ grpc : StartCall()
    activate grpc #green
    & grpc --\? : send Request\nto server

...
autonumber 2.1
== 2. Stream reading ==
loop
    grpc <--? : receive Response\nfrom server
    &grpc -[#darkgreen]> data : <i>writes response
    activate data #yellow
    reactor <[#darkgreen]- grpc : OnReadDone : true
alt Turn-by-turn | onreaddoneok callback returns true
group #lightgreen (grpc-thread callback) onreaddoneok
    {start1} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk
    &reactor [#darkgreen]-\ grpc : AddHold()
end
    deactivate grpc
    &grpc -> grpc : holding RPC
    activate grpc
    {end1} app -[#darkblue]> reactor : ProceedEvent: OnReadDoneOk
    {start1} <-> {end1} : Eventloop
group #lightblue (app-thread callback) onreaddoneok
    data o-[#darkblue]> reactor : <i>extracts response
    deactivate data
    data o-[#darkblue]->> reactor : Response holder
    & reactor -[#darkblue]\ grpc : StartRead()
    reactor -[#darkblue]\ grpc : RemoveHold()
    &reactor -[#darkblue]> app : <i>update application with response
    activate app #blue
    deactivate app
end
    deactivate grpc
    &grpc -> grpc : resuming RPC
    activate grpc #green
else continuously | onreaddoneok callback returns false
    activate data #yellow
group #lightgreen (grpc-thread callback) onreaddoneok
    data o-[#darkblue]> reactor : <i>extracts response
    deactivate data
    data o-[#darkgreen]->> reactor : Response holder
    & reactor -[#darkgreen]\ grpc : StartRead()
    {start21} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n + response
end
group #lightblue (app-thread callback) onreaddoneok
    {end21} app -[#darkblue]> app : <i>update application\nwith response
    {start21} <-> {end21} : Eventloop
    activate app #blue
    deactivate app
end
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
    {start3} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneNok
end

    grpc <--? : RPC termination
    & reactor <[#darkgreen]- grpc : OnDone
    deactivate grpc
group #lightgreen (grpc-thread callback) ondone
    app /[#darkgreen]- reactor : TriggerEvent : OnDone
end
...
    app -[#darkblue]> reactor : ProceedEvent: OnReadDoneNok
group #lightblue (app-thread callback) onreaddonenok
    reactor -[#darkblue]> app : <i>update application
    activate app #blue
    deactivate app
end
    {end3} app -[#darkblue]> reactor : ProceedEvent: OnDone
group #lightblue (app-thread callback) ondone
    reactor -[#darkblue]> app : <i>update application with status
    activate app #blue
    deactivate app
    {start3} <-> {end3} : Eventloop
    app -[#darkblue]> reactor : Destroy Reactor
    destroy reactor
end
```

## Client-side streaming RPC client

gRPC API keywords: ClientWriteReactor, ClientCallbackWriter

Upcoming development

## Bidirectional streaming RPC client

gRPC API keywords: ClientBidiReactor, ClientCallbackReaderWriter

Upcoming development

<!-- Reference links -->
[proxy-pattern]: https://refactoring.guru/design-patterns/proxy
[reactor-pattern]: https://www.modernescpp.com/index.php/reactor/
[eventloop-lib]: https://github.com/amoldhamale1105/EventLoop
[grpc-callback-tutorial]: https://grpc.io/docs/languages/cpp/callback/