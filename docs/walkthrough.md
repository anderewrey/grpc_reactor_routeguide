# Walkthrough

This walkthrough follows two RPCs through the demo application, so every step points at code you can run and read.

- [Part 1](#part-1-one-unary-call) follows one unary RPC, `GetFeature`, from the moment the application starts it to
  the moment it destroys its reactor, and ends with the one choice it asks of you.
- [Part 2](#part-2-a-stream-reader) follows a server-streaming RPC, `ListFeatures`, message by message, and shows
  the second choice a stream adds, and the hold that comes with it.
- [Part 3](#part-3-a-stream-writer) follows a client-streaming RPC, `RecordRoute`, whose application drives the
  stream one write at a time, and shows the one place where the library has no hold to offer.

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

## Part 1: one unary call

### Step 1: run the demo

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

### Step 2: follow one call

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

### Step 3: compare the two delivery approaches

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

### Step 4: choose for your RPC

For a unary RPC, the choice is about one thing: who owns the response while it waits in your event queue. Under
`kPull` the reactor owns it, so your queue only carries reactor pointers. Under `kPush` the event owns it, so your
handlers take on the obligations listed under
[Message ownership](/applications/reactor/reactor_client.md#message-ownership).

The constraints that decide it, and the reasoning behind each, are in
[Choosing for a single terminal response](/docs/architecture.md#choosing-for-a-single-terminal-response). The same
reasoning applies to `RecordRoute`, whose reactor also delivers one terminal response, as
[Part 3](#part-3-a-stream-writer) shows.

## Part 2: a stream reader

A server-streaming RPC delivers many messages, so after each one the reactor has to read again, and a second question
appears: who arms that next read? A second template parameter, `ReadPacing`, answers it. Its three modes sit on one
scale, from a stream that never waits for your handler to one that always does:

```text
kContinuous  ─────────────  kPerMessage  ─────────────  kTurnByTurn
never waits                 waits when read_ok          always waits
                            says so (the default)
```

This part follows `ListFeatures` through the default, `kPerMessage`, under `Delivery::kPull`, because its two
branches already contain the two other modes. It then moves along the scale.

### Step 5: run the stream scenarios

Run the server and the demo as in [Step 1](#step-1-run-the-demo), and filter on the default scenario:

```bash
./$DIR/applications/reactor/route_guide_active_reactor_client | grep ListFeatures.pull.msg
```

The output looks like this, shortened here to its first message, its first declined ones, and its end:

```text
[12:16:14.400576][Main][9663][I] -------------- ListFeatures kPull kPerMessage --------------
[12:16:14.400667][ListFeatures.pull.msg][9663][I]          | reactor[0x5646a644c370] created
[12:16:14.407242][ListFeatures.pull.msg][9673][I] read_ok  | #1 routeguide.Feature
[12:16:14.407296][ListFeatures.pull.msg][9663][I] pulled   | #1 arming next read
[12:16:14.407308][ListFeatures.pull.msg][9663][I] RESPONSE | #1 routeguide.Feature: name: "Patriots Path, ..." ...
...
[12:16:14.415924][ListFeatures.pull.msg][9663][I] pulled   | #23 arming next read
[12:16:14.415937][ListFeatures.pull.msg][9663][I] RESPONSE | #23 routeguide.Feature: name: "1300 Airport Road, ..." ...
[12:16:14.416013][ListFeatures.pull.msg][9673][I] skipped  | #1 unnamed feature: location { latitude: 407113723 ... }
[12:16:14.416132][ListFeatures.pull.msg][9673][I] skipped  | #2 unnamed feature: location { latitude: 402133926 ... }
[12:16:14.416206][ListFeatures.pull.msg][9673][I] skipped  | #3 unnamed feature: location { latitude: 400273442 ... }
[12:16:14.416220][ListFeatures.pull.msg][9673][I] skipped  | #4 unnamed feature: location { latitude: 411236786 ... }
[12:16:14.416250][ListFeatures.pull.msg][9673][I] read_ok  | #24 routeguide.Feature
[12:16:14.416317][ListFeatures.pull.msg][9663][I] pulled   | #24 arming next read
...
[12:16:14.429123][ListFeatures.pull.msg][9663][I]          | ListFeaturesPullMsgOnReadDoneNOk reactor: 0x5646a644c370
[12:16:14.429127][ListFeatures.pull.msg][9663][I]          | ListFeaturesPullMsgOnDone reactor: 0x5646a644c370
[12:16:14.429130][ListFeatures.pull.msg][9663][I] totals   | 64 kept, 36 skipped
[12:16:14.429188][ListFeatures.pull.msg][9663][I]          | reactor[0x5646a644c370] ended
```

Unlike `GetFeature`, this transcript uses two kinds of thread. Thread 9663 is the application thread, the one that
logs the `Main` banner. The `read_ok` and `skipped` lines carry another id, here 9673, because the `read_ok` callback
logs them on a gRPC thread. Which gRPC thread runs it can change from one message to the next.

Three things in it are worth noticing:

- The rectangle holds 100 features, and 36 of them have no name. `read_ok` keeps the 64 named ones and declines the
  others, so this scenario is a filter.
- A `skipped` line is never followed by an application-thread line: a declined feature never reaches your handler.
- `read_ok #k+1` never comes before `pulled #k`, as `#24` follows `#23` here. While a kept feature waits for your
  handler, the stream waits too.

### Step 6: follow one message

Open [list_features_pull_per_message.cpp](/applications/reactor/scenarios/list_features_pull_per_message.cpp). One
feature goes through these steps.

1. `ListFeatures::Start()` fills a callback struct and creates the reactor, on the application thread, as
   `GetFeature::Start()` did.
2. The reactor's constructor, in
   [reactor_client_routeguide.h](/applications/reactor/reactor_client_routeguide.h), binds itself to the RPC, then
   calls `StartRead()` before `StartCall()`. The first read is armed before the call even starts.
3. A feature arrives. gRPC writes it into the reactor's read target and calls `OnReadDone(true)` on one of its own
   threads.
4. The reactor takes a hold, with `AddHold()`, before it calls `read_ok`. It does not know the answer yet, and Step 7
   explains why it cannot wait for it.
5. `read_ok` runs on that gRPC thread, with the feature lent to it.
   - A named feature is kept: `read_ok` logs it, calls `EventLoop::TriggerEvent()` with the reactor, and returns
     `true`. The feature stays in the reactor, and no read is armed.
   - An unnamed feature is declined: `read_ok` logs `skipped` and returns `false`. The reactor arms the next read
     itself and gives the hold back, both inside the same reaction. Nothing reaches the application thread.
6. For a kept feature, the EventLoop runs `ListFeatures::OnReadOk()` on the application thread. It calls
   `GetResponse()`, which swaps the feature out without a deep copy, arms the next read, and releases the hold. Only
   then does it use the feature.
7. When the server has sent everything, `OnReadDone(false)` fires the `read_nok` slot, then `OnDone()` fires the `done`
   slot. Their handlers run on the application thread, and the `OnDone` handler logs the totals and destroys the
   reactor as its last action.

The sequence diagram under
[Delivery::kPull, ReadPacing::kPerMessage](/applications/reactor/reactor_client.md#deliverykpull-readpacingkpermessage)
draws these steps. Points 2.4 and 2.5 are the lent feature and the hold, the `Kept` branch is steps 5 and 6, and the
`Declined` branch is the reactor reading on by itself.

### Step 7: see why the hold is needed

In Part 1, the application issued nothing to gRPC after creating the reactor, so it needed no hold. Here it does:
`GetResponse()` arms the next read from the application thread, outside any reaction. Between the moment `read_ok`
returns and the moment `GetResponse()` runs, no read is armed and no reaction is running, so without a hold gRPC could
conclude the RPC and fire `OnDone()`, and `GetResponse()` would then reach into a finished RPC. The hold keeps the RPC
alive across that gap.

Two orderings make it safe:

- The hold is taken before `read_ok` runs, not after. `read_ok` hands the feature off when it calls `TriggerEvent()`,
  and the application thread can run `GetResponse()`, and release the hold, before `read_ok` has even returned.
- Inside `GetResponse()`, the next read is armed before the hold is released, so gRPC never sees a moment with
  neither.

A declined feature needs no hold at all, since the reactor re-arms the read from inside the reaction. It still costs
one, because the reactor takes it before `read_ok` answers, and gives it back at once. The rule itself is under
[Hold requirements](/applications/reactor/reactor_client.md#hold-requirements), and the reasoning under
[Why kPerMessage takes one for every message](/docs/architecture.md#why-kpermessage-takes-one-for-every-message).

The `pulled` line is logged just before `GetResponse()`, not after it, for a related reason. `GetResponse()` arms the
next read, so the next `read_ok` line can be printed before `GetResponse()` even returns.

### Step 8: release once per kept message

`GetResponse()` releases whichever message is held at that moment, not a particular one. So your handler calls it
exactly once for every message `read_ok` kept, and never for one it declined. A call for a declined message could
release the next message, which may already be held while its own `read_ok` is still deciding.

In this scenario, the rule holds by construction: only a kept feature triggers an event, so `OnReadOk()` runs once per
kept feature. Under `Delivery::kPush` it takes care, because the callback owns every message, declined ones included,
and usually hands all of them on. Filter on `ListFeatures.push.msg` to see the push scenario: its `read_ok` holds the
stream only once three messages wait for the application, and each `read_ok` line ends with `held` or `not held`.
Only a held message gets a `resumed` line. Open
[list_features_push_per_message.cpp](/applications/reactor/scenarios/list_features_push_per_message.cpp) to see how:
`read_ok` records the number of the message it held, and only that message's handler calls `ResumeRead()`.

### Step 9: move along the scale

`kPerMessage` decides for each message. The two other modes fix that decision for the whole RPC, each taking one of
its branches for every message. Compare the scenarios one step at a time:

```bash
cd applications/reactor/scenarios
diff list_features_pull_per_message.cpp list_features_pull_turn_by_turn.cpp  # every message kept
diff list_features_pull_turn_by_turn.cpp list_features_pull_continuous.cpp   # no message kept
diff list_features_pull_turn_by_turn.cpp list_features_push_turn_by_turn.cpp # the delivery, at the same pacing
```

| `ReadPacing` | `read_ok` returns | Under `kPull`, your handler calls | Under `kPush`, your handler calls | Rules out |
| --- | --- | --- | --- | --- |
| `kContinuous` | Nothing | Nothing: `read_ok` used it in place | Nothing: it owns it | Holding, and every release call |
| `kPerMessage` | `true` to keep, `false` to decline | `GetResponse()`, if kept | `ResumeRead()`, if kept | Nothing |
| `kTurnByTurn` | Nothing | `GetResponse()`, for every message | `ResumeRead()`, for every message | Deciding |

A fixed mode adds no behaviour. What it adds is a guarantee the compiler enforces. Try it: in
`list_features_pull_continuous.cpp`, call `reactor_->GetResponse(...)` from a handler and build. It does not compile:
the compiler reports that the constraints of `GetResponse()` are not satisfied, because under `kContinuous` no message
is ever held, so there is nothing to pull.

The `RouteChat` scenarios repeat all six combinations on a bidirectional stream. Their read side works exactly like
`ListFeatures`, while the write side keeps sending, even when a read hold stalls the reads. Filter on
`RouteChat.pull.msg` to see another filter: it keeps each note the first time the server echoes it, and declines the
repeats.

### Step 10: choose for your stream

Start from the default. A `ClientReactor` that names only its `Delivery`, such as
`routeguide::ListFeatures::ClientReactor<Delivery::kPull>`, uses `kPerMessage`, and its `read_ok` decides one message
at a time. Fix an end of the scale once your design has decided: `kContinuous` when every message is consumed as it
arrives, `kTurnByTurn` when the stream must wait for the application every time.

Which combination suits which stream is argued under
[Choosing for a stream of messages](/docs/architecture.md#choosing-for-a-stream-of-messages), and what each fixed
mode rules out under
[Why kPerMessage is the default, and the two others still exist](/docs/architecture.md#why-kpermessage-is-the-default-and-the-two-others-still-exist).

## Part 3: a stream writer

A client-streaming RPC turns Part 2 around: the application streams the messages, and the server answers once, with a
single response at the end. `RecordRoute` sends a route as a stream of points, and the server replies with a
`RouteSummary`. Its reactor, `ActiveWriteReactor`, takes a `Delivery` for that single response, exactly as in Part 1,
and no `ReadPacing`, since there is no read stream to pace. What is new is that your application drives the stream,
one write at a time.

### Step 11: run the writer scenarios

Run the server and the demo as in [Step 1](#step-1-run-the-demo), and filter on the two writer scenarios:

```bash
./$DIR/applications/reactor/route_guide_active_reactor_client | grep RecordRoute
```

The output looks like this:

```text
[12:16:14.400920][Main][9663][I] -------------- RecordRoute kPull --------------
[12:16:14.400953][RecordRoute.pull][9663][I]          | reactor[0x5646a6466440] created
[12:16:14.400964][Main][9663][I] -------------- RecordRoute kPush --------------
[12:16:14.400999][RecordRoute.push][9663][I]          | reactor[0x5646a6469f40] created
[12:16:14.402173][RecordRoute.pull][9663][I]          | RecordRoutePullOnWriteDone reactor: 0x5646a6466440
[12:16:14.402215][RecordRoute.push][9663][I]          | RecordRoutePushOnWriteDone reactor: 0x5646a6469f40
[12:16:14.402405][RecordRoute.pull][9663][I]          | RecordRoutePullOnWriteDone reactor: 0x5646a6466440
[12:16:14.402485][RecordRoute.push][9663][I]          | RecordRoutePushOnWriteDone reactor: 0x5646a6469f40
[12:16:14.402570][RecordRoute.pull][9663][I]          | RecordRoutePullOnWriteDone reactor: 0x5646a6466440
[12:16:14.402712][RecordRoute.push][9663][I]          | RecordRoutePushOnWriteDone reactor: 0x5646a6469f40
[12:16:14.408311][RecordRoute.push][9663][I] RESPONSE | routeguide.RouteSummary: point_count: 3 feature_count: 2 ...
[12:16:14.408352][RecordRoute.push][9663][I]          | reactor[0x5646a6469f40] ended
[12:16:14.408362][RecordRoute.pull][9663][I] RESPONSE | routeguide.RouteSummary: point_count: 3 feature_count: 2 ...
[12:16:14.408385][RecordRoute.pull][9663][I]          | reactor[0x5646a6466440] ended
```

Every line comes from the application thread again, as in Part 1, because these scenarios log nothing on a gRPC
thread. Each scenario sends three points and logs three `OnWriteDone` lines, one per completed write, before its one
`RESPONSE`. Both report the same summary, since they send the same route.

### Step 12: follow the request stream

Open [record_route_pull.cpp](/applications/reactor/scenarios/record_route_pull.cpp). The route goes through these
steps.

1. `RecordRoute::Start()` keeps the points in its own queue, creates the reactor, and sends the first point itself,
   through `SendNextPoint()`.
2. The reactor's constructor, in
   [reactor_client_routeguide.h](/applications/reactor/reactor_client_routeguide.h), binds itself to the RPC with the
   address where gRPC must write the final response, then calls `StartCall()`. Unlike `ListFeatures`, it arms no read:
   there is nothing to read until the RPC ends.
3. `SendNextPoint()` takes the next point out of the queue and hands it to `SendRequest()`, or to `SendLastRequest()`
   for the last one. The reactor moves it into its own write buffer, since the caller gives it up.
4. gRPC sends the point, then calls `OnWriteDone()` on one of its own threads. The `write_done` slot only calls
   `TriggerEvent()`.
5. The EventLoop runs `RecordRoute::OnWriteDone()` on the application thread, and it sends the next point, if any
   remains. The point sent through `SendLastRequest()` completes with an `OnWriteDone` too, which is the third line in
   Step 11. By then the queue is empty, so the handler sends nothing.
6. Once the request stream is closed, the server answers. gRPC writes the `RouteSummary` into the reactor and calls
   `OnDone()`, and from there it is Part 1's [Step 2](#step-2-follow-one-call) again: the `done` slot queues the
   event, and the handler pulls the response with `GetResponse()` and destroys the reactor.

The sequence diagrams under
[Client-side streaming RPC client](/applications/reactor/reactor_client.md#client-side-streaming-rpc-client) draw
these steps: section 2 is the pump, section 3 the closing, and section 5 the response.

### Step 13: keep one write in flight

gRPC allows one write in flight per stream. `SendRequest()` returns `false`, and sends nothing, if a write is still
in flight, if the stream is already closed, or if the RPC is already over. The pump never meets that refusal, because
it sends only from `Start()` and from the `OnWriteDone` handler, each time the previous write has completed. Sending
from anywhere else, such as a timer, has to be ready for `false`.

A refusal is a dead end for the pump: the point has already left the queue, and no `OnWriteDone` will come to drive
the next one. That is why `SendNextPoint()` checks the result, and logs `write rejected` instead of stalling in
silence.

The stream can be closed in two ways, which the diagrams draw as section 3:

| Call | When | Completes with |
| --- | --- | --- |
| `SendLastRequest()` | With the last request, in one operation | `OnWriteDone`, as any write |
| `CloseRequestStream()` | After the last request's `OnWriteDone` | `OnWritesDoneDone`, which reaches no slot |

The pump uses `SendLastRequest()`: it saves an operation, and leaves no moment between the last write and the close.

### Step 14: find the write that has no hold

Part 2's rule says that an operation issued from the application thread, outside any reaction, needs a hold.
`SendRequest()` is exactly that: it calls `StartWrite()` on the application thread. Yet `ActiveWriteReactor` takes no
hold.

Open [reactor_client.h](/applications/reactor/reactor_client.h) at `ActiveWriteReactor::SendRequest()`. Before it
writes, it checks that the RPC is not over yet. That check narrows the window but cannot close it: a deadline or a
server cancellation can end the RPC on a gRPC thread right after the check passes, and the write then reaches into a
finished RPC. The writer diagrams mark that spot with a note. It is the library's one known residual race.

Now compare `ActiveBidiReactor::SendRequest()` in the same file. A bidirectional reactor keeps a hold while its write
side is idle, and a write starts by claiming it. That works because the bidi reactor has a read side: when the RPC
ends on its own, `OnReadDone(false)` arrives, and releases the idle hold. A client-streaming RPC has no read side, so
no reaction would ever arrive to release such a hold, and an RPC ending on its own could then never reach `OnDone()`.
The full reasoning, including the approach tried and rejected, is under
[Why the write side of ActiveWriteReactor has no hold](/docs/architecture.md#why-the-write-side-of-activewritereactor-has-no-hold).

### Step 15: choose for your writer

`ReadPacing` does not apply, so the only choice is the `Delivery`, and it is Part 1's choice again: who owns the single
response while it waits in your event queue. Compare the two scenarios:

```bash
diff applications/reactor/scenarios/record_route_pull.cpp applications/reactor/scenarios/record_route_push.cpp
```

As for `GetFeature`, they differ only in the `done` slot and the `OnDone` handler. The write pump is identical, since
the `Delivery` changes nothing on the write side. The constraints that decide the choice are under
[Choosing for a single terminal response](/docs/architecture.md#choosing-for-a-single-terminal-response).

`RouteChat` puts Part 2 and Part 3 together on one bidirectional stream: a read side paced as in Part 2, and a write
side pumped as here, but covered by the idle hold. Its diagrams are under
[Bidirectional streaming RPC client](/applications/reactor/reactor_client.md#bidirectional-streaming-rpc-client).

## Where to go next

| To learn | Read |
| ---------- | ------ |
| Every class, callback, and sequence diagram | [reactor_client.md](/applications/reactor/reactor_client.md) |
| Why the library offers its delivery approaches and pacing modes, and the hold rules | [architecture.md](/docs/architecture.md) |
| How the tests cover each approach | [testing.md](/docs/testing.md) |
