# Walkthrough: your first reactor

This walkthrough is for a first contact with the library. You have an RPC to call, you know its shape, and you want a
working reactor for it. Each part starts from one shape, gives you the choices that shape asks for with a sensible
first answer, and builds the reactor step by step from a demo scenario you can run and copy.

How to use it:

1. Find your reactor in [Which reactor do you need?](#which-reactor-do-you-need)
2. Read [Before you start](#before-you-start), which every part relies on.
3. Read only your part. Each part stands alone, so you can skip the others.

For every variant side by side, see
[Reactors and their variants](/applications/reactor/reactor_client.md#reactors-and-their-variants) in
reactor_client.md, which is also the reference for each class. For the reasoning behind the design, see
[architecture.md](/docs/architecture.md).

## Which reactor do you need?

Look at your RPC in its `.proto` file: the `stream` keywords decide the reactor.

| Your RPC, in its `.proto` file | Its shape | Your reactor | Read |
|--------------------------------|-----------|--------------|------|
| `rpc M(Request) returns (Response)` | Unary | `ActiveUnaryReactor` | [Part 1][part-1] |
| `rpc M(Request) returns (stream Response)` | Server-streaming | `ActiveReadReactor` | [Part 2][part-2] |
| `rpc M(stream Request) returns (Response)` | Client-streaming | `ActiveWriteReactor` | [Part 3][part-3] |
| `rpc M(stream Request) returns (stream Response)` | Bidirectional | `ActiveBidiReactor` | [Part 4][part-4] |

[part-1]: #part-1-a-unary-call
[part-2]: #part-2-receive-a-stream
[part-3]: #part-3-send-a-stream
[part-4]: #part-4-both-directions

Each reactor takes up to three template parameters, and each part tells you which ones yours takes:

- `Delivery`, for whatever your RPC receives: does the reactor keep each message until you pull it (`kPull`), or push
  it to you as an owned object (`kPush`)?
- `ReadPacing`, for a stream you receive: does the stream wait for your application, never (`kContinuous`), when your
  callback says so (`kPerMessage`, the default), or after every message (`kTurnByTurn`)?
- `Submission`, for a stream you send: does your application push each request (`kApplicationPushes`), or does the
  reactor pull it from your callback (`kReactorPulls`)?

## Before you start

### Build and run the demo

Build the project as described in [developing.md](/docs/developing.md). The commands below use the directory of the
`vcpkg-gcc-debug` preset. Start the server, then the demo client, in two terminals:

```bash
DIR=cmake-build-vcpkg-debug-gcc
./$DIR/applications/callback/route_guide_callback_server
./$DIR/applications/reactor/route_guide_active_reactor_client
```

The demo starts all of its scenarios at once, one per variant it shows, and keeps its event loop running, so stop it
with Ctrl+C once the output settles. Each scenario logs under its own name, and each part below gives the `grep` that picks
it out. The scenario files live in
[applications/reactor/variants/](/applications/reactor/variants/), one per variant, and they are what you copy. Its
[README](/applications/reactor/variants/README.md) lists every scenario with its variant, and the use cases, which
show a variant serving a real-life situation, are in [usecases/](/applications/reactor/usecases/), run by their own
application against the same server:

```bash
./$DIR/applications/reactor/route_guide_reactor_usecases
```

Every log line reads `[time][logger][thread id][level] message`. The thread id is how you tell the two threads apart.

### Two threads

| Thread                                         | What runs there                                              |
|------------------------------------------------|--------------------------------------------------------------|
| A gRPC thread, from gRPC's own pool            | The reactor's events, and your callbacks, which hand them on |
| The application thread, run by the `EventLoop` | Your handlers, where the real work runs, one event at a time |

All of your application's logic runs on the one application thread, so its state needs no locks. A callback runs on
a gRPC thread, which gRPC needs for other RPCs, so it does as little as possible: it queues an event with
`EventLoop::TriggerEvent()`, and returns.

### The five moves every reactor follows

Every scenario is one class that owns one RPC. Here is the smallest one,
[get_feature_pull.h](/applications/reactor/variants/get_feature_pull.h), condensed:

```cpp
class GetFeature {
  using Delivery = routeguide::GetFeature::Delivery;
  using Callbacks = routeguide::GetFeature::Callbacks<Delivery::kPull>;
  using ClientReactor = routeguide::GetFeature::ClientReactor<Delivery::kPull>;

 public:
  explicit GetFeature(routeguide::RouteGuide::Stub& stub);
  void Start(routeguide::Point point);           // creates the reactor, and returns at once

 private:
  void OnDone(const EventLoop::Event* event);    // a handler, on the application thread

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;
  RpcReactor::EventConnection on_done_;          // declared last, so it deregisters first
};
```

Whatever the reactor, your code makes the same five moves:

1. **Register a handler per event**, for as long as your class lives. An `RpcReactor::EventConnection` member does it,
   from [reactor_eventloop.h](/applications/reactor/reactor_eventloop.h):

   ```cpp
   constexpr auto kOnDone{"GetFeaturePullOnDone"};  // one distinct name per scenario and event
   GetFeature::GetFeature(routeguide::RouteGuide::Stub& stub)
       : stub_(stub), on_done_(kOnDone, [this](const EventLoop::Event* e) { OnDone(e); }) {}
   ```

2. **Start the RPC** on the application thread: fill a `Callbacks` struct, then create the reactor. Its constructor
   starts the RPC, so there is nothing else to call. The demo's `CreateClientContext()` returns a fresh
   `grpc::ClientContext`, where a deadline would go.

   ```cpp
   reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), point, std::move(cbs));
   ```

3. **Let each callback only queue an event.** It runs on a gRPC thread, and passes either the reactor pointer or an
   owned message, depending on the `Delivery`. Use the reactor pointer the callback is given, never your `reactor_`
   member: since the constructor starts the RPC, a callback can run before `std::make_unique` has returned and set
   `reactor_`. A handler runs only after your code has returned, so it can use `reactor_` freely.
4. **Do the work in the handlers**, on the application thread.
5. **Destroy the reactor in the `OnDone` handler**, as its last action, after reading `Status()`. gRPC requires the
   reactor to outlive `OnDone()`, and never uses it after that. If you must tear a reactor down before its RPC ends,
   such as at shutdown, call `reactor_->CancelAndWait(timeout)` first, and destroy it only if that returns `true`.
   The demo's scenario destructors do it through `TearDownReactor()`, in
   [support.h](/applications/reactor/demo_support.h).

The parts below only fill in moves 2 to 4 for each reactor.

### Your own service: the adapter

The demo calls `routeguide::GetFeature::ClientReactor`, an adapter from
[reactor_client_routeguide.h](/applications/reactor/reactor_client_routeguide.h). Your service needs the same one per
RPC, which is about 20 lines that bind the generic reactor to your stub's method. Copy the block of the RouteGuide RPC
with your shape, and rename. For a unary method `GetThing` of a service `MyService`:

```cpp
namespace myservice::GetThing {
using RequestT = ThingRequest;  // your request and response messages
using ResponseT = Thing;
using Delivery = RpcReactor::Client::Delivery;

template <Delivery kDelivery>
using Callbacks = RpcReactor::Client::ActiveUnaryCallbacks<ResponseT, kDelivery>;

template <Delivery kDelivery>
class ClientReactor final : public RpcReactor::Client::ActiveUnaryReactor<ResponseT, kDelivery> {
  using Base = RpcReactor::Client::ActiveUnaryReactor<ResponseT, kDelivery>;

 public:
  ClientReactor(MyService::Stub& stub, std::unique_ptr<grpc::ClientContext> context, const RequestT& request,
                Callbacks<kDelivery>&& cbs)
      : Base(std::move(context), std::move(cbs)) {
    stub.async()->GetThing(this->context_.get(), &request, this->response_.get(), this);
    this->StartCall();
  }
};
}  // namespace myservice::GetThing
```

Only two things change from one shape to the next: the base class, and the constructor's body. The base class takes the
same template parameters as the reactor, in the same order, and so do the `Callbacks` and `ClientReactor` templates.

| Shape | Base class |
|-------|------------|
| Unary | `ActiveUnaryReactor<ResponseT, kDelivery>` |
| Server-streaming | `ActiveReadReactor<ResponseT, kDelivery, kPacing>` |
| Client-streaming | `ActiveWriteReactor<RequestT, ResponseT, kDelivery, kSubmission>` |
| Bidirectional | `ActiveBidiReactor<RequestT, ResponseT, kDelivery, kSubmission, kPacing>` |

The constructor body for each shape:

```cpp
// Unary
stub.async()->M(ctx, &request, this->response_.get(), this);
this->StartCall();

// Server-streaming
stub.async()->M(ctx, &request, this);
this->StartRead(&this->response_);
this->StartCall();

// Client-streaming
stub.async()->M(ctx, this->response_.get(), this);
this->StartCall();

// Bidirectional
stub.async()->M(ctx, this);
this->StartRead(&this->response_);
this->StartCall();
```

Here `ctx` is `this->context_.get()`. `this->StartCall()` must come last: it is the reactor's own `StartCall()`, which
takes the holds the writing reactors need, or pulls their first request, before it starts the RPC. A reading reactor
arms its first read just before it.

## Part 1: a unary call

`ActiveUnaryReactor` sends one request and receives one response, which arrives with the end of the RPC. It is the
simplest reactor: one choice, one callback, one handler, and no obligation beyond the five moves.

Demo: `GetFeature`, in [get_feature_pull.cpp](/applications/reactor/variants/get_feature_pull.cpp) and
[get_feature_push.cpp](/applications/reactor/variants/get_feature_push.cpp).

### Step 1: choose the delivery

`Delivery` is the only choice, and it is about one thing: who owns the response while it waits in your event queue.

- `kPull`: the reactor keeps it, the event carries the reactor pointer, and your handler pulls the response out with
  `GetResponse()`. Start here if your handler simply uses the response.
- `kPush`: the reactor gives it away, and the event carries the response itself. Choose it if the response must be
  handed on to another component.

### Step 2: write the done callback

The `done` slot runs on a gRPC thread when the RPC ends. Under `kPull` it is lent the response, and passes the
reactor on:

```cpp
using ResponseT = routeguide::GetFeature::ResponseT;
Callbacks cbs;
cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
  EventLoop::TriggerEvent(kOnDone, reactor);
};
```

Under `kPush` it is given the response, and releases it into the queue:

```cpp
cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
  EventLoop::TriggerEvent(kOnDone, response.release());
};
```

### Step 3: create the reactor

```cpp
reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), point, std::move(cbs));
```

The constructor sends the request and starts the RPC. Your code issues nothing else to gRPC, apart from an optional
`reactor_->TryCancel()`.

### Step 4: handle the end

Under `kPull`, the handler pulls the response out of the reactor, by a swap rather than a copy, then destroys the
reactor:

```cpp
void GetFeature::OnDone(const EventLoop::Event* event) {
  if (const auto status = reactor_->Status(); status.ok()) {
    routeguide::GetFeature::ResponseT response;
    if (reactor_->GetResponse(response)) {
      // use the response
    }
  }
  reactor_.reset();
}
```

Under `kPush`, the handler first takes the response back into a `std::unique_ptr`, so it is freed whatever happens
next. After a failed RPC it is an empty message, never a null pointer, so `Status()` is what tells you whether to use
it:

```cpp
void GetFeature::OnDone(const EventLoop::Event* event) {
  const std::unique_ptr<routeguide::GetFeature::ResponseT> response{
      static_cast<routeguide::GetFeature::ResponseT*>(event->getData())};
  if (reactor_->Status().ok()) {
    // use *response
  }
  reactor_.reset();
}
```

### Step 5: see it run

```bash
./$DIR/applications/reactor/route_guide_active_reactor_client | grep GetFeature
```

The output looks like this, with the response shortened. The feature changes from run to run, since the demo picks a
random point:

```text
[19:22:54.961224][Main][23807][I] -------------- GetFeature kPull --------------
[19:22:54.961248][GetFeature.pull][23807][I]          | reactor[0x555994125820] created
[19:22:54.961363][Main][23807][I] -------------- GetFeature kPush --------------
[19:22:54.961400][GetFeature.push][23807][I]          | reactor[0x555994122d40] created
[19:22:54.962746][GetFeature.pull][23807][I] RESPONSE | routeguide.Feature: name: "517-521 Huntington Drive, ..." ...
[19:22:54.962772][GetFeature.pull][23807][I]          | reactor[0x555994125820] ended
[19:22:54.962928][GetFeature.push][23807][I] RESPONSE | routeguide.Feature: name: "517-521 Huntington Drive, ..." ...
[19:22:54.962989][GetFeature.push][23807][I]          | reactor[0x555994122d40] ended
```

Every line carries thread 23807, the application thread that logs the `Main` banners: these scenarios log nothing on
a gRPC thread. Compare the two files to see everything the delivery changes, which is the `done` slot and the handler:

```bash
diff applications/reactor/variants/get_feature_pull.cpp applications/reactor/variants/get_feature_push.cpp
```

### Your checklist

- [ ] One event name, and one `EventConnection`, for `OnDone`.
- [ ] The `done` callback only calls `TriggerEvent()`.
- [ ] Under `kPush`, the handler takes the response back into a `std::unique_ptr` first, exactly once.
- [ ] The handler checks `Status()`, and destroys the reactor last.

The sequence diagrams of both deliveries are under
[Unary RPC client](/applications/reactor/reactor_client.md#unary-rpc-client), and what `kPush` asks of your event
queue under [Message ownership](/applications/reactor/reactor_client.md#message-ownership).

## Part 2: receive a stream

`ActiveReadReactor` sends one request and receives a stream of messages, one `read_ok` per message, then `read_nok`
when the server has sent everything, then `done`. A stream adds one question to the unary call: once a message is
delivered, who arms the next read?

Demo: `ListFeatures`, in the six `list_features_<delivery>_<pacing>.cpp` files. This part follows
[list_features_pull_per_message.cpp](/applications/reactor/variants/list_features_pull_per_message.cpp).

### Step 1: choose the delivery and the pacing

`Delivery` works as in a unary call, for each message:

- `kPull`: `read_ok` is lent the message, which stays in the reactor's read target. Your handler pulls it out with
  `GetResponse()`, which also arms the next read.
- `kPush`: `read_ok` is given the message as an owned object, moved out of the reactor's read target, so it survives
  every later read.

`ReadPacing` decides whether the stream waits for your application. Start with the default, `kPerMessage`: your
`read_ok` callback returns `true` to keep a message, which holds the stream until your handler releases it, or `false`
to decline it, and the reactor reads on at once. The two other pacings fix that answer for the whole RPC, which
[Step 7](#step-7-fix-the-pacing-once-your-design-has-decided) comes back to.

A `ClientReactor` that names only its delivery uses the default:

```cpp
using Delivery = routeguide::ListFeatures::Delivery;
using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull>;
using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull>;
```

### Step 2: write read_ok

`read_ok` runs on a gRPC thread, for each message. Under `kPull` with `kPerMessage`, it decides on the lent message,
queues the reactor pointer for a message it keeps, and returns its decision. The variant decides by an arbitrary rule,
keeping every second message, so that it shows the mechanics and nothing else. A real application decides from the
message. The [place_search](/applications/reactor/usecases/place_search.cpp) use case keeps the named places only,
which makes it a filter:

```cpp
cbs.read_ok = [](auto* reactor, const ResponseT& response) -> bool {
  if (response.name().empty()) return false;  // declined: the reactor reads on by itself
  EventLoop::TriggerEvent(kOnReadDoneOk, reactor);
  return true;                                // kept: the stream waits for GetResponse()
};
```

A declined message is gone once `read_ok` returns, since the next read overwrites it, so copy out anything you need
from it before returning. Under `kPush` the callback receives a `std::unique_ptr<ResponseT>` instead, and owns every
message, declined ones included.

### Step 3: write read_nok and done

Both only queue their event, with the reactor pointer:

```cpp
cbs.read_nok = [](auto* reactor) { EventLoop::TriggerEvent(kOnReadDoneNOk, reactor); };
cbs.done = [](auto* reactor, const grpc::Status&) { EventLoop::TriggerEvent(kOnDone, reactor); };
```

`done` receives no message: the stream's messages have all gone through `read_ok`. Your `OnDone` handler reads
`Status()` and destroys the reactor, as in a unary call.

### Step 4: create the reactor

```cpp
reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), rect, std::move(cbs));
```

The constructor arms the first read, then starts the RPC.

### Step 5: release each kept message

For each message `read_ok` kept, your handler makes exactly one release call: `GetResponse()` under `kPull`,
`ResumeRead()` under `kPush`. It arms the next read, and releases the hold the reactor took for that message:

```cpp
void ListFeatures::OnReadOk(const EventLoop::Event* event) {
  routeguide::ListFeatures::ResponseT response;
  if (!reactor_->GetResponse(response)) return;
  // use the response
}
```

Two rules come with it:

- **Never skip it.** While a kept message waits, no read is armed, and the reactor holds the RPC open for your release
  call. Without that call the RPC never ends, and its reactor can never be destroyed.
- **Never call it for a declined message.** A release call releases whichever message is held at that moment. For a
  declined message, that can be the next one, whose `read_ok` may still be deciding.

Under `kPull`, both rules hold by construction: only a kept message queues an event, so `OnReadOk` runs once per kept
message. Under `kPush`, where you usually queue every message, record which one `read_ok` held, and call `ResumeRead()`
only from that message's handler, as
[list_features_push_per_message.cpp](/applications/reactor/variants/list_features_push_per_message.cpp) does. The
[offline_map_download](/applications/reactor/usecases/offline_map_download.cpp) use case applies it to a real decision:
the download holds the stream once three places wait for the disk writer.

### Step 6: see it run

```bash
./$DIR/applications/reactor/route_guide_active_reactor_client | grep -F '[ListFeatures.pull.msg]'
```

The output looks like this, shortened to its first messages and its end:

```text
[14:54:21.738720][ListFeatures.pull.msg][20403][I]          | reactor[0x564a1b8ffcd0] created
[14:54:21.853182][ListFeatures.pull.msg][20412][I] skipped  | #1 routeguide.Feature: name: "Patriots Path, ..." ...
[14:54:21.853384][ListFeatures.pull.msg][20412][I] read_ok  | #1 routeguide.Feature
[14:54:21.853504][ListFeatures.pull.msg][20403][I] pulled   | #1 arming next read
[14:54:21.853508][ListFeatures.pull.msg][20403][I] RESPONSE | #1 routeguide.Feature: name: "101 New Jersey 10, ..." ...
[14:54:21.853764][ListFeatures.pull.msg][20413][I] skipped  | #2 routeguide.Feature: name: "U.S. 6, Shohola, ..." ...
[14:54:21.853944][ListFeatures.pull.msg][20413][I] read_ok  | #2 routeguide.Feature
[14:54:21.853962][ListFeatures.pull.msg][20403][I] pulled   | #2 arming next read
[14:54:21.853970][ListFeatures.pull.msg][20403][I] RESPONSE | #2 routeguide.Feature: name: "5 Conners Road, ..." ...
...
[14:54:21.865308][ListFeatures.pull.msg][20403][I]          | ListFeaturesPullMsgOnReadDoneNOk reactor: 0x564a1b8ffcd0
[14:54:21.865310][ListFeatures.pull.msg][20403][I]          | ListFeaturesPullMsgOnDone reactor: 0x564a1b8ffcd0
[14:54:21.865312][ListFeatures.pull.msg][20403][I] totals   | 50 kept, 50 skipped
[14:54:21.865313][ListFeatures.pull.msg][20403][I]          | reactor[0x564a1b8ffcd0] ended
```

Three things are worth checking in your own run:

- Two kinds of thread appear. Thread 20403 is the application thread. The `read_ok` and `skipped` lines carry other
  ids, here 20412 and 20413, because `read_ok` logs them on a gRPC thread, which can change from one message to the
  next.
- `read_ok #k+1` never comes before `pulled #k`: while a kept message waits for your handler, the stream waits too.
- A `skipped` line is never followed by an application-thread line: a declined message never reaches your handler,
  and the stream did not wait for it.

The same filter in `place_search`, run by `route_guide_reactor_usecases`, keeps the 64 named places of the rectangle
and skips its 36 unnamed ones, and logs each skipped one as `skipped  | #j unnamed place`.

### Step 7: fix the pacing once your design has decided

`kPerMessage` decides for each message. The two other pacings take one of its branches for every message, and the
compiler then enforces the decision:

| `ReadPacing` | `read_ok` returns | Your handler calls | Choose it when |
|--------------|-------------------|--------------------|----------------|
| `kContinuous` | Nothing | Nothing: no message is held | Messages are consumed on arrival, and the stream never waits |
| `kPerMessage` | `bool` | `GetResponse()` or `ResumeRead()`, once per kept message | The answer varies per message |
| `kTurnByTurn` | Nothing | `GetResponse()` or `ResumeRead()`, once per message | The stream waits after every message |

Under `kContinuous` with `kPull`, nothing is held, so the lent message is gone once `read_ok` returns: consume it, or
copy it, inside the callback. A call the pacing rules out does not compile. In
`list_features_pull_continuous.cpp`, a `reactor_->GetResponse(...)` call fails the build, since there is never a
message to pull. Compare the files one step at a time:

```bash
cd applications/reactor/variants
diff list_features_pull_per_message.cpp list_features_pull_turn_by_turn.cpp  # every message kept
diff list_features_pull_turn_by_turn.cpp list_features_pull_continuous.cpp   # no message kept
diff list_features_pull_turn_by_turn.cpp list_features_push_turn_by_turn.cpp # the delivery, same pacing
```

Two use cases, in [usecases/](/applications/reactor/usecases/) and named after the situation they serve, show what
each `kContinuous` delivery is for, in the `route_guide_reactor_usecases` application. Each is a clone of its
`kContinuous` variant along the use only:

```bash
diff list_features_pull_continuous.cpp ../usecases/live_vehicle_position.cpp  # only the newest kept
diff list_features_push_continuous.cpp ../usecases/trip_recorder.cpp          # every one kept, backlog seen
```

`live_vehicle_position.cpp` is the use `kContinuous` with `kPull` is best at: a map drawing a vehicle's position from a
live feed of fixes, where only the newest fix matters. `read_ok` copies each fix into a `RpcReactor::LatestSlot`,
replacing the one your handler has not taken yet, and triggers an event only when the slot was empty. Your handler
draws the newest fix, so a slow map skips the stale ones while the feed never waits. In the demo run the map takes
5 ms per redraw, so it draws fix #1, then #33, then #100, and ends:

```text
[12:57:29.440047][LiveVehiclePosition][673][I] read_ok  | #1 routeguide.Feature: name: "Patriots Path, ..."
[12:57:29.440077][LiveVehiclePosition][664][I] RESPONSE | #1 routeguide.Feature: name: "Patriots Path, ..."
[12:57:29.440401][LiveVehiclePosition][673][I] read_ok  | #2 routeguide.Feature: name: "101 New Jersey 10, ..."
...
[12:57:29.446209][LiveVehiclePosition][673][I] read_ok  | #33 routeguide.Feature: name: "650-652 Willi Hill ..."
[12:57:29.446619][LiveVehiclePosition][664][I] RESPONSE | #33 routeguide.Feature: name: "650-652 Willi Hill ..."
...
[12:57:29.453778][LiveVehiclePosition][673][I] read_ok  | #100 routeguide.Feature: name: "3 Hasta Way, ..."
[12:57:29.488403][LiveVehiclePosition][664][I] RESPONSE | #100 routeguide.Feature: name: "3 Hasta Way, ..."
[12:57:29.573374][LiveVehiclePosition][664][I] totals   | 100 fixes, 3 drawn, 97 replaced before drawing, last ...
```

The pattern is described under [Keeping only the newest message][latest-only] in the reference.

`trip_recorder.cpp` is the opposite use, the one `kContinuous` with `kPush` is best at: a vehicle's black box storing
every position report of a trip, where every report matters and the feed must still never wait. `read_ok` queues each
owned report as its own event and the reactor reads on, so slow storage builds a backlog instead of slowing the server.
In the demo run storage takes 1 ms per report, so the RPC ends with most reports still queued, and the trip log
catches up later, in order:

```text
[12:57:29.440093][TripRecorder][674][I] read_ok  | #1 routeguide.Feature, backlog 1
[12:57:29.440423][TripRecorder][673][I] read_ok  | #2 routeguide.Feature, backlog 2
[12:57:29.445186][TripRecorder][664][I] RESPONSE | #1 routeguide.Feature: name: "Patriots Path, ..."
...
[12:57:29.453726][TripRecorder][674][I] read_ok  | #100 routeguide.Feature, backlog 98
[12:57:29.453744][TripRecorder][673][I] done     | RPC ended, 98 reports still queued for the trip log
...
[12:57:29.572048][TripRecorder][664][I] RESPONSE | #100 routeguide.Feature: name: "3 Hasta Way, ..."
[12:57:29.573255][TripRecorder][664][I] totals   | 100 reports read, 100 stored in the trip log, peak backlog 98
```

Nothing bounds that backlog but the length of the stream. When yours must stay bounded, keep `kPush` and choose
`kPerMessage`, as the `offline_map_download.cpp` use case does. The trade-off is described under
[Handling every message without slowing the stream][journal] in the reference.

### Your checklist

- [ ] Under `kPerMessage`, `read_ok` returns `true` only for a message it queued for your handler.
- [ ] Each kept message gets exactly one `GetResponse()` or `ResumeRead()`, and a declined one none.
- [ ] Under `kPull`, a message `read_ok` does not keep is consumed or copied before `read_ok` returns.
- [ ] The `OnDone` handler destroys the reactor last.

The sequence diagrams, which draw `kPerMessage` under both deliveries, are under
[Server-side streaming RPC client](/applications/reactor/reactor_client.md#server-side-streaming-rpc-client). Why the
reactor takes a hold for each message is under
[Hold requirements](/applications/reactor/reactor_client.md#hold-requirements).

## Part 3: send a stream

`ActiveWriteReactor` sends a stream of requests, one write at a time, and receives one response, which arrives with the
end of the RPC, as in a unary call. It takes two choices: who sends each request, and how the final response is
delivered.

Demo: `RecordRoute`, which sends a route as a stream of points and receives a `RouteSummary`, in
[record_route_pull_producer.cpp](/applications/reactor/variants/record_route_pull_producer.cpp) and
[record_route_pull.cpp](/applications/reactor/variants/record_route_pull.cpp).

### Step 1: choose who sends each request

This is the choice that shapes your code, so make it first. `Submission` decides it:

- `kReactorPulls`: the reactor asks your `next_request` callback for each request, and closes the stream when the
  callback has none. Choose it when every next request is available at once: a list you prepared, a generator, a
  file. It is the simpler of the two, since your application makes no write call and owes nothing.
- `kApplicationPushes`: your application calls `SendRequest()` whenever it has a request. Choose it when requests appear
  later, on your own thread, for example as user input or events arrive, since a `next_request` callback cannot answer
  "not yet".

Then choose the `Delivery` of the final response, exactly as for a unary call in
[Part 1](#step-1-choose-the-delivery). The steps below use `kPull`.

```cpp
using Delivery = routeguide::RecordRoute::Delivery;
using Submission = routeguide::RecordRoute::Submission;
using ClientReactor = routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>;
```

`Submission` has no default, so the type always says which one you chose. If you chose `kReactorPulls`, follow Step 2.
If you chose `kApplicationPushes`, skip to Step 3.

### Step 2: if the reactor pulls, write next_request

The reactor calls `next_request` once while its constructor starts the RPC, then once after each successful write.
Return the next request, or `std::nullopt` to close the request stream. The demo keeps its points in a queue that it
fills before creating the reactor:

```cpp
cbs.next_request = [this](auto*) { return NextPoint(); };

std::optional<routeguide::Point> RecordRoute::NextPoint() {
  if (pending_.empty()) return std::nullopt;  // the reactor closes the request stream
  auto point = std::move(pending_.front());
  pending_.erase(pending_.begin());
  return point;
}
```

The callback's contract is short, and strict:

- **The first call runs inside the reactor's constructor**, on the thread that creates the reactor, before
  `std::make_unique` has even returned. So prepare the source first, and do not reach for `reactor_` inside the
  callback.
- **Every later call runs on a gRPC thread**, right after `write_done` for the previous write. Calls never overlap, so
  a queue that only the callback touches after `Start()` needs no lock.
- **It must answer at once.** It runs inside a reaction, so it must not block or wait.

Bind `write_done` and `done` as usual, and create the reactor:

```cpp
cbs.write_done = [](auto* reactor, bool) { EventLoop::TriggerEvent(kOnWriteDone, reactor); };
cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) { EventLoop::TriggerEvent(kOnDone, reactor); };
reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), std::move(cbs));
```

That is all. The reactor takes no hold, so the RPC ends on its own however it ends, and your `OnWriteDone` handler
has nothing to do. Your `OnDone` handler pulls the summary and destroys the reactor, as in
[Part 1](#step-4-handle-the-end). See it run:

```bash
./$DIR/applications/reactor/route_guide_active_reactor_client | grep -F '[RecordRoute.pull.producer]'
```

```text
[19:22:54.961667][RecordRoute.pull.producer][23807][I] next     | #1 latitude: 401012643 longitude: -744035134
[19:22:54.961849][RecordRoute.pull.producer][23807][I]          | reactor[0x555994149190] created
[19:22:54.961893][RecordRoute.pull.producer][23816][I] next     | #2 latitude: 406523420 longitude: -742135517
[19:22:54.962130][RecordRoute.pull.producer][23817][I] next     | #3 latitude: 400273442 longitude: -741220915
[19:22:54.962177][RecordRoute.pull.producer][23817][I] next     | none left, the reactor closes the request stream
[19:22:54.962592][RecordRoute.pull.producer][23807][I]          | RecordRoutePullProducerOnWriteDone reactor: ...
[19:22:54.962596][RecordRoute.pull.producer][23807][I]          | RecordRoutePullProducerOnWriteDone reactor: ...
[19:22:54.962670][RecordRoute.pull.producer][23807][I]          | RecordRoutePullProducerOnWriteDone reactor: ...
[19:22:54.967574][RecordRoute.pull.producer][23807][I] RESPONSE | routeguide.RouteSummary: point_count: 3 ...
[19:22:54.967620][RecordRoute.pull.producer][23807][I]          | reactor[0x555994149190] ended
```

The first `next` line comes from the application thread, 23807, before the `created` line: that is the first pull,
inside the constructor. The others come from gRPC threads, each after the previous write completed.

### Step 3: if your application pushes, pump one write at a time

gRPC allows one write in flight per stream. So your application sends the first request, then the next one from each
`OnWriteDone` handler, and sends the last one through `SendLastRequest()`, which closes the stream in the same
operation. The demo's pump, from [record_route_pull.cpp](/applications/reactor/variants/record_route_pull.cpp):

```cpp
void RecordRoute::SendNextPoint() {
  auto point = std::move(pending_.front());
  pending_.erase(pending_.begin());
  const bool last = pending_.empty();
  if (!(last ? reactor_->SendLastRequest(std::move(point)) : reactor_->SendRequest(std::move(point)))) {
    // refused: no OnWriteDone will come to drive the next write
  }
}

void RecordRoute::OnWriteDone(const EventLoop::Event* event) {
  if (!pending_.empty()) SendNextPoint();  // the last write completes with an OnWriteDone too
}
```

`Start()` creates the reactor, then calls `SendNextPoint()` once. A write call gives up the request, so pass a
temporary or `std::move()` it. It returns `false`, and sends nothing, while a write is in flight, once the write side
has ended, or once the RPC is over, so a call from anywhere but the pump must be ready for `false`.

One obligation comes with pushing:

> The RPC completes only once your application ends its write side: with `SendLastRequest()`,
> `CloseRequestStream()`, or `TryCancel()`, or by a write that fails.

The reactor holds the RPC open while a write may still come from your thread, and a client-streaming RPC has no read
side to report an early end. So if your application may go quiet before its last request, it must still close or
cancel at some point, or its `OnDone` handler never runs. The pump meets the obligation through `SendLastRequest()`.
When the last request is not known in advance, call `CloseRequestStream()` after the last write's `OnWriteDone`. See it
run:

```bash
./$DIR/applications/reactor/route_guide_active_reactor_client | grep -F '[RecordRoute.push]'
```

```text
[19:22:54.961459][RecordRoute.push][23807][I]          | reactor[0x555994145690] created
[19:22:54.962546][RecordRoute.push][23807][I]          | RecordRoutePushOnWriteDone reactor: 0x555994145690
[19:22:54.962787][RecordRoute.push][23807][I]          | RecordRoutePushOnWriteDone reactor: 0x555994145690
[19:22:54.966690][RecordRoute.push][23807][I]          | RecordRoutePushOnWriteDone reactor: 0x555994145690
[19:22:54.967629][RecordRoute.push][23807][I] RESPONSE | routeguide.RouteSummary: point_count: 3 feature_count: 1 ...
[19:22:54.967653][RecordRoute.push][23807][I]          | reactor[0x555994145690] ended
```

This is the `kPush` scenario, whose pump is the same as in `record_route_pull.cpp`. Each `OnWriteDone` line is a
completed write, and each one drives the next.

### Step 4: compare along each choice

```bash
cd applications/reactor/variants
diff record_route_pull.cpp record_route_pull_producer.cpp  # who sends: the pump becomes next_request
diff record_route_pull.cpp record_route_push.cpp           # the delivery of the summary
```

### Your checklist

- [ ] Under `kReactorPulls`, the source is ready before the reactor is created, and `next_request` never blocks.
- [ ] Under `kApplicationPushes`, one write in flight at a time, and a `false` from a write call is handled.
- [ ] Under `kApplicationPushes`, every path ends the write side: last request, close, or cancel.
- [ ] The `OnDone` handler takes the summary, as in a unary call, and destroys the reactor last.

The sequence diagrams of both submissions are under
[Client-side streaming RPC client](/applications/reactor/reactor_client.md#client-side-streaming-rpc-client), and the
callback's full contract under [Request submission](/applications/reactor/reactor_client.md#request-submission).

## Part 4: both directions

`ActiveBidiReactor` sends a stream and receives a stream, on one RPC. Its read side is
[Part 2](#part-2-receive-a-stream)'s reader and its write side is [Part 3](#part-3-send-a-stream)'s writer: the same
choices, the same callbacks, the same rules. The two directions run independently, and this part shows the few places
where they meet.

Demo: `RouteChat`, which sends route notes, while the server echoes back every note already recorded at the same
location. This part follows
[route_chat_pull_per_message.cpp](/applications/reactor/variants/route_chat_pull_per_message.cpp).

### Step 1: choose all three

| Choice | For | First answer |
|--------|-----|--------------|
| `Delivery` | Received messages | `kPull`, as in [Part 2](#step-1-choose-the-delivery-and-the-pacing) |
| `Submission` | Sent requests | `kReactorPulls` or `kApplicationPushes`, as in [Part 3][p3s1] |
| `ReadPacing` | The read side | The default, `kPerMessage` |

For `Submission`, choose `kReactorPulls` if every request is ready at once, and `kApplicationPushes` otherwise.

[p3s1]: #step-1-choose-who-sends-each-request

```cpp
using Delivery = routeguide::RouteChat::Delivery;
using Submission = routeguide::RouteChat::Submission;
using ClientReactor = routeguide::RouteChat::ClientReactor<Delivery::kPull, Submission::kApplicationPushes>;
```

The parameters come in that order, and only the pacing has a default, so this type uses `kPerMessage`.

### Step 2: write the callbacks

All of them come from the two parts before:

| Slot | Write it as in |
|------|----------------|
| `read_ok` | [Part 2, Step 2](#step-2-write-read_ok) |
| `read_nok`, `done` | [Part 2, Step 3](#step-3-write-read_nok-and-done) |
| `write_done` | [Part 3](#step-3-if-your-application-pushes-pump-one-write-at-a-time) |
| `next_request` | [Part 3, Step 2](#step-2-if-the-reactor-pulls-write-next_request), under `kReactorPulls` only |

Create the reactor without a request, which the write side sends:

```cpp
reactor_ = std::make_unique<ClientReactor>(stub_, CreateClientContext(), std::move(cbs));
```

Then, under `kApplicationPushes`, send the first note, and the next ones from your `OnWriteDone` handler, as in Part 3.
Your read handlers release each kept note, as in Part 2.

### Step 3: know where the two directions meet

- **A read hold stalls only the reads.** While a kept note waits for your handler, no read is armed, but the write
  side keeps sending.
- **Closing your write side does not end the read side.** After `SendLastRequest()` or `CloseRequestStream()`, or once
  `next_request` has returned `std::nullopt`, the server may keep sending, and your read handlers keep working.
- **The RPC ends on its own.** Unlike Part 3's pushing writer, you need not end the write side. Under
  `kApplicationPushes` the reactor holds the RPC only while your write side is idle, and lets that hold go when the read
  side reports the end. Under `kReactorPulls` it holds nothing on the write side at all.
- **A held note delays that end.** While a kept note waits, no read is armed, so the end of the RPC cannot be reported
  until your release call. The release call you owe from Part 2 is therefore the one obligation this reactor adds.

### Step 4: see it run

```bash
./$DIR/applications/reactor/route_guide_active_reactor_client | grep -F '[RouteChat.pull.msg]'
```

The output looks like this, shortened in the middle:

```text
[14:54:21.739284][RouteChat.pull.msg][20403][I]          | reactor[0x564a1b92c370] created
[14:54:21.837629][RouteChat.pull.msg][20403][I]          | RouteChatPullMsgOnWriteDone reactor: 0x564a1b92c370
[14:54:21.853031][RouteChat.pull.msg][20403][I]          | RouteChatPullMsgOnWriteDone reactor: 0x564a1b92c370
[14:54:21.853328][RouteChat.pull.msg][20412][I] skipped  | #1 routeguide.RouteNote: location { } message: "First message"
[14:54:21.853416][RouteChat.pull.msg][20412][I] read_ok  | #1 routeguide.RouteNote
[14:54:21.853525][RouteChat.pull.msg][20403][I] pulled   | #1 arming next read
[14:54:21.853529][RouteChat.pull.msg][20403][I] RESPONSE | #1 routeguide.RouteNote: location { } message: "First message"
[14:54:21.853550][RouteChat.pull.msg][20403][I]          | RouteChatPullMsgOnWriteDone reactor: 0x564a1b92c370
[14:54:21.853871][RouteChat.pull.msg][20413][I] skipped  | #2 routeguide.RouteNote: location { } message: "First message"
[14:54:21.853959][RouteChat.pull.msg][20413][I] read_ok  | #2 routeguide.RouteNote
[14:54:21.854270][RouteChat.pull.msg][20403][I] pulled   | #2 arming next read
...
[14:54:21.855239][RouteChat.pull.msg][20413][I] skipped  | #5 routeguide.RouteNote: location { latitude: 10000000 } ...
[14:54:21.855560][RouteChat.pull.msg][20403][I]          | RouteChatPullMsgOnReadDoneNOk reactor: 0x564a1b92c370
[14:54:21.855563][RouteChat.pull.msg][20403][I]          | RouteChatPullMsgOnDone reactor: 0x564a1b92c370 Status: OK: ...
[14:54:21.855564][RouteChat.pull.msg][20403][I] totals   | 4 kept, 5 skipped
[14:54:21.855573][RouteChat.pull.msg][20403][I]          | reactor[0x564a1b92c370] ended
```

The write side's `OnWriteDone` lines interleave with the read side's: the two directions do not wait for each other.
The read side is Part 2's, with the same arbitrary rule: `read_ok`
keeps every second note and declines the others. The counts change from run to run, because the reference server
keeps one note history for the whole service, so each `RouteChat` scenario also receives the notes the others sent. The
[guided_tour_briefing](/applications/reactor/usecases/guided_tour_briefing.cpp) use case decides for a reason: it keeps
a reply the first time it is seen, and declines the repeats the server echoes again and again.

### Step 5: compare along each choice

```bash
cd applications/reactor/variants
diff route_chat_pull_per_message.cpp route_chat_pull_per_message_producer.cpp  # who sends
diff route_chat_pull_per_message.cpp route_chat_push_per_message.cpp           # the delivery
diff route_chat_pull_per_message.cpp route_chat_pull_turn_by_turn.cpp          # the pacing
```

The first diff touches only the write side, and the two others only the read side: the directions stay independent.

### Your checklist

- [ ] The read side follows Part 2's checklist, and the write side Part 3's.
- [ ] Your write side need not end for the RPC to complete, but each kept note must still be released.
- [ ] The `OnDone` handler destroys the reactor last.

The sequence diagrams are under
[Bidirectional streaming RPC client](/applications/reactor/reactor_client.md#bidirectional-streaming-rpc-client).

## Where to go next

| To                                                   | Read                                                         |
|------------------------------------------------------|--------------------------------------------------------------|
| Compare every variant, and what each one asks of you | [Reactors and their variants][variants] in reactor_client.md |
| Look up a class, a callback, or a sequence diagram   | [reactor_client.md](/applications/reactor/reactor_client.md) |
| Understand why each choice and each hold exists      | [architecture.md](/docs/architecture.md)                     |
| See how the tests cover every variant                | [testing.md](/docs/testing.md)                               |

[variants]: /applications/reactor/reactor_client.md#reactors-and-their-variants
[latest-only]: /applications/reactor/reactor_client.md#keeping-only-the-newest-message
[journal]: /applications/reactor/reactor_client.md#handling-every-message-without-slowing-the-stream
