# Walkthrough

This walkthrough follows one unary RPC, `GetFeature`, from the moment the application starts it to the moment it
destroys its reactor. It uses the demo application, so every step points at code you can run and read. It ends with
the one choice a unary RPC asks of you, and with what changes once a stream is involved.

The unary RPC is the right place to start because it is the simplest reactor. It has a single choice to make, the
`Delivery` approach, and it needs no hold. Everything else in the library builds on what this one call shows.

For the class-by-class reference, see [reactor_client.md](/applications/reactor/reactor_client.md). For the reasoning
behind the design, see [architecture.md](/docs/architecture.md).

## Before you start

Build the project as described in [developing.md](/docs/developing.md). The commands below use the
`vcpkg-gcc-debug` preset directory.

Two ideas carry the whole library:

| Thread | What runs there |
| -------- | ----------------- |
| A gRPC thread, from gRPC's own pool | The reactor's events, and your callbacks, which only hand each event on |
| The application thread, driven by the EventLoop | Your handlers, where the real work runs later |

Because every piece of application logic runs on that one thread, the application state needs no locks.

## Step 1: run the demo

Start the server, then the demo client, in two terminals:

```bash
DIR=cmake-build-vcpkg-debug-gcc
./$DIR/applications/callback/route_guide_callback_server
./$DIR/applications/reactor/route_guide_active_reactor_client
```

The demo starts every reactor variant at once and keeps its event loop running, so stop it with Ctrl+C once the
output settles. Each variant logs under its own name, so you can pick out the two `GetFeature` scenarios:

```bash
./$DIR/applications/reactor/route_guide_active_reactor_client | grep GetFeature
```

The output looks like this. The pointers and the feature change from run to run, since the demo picks a random
point:

```text
[00:09:55.480370][Main][8974][I] -------------- GetFeature kPull --------------
[00:09:55.480399][GetFeature.pull][8974][I]          | reactor[0x5618565a9d90] created
[00:09:55.480402][Main][8974][I] -------------- GetFeature kPush --------------
[00:09:55.480437][GetFeature.push][8974][I]          | reactor[0x5618565a7a50] created
[00:09:55.483115][GetFeature.push][8974][I] RESPONSE | routeguide.Feature: location { latitude: 405314270 ... }
[00:09:55.483257][GetFeature.push][8974][I]          | reactor[0x5618565a7a50] ended
[00:09:55.483277][GetFeature.pull][8974][I] RESPONSE | routeguide.Feature: location { latitude: 405314270 ... }
[00:09:55.483361][GetFeature.pull][8974][I]          | reactor[0x5618565a9d90] ended
```

The third column is the thread id. Every line here comes from the application thread, because the `GetFeature`
scenarios log nothing on the gRPC thread. Both variants receive the same point and report the same feature.

## Step 2: follow one call

Open [get_feature_pull.cpp](/applications/reactor/scenarios/get_feature_pull.cpp). The call goes through five steps.

1. `GetFeature::Start()` runs on the application thread. It fills a callback struct and creates the reactor, then
   returns at once. This is the Proxy role of the Active Object pattern.
2. The reactor's constructor, in
   [reactor_client_routeguide.h](/applications/reactor/reactor_client_routeguide.h), binds itself to the RPC with
   `stub.async()->GetFeature(...)`, passing the address where gRPC must write the response. It then calls
   `StartCall()`, which sends the request.
3. When the RPC ends, gRPC writes the response into the reactor and calls `OnDone()` on one of its own threads.
   `OnDone()` copies the status into the reactor and calls the `done` slot of your callback struct.
4. The `done` callback runs on that gRPC thread. It does one thing: it calls `EventLoop::TriggerEvent()`, which queues
   the event for the application thread.
5. The EventLoop runs `GetFeature::OnDone()` on the application thread. The handler reads `Status()`, uses the
   response, and destroys the reactor as its last action. gRPC never reuses a reactor once `OnDone()` has fired.

The application never issues anything to gRPC after step 2, apart from an optional `TryCancel()`. That is why a unary
reactor needs no hold: the hold rule only applies to operations issued from the application thread, and there are
none. See [Hold requirements](/applications/reactor/reactor_client.md#hold-requirements) for the rule itself.

## Step 3: compare the two delivery approaches

The two `GetFeature` scenarios differ only in their `Delivery`. Compare them:

```bash
diff applications/reactor/scenarios/get_feature_pull.cpp applications/reactor/scenarios/get_feature_push.cpp
```

The difference comes down to step 4 and step 5: what the event carries, and where the response waits until the
handler runs.

| | `Delivery::kPull` | `Delivery::kPush` |
| --- | --- | --- |
| `done` callback receives | `const ResponseT&`, lent | `std::unique_ptr<ResponseT>`, given |
| The event carries | The reactor pointer | The response itself |
| The response waits | In the reactor | In the event queue |
| The handler gets it with | `GetResponse()`, a swap without a deep copy | A reclaim into a `std::unique_ptr` |
| After a failed RPC | `GetResponse()` returns `false` | An empty message arrives; check `Status()` |

Under `kPull`, the `done` callback passes the reactor on:

```cpp
cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
  EventLoop::TriggerEvent(kOnDone, reactor);
};
```

Under `kPush`, it releases the response into the queue, and the handler must reclaim it first:

```cpp
cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
  EventLoop::TriggerEvent(kOnDone, response.release());
};
```

`Delivery` has no default. Each `ClientReactor` names its approach, as both scenario headers do with
`ClientReactor<Delivery::kPull>` and `ClientReactor<Delivery::kPush>`.

## Step 4: choose for your RPC

For a unary RPC, the choice is about one thing: who owns the response while it waits in your event queue. Under
`kPull` the reactor owns it, so your queue only carries reactor pointers. Under `kPush` the event owns it, so your
handlers take on the obligations listed under
[Message ownership](/applications/reactor/reactor_client.md#message-ownership).

The constraints that decide it, and the reasoning behind each, are in
[Choosing for a single terminal response](/docs/architecture.md#choosing-for-a-single-terminal-response). The same
reasoning applies to `RecordRoute`, whose reactor also delivers one terminal response.

## Step 5: what changes with a stream

A streaming RPC delivers many messages, so a second question appears: who arms the next read? A second template
parameter, `ReadPacing`, answers it, with either `Delivery`. Its three modes sit on one scale, from a stream that never
waits for your handler to one that always does:

| `ReadPacing` | Who arms the next read | Under `kPull`, your handler calls | Under `kPush`, your handler calls |
| --- | --- | --- | --- |
| `kContinuous` | The reactor, once `read_ok` returns | Nothing: `read_ok` used it in place | Nothing: it owns it |
| `kPerMessage` | `read_ok`, for each message | `GetResponse()`, if `read_ok` kept it | `ResumeRead()`, if kept |
| `kTurnByTurn` | Your handler, once it is done | `GetResponse()` | `ResumeRead()` |

The middle one, `kPerMessage`, is the default, so `ClientReactor<Delivery::kPull>` uses it: your `read_ok` returns
`true` to hold a message for your handler, or `false` to let the stream run on. The two ends fix that answer for the
whole RPC, for a design that has already made it, as in `ClientReactor<Delivery::kPull, ReadPacing::kTurnByTurn>`.

That makes six combinations, and the demo runs all six for `ListFeatures`. Whenever your handler arms the next
read, it does so from the application thread, which needs a hold, so the hold rule now matters. Run the demo again
and filter on `ListFeatures.pull.msg`, the default pacing used as a filter. The `read_ok` and `skipped` lines carry
a different thread id from the `pulled` and `RESPONSE` lines, because `read_ok` is logged on the gRPC thread. A
skipped feature is one `read_ok` declined, so no handler line follows it.

Three comparisons show the two axes, one at a time:

```bash
cd applications/reactor/scenarios
diff list_features_pull_turn_by_turn.cpp list_features_pull_per_message.cpp  # what deciding per message adds
diff list_features_push_continuous.cpp list_features_push_turn_by_turn.cpp    # the two ends of the scale
diff list_features_pull_turn_by_turn.cpp list_features_push_turn_by_turn.cpp  # the delivery, at the same pacing
```

Every combination's facts are under [Read pacing modes](/applications/reactor/reactor_client.md#read-pacing-modes),
and which one suits which stream is argued under
[Choosing for a stream of messages](/docs/architecture.md#choosing-for-a-stream-of-messages).

## Where to go next

| To learn | Read |
| ---------- | ------ |
| Every class, callback, and sequence diagram | [reactor_client.md](/applications/reactor/reactor_client.md) |
| Why the library offers its delivery approaches and pacing modes, and the hold rules | [architecture.md](/docs/architecture.md) |
| How the tests cover each approach | [testing.md](/docs/testing.md) |
