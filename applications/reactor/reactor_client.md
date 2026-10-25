# Reactor implementation of gRPC clients

This is the reference for the client-side reactor library. It opens with an overview of the four reactors and every
variant they come in, then gives the naming conventions, the delivery contracts, and a class-by-class walkthrough of
all four reactors with their sequence diagrams. It states what the code does.

For a first reactor, step by step, see [walkthrough.md](/docs/walkthrough.md). For why the library is built this way,
which alternatives were rejected, and the reasoning behind the hold rules, see [architecture.md](/docs/architecture.md).
For build and test commands, see [developing.md](/docs/developing.md) and [testing.md](/docs/testing.md).

## Reactors and their variants

The library offers one reactor per gRPC call shape, and up to three template parameters on each. A parameter applies
only where the reactor carries messages in its direction: why, by direction and by single message or stream, is under
[The model behind the reactors' choices](/docs/architecture.md#the-model-behind-the-reactors-choices).

### The four reactors

Each reactor derives from the matching gRPC callback reactor. The RouteGuide layer, `reactor_client_routeguide.h`, adds
one thin adapter per RPC, `routeguide::<Rpc>::ClientReactor`, which starts the call and passes every template parameter
through untouched.

| Reactor              | Call shape       | Sends                    | Receives             | RouteGuide RPC |
|----------------------|------------------|--------------------------|----------------------|----------------|
| `ActiveUnaryReactor` | Unary            | One request, at creation | One final response   | `GetFeature`   |
| `ActiveReadReactor`  | Server-streaming | One request, at creation | A stream of messages | `ListFeatures` |
| `ActiveWriteReactor` | Client-streaming | A stream of requests     | One final response   | `RecordRoute`  |
| `ActiveBidiReactor`  | Bidirectional    | A stream of requests     | A stream of messages | `RouteChat`    |

A final response arrives together with the end of the RPC, through the `done` slot. A stream of messages arrives one
message at a time, through the `read_ok` slot, and `read_nok` reports its end. A stream of requests is sent one write
at a time, since gRPC allows one write in flight per stream, and each completed write is reported through
`write_done`.

### The three parameters

Each parameter answers one question about one direction, and the application chooses it per RPC.

**`Delivery`: who starts the transfer of a received message.** Taken by all four reactors, with no default. See
[Delivery approaches](#delivery-approaches).

| `Delivery` | The callback receives         | The message then waits | The application takes it with |
|------------|-------------------------------|------------------------|-------------------------------|
| `kPull`    | `const ResponseT&`, lent      | In the reactor         | `GetResponse()`, a swap       |
|            |                               |                        | without a deep copy           |
| `kPush`    | `std::unique_ptr<ResponseT>`, | Wherever the callback  | Nothing more: it owns it      |
|            | given                         | hands it               |                               |

`kPull` costs nothing per message, but the reactor has one read target, which the next read overwrites. `kPush` gives
each message its own object, which survives any later read, and brings the obligations under
[Message ownership](#message-ownership).

**`ReadPacing`: whether an incoming stream waits for the application.** Taken by the two reading reactors,
`ActiveReadReactor` and `ActiveBidiReactor`, with `kPerMessage` as the default. The three values sit on one scale, from
a stream that never waits to one that always does. See [Read pacing modes](#read-pacing-modes).

| `ReadPacing`  | The stream waits              | `read_ok` returns     | A held message is released by      |
|---------------|-------------------------------|-----------------------|------------------------------------|
| `kContinuous` | Never                         | Nothing               | Nothing: no message is held        |
| `kPerMessage` | When `read_ok` returns `true` | `bool`: `true` holds, | `GetResponse()` or `ResumeRead()`, |
|               |                               | `false` reads on      | once per kept message              |
| `kTurnByTurn` | After every message           | Nothing               | `GetResponse()` or `ResumeRead()`, |
|               |                               |                       | once per message                   |

`kPerMessage` reaches both ends of the scale at runtime, one message at a time. `kContinuous` and `kTurnByTurn` add no
behaviour to it: they fix its decision for the whole RPC, and the compiler then enforces it.

**`Submission`: who starts the transfer of each request.** Taken by the two writing reactors, `ActiveWriteReactor`
and `ActiveBidiReactor`, with no default. See [Request submission](#request-submission).

| `Submission`         | Each request is sent by                 | The request stream is closed by | Write-side hold |
|----------------------|-----------------------------------------|---------------------------------|-----------------|
| `kApplicationPushes` | The application, with `SendRequest()`   | `SendLastRequest()` or          | Yes             |
|                      | or `SendLastRequest()`, from any thread | `CloseRequestStream()`          |                 |
| `kReactorPulls`      | The reactor, with what the              | `next_request` returning        | None            |
|                      | `next_request` callback returns         | `std::nullopt`                  |                 |

Under `kApplicationPushes` the write calls come from outside gRPC's reactions, so the reactor holds the RPC while one
may still come: see [Write-side idle hold](#write-side-idle-hold) and [Write-flow hold](#write-flow-hold). Under
`kReactorPulls` every write starts inside a reaction, so the reactor holds nothing, and the RPC ends on its own. The
price is the callback's contract: it must answer at once, and cannot answer "not yet".

An outgoing stream has no pacing parameter. gRPC allows one write in flight, and the reader's pacing on the other end
already decides how fast the writes complete.

### Which direction each parameter acts on

Each parameter acts on one direction of the flow, and a reactor takes it only where that direction carries messages:

| Reactor              | Outgoing: requests                          | Incoming: responses                   |
|----------------------|---------------------------------------------|---------------------------------------|
| `ActiveUnaryReactor` | One request, given to the constructor: none | One final response: `Delivery`        |
| `ActiveReadReactor`  | One request, given to the constructor: none | A stream: `Delivery` and `ReadPacing` |
| `ActiveWriteReactor` | A stream: `Submission`                      | One final response: `Delivery`        |
| `ActiveBidiReactor`  | A stream: `Submission`                      | A stream: `Delivery` and `ReadPacing` |

- `Delivery` is on all four reactors, since every reactor receives something.
- `Submission` is on the two that send a stream. A single request goes to the constructor, and is sent when the call
  starts, so no later transfer is left to decide.
- `ReadPacing` is on the two that receive a stream. A final response arrives together with the end of the RPC, so no
  next read is left to arm.

The bidirectional reactor therefore has a push or pull for each direction: `Delivery` for what it receives, and
`Submission` for what it sends. They are independent, so any `Delivery` combines with any `Submission`.

### Every combination

| Reactor              | `Delivery` | `Submission` | `ReadPacing` | Variants |
|----------------------|------------|--------------|--------------|----------|
| `ActiveUnaryReactor` | 2          |              |              | 2        |
| `ActiveReadReactor`  | 2          |              | 3            | 6        |
| `ActiveWriteReactor` | 2          | 2            |              | 4        |
| `ActiveBidiReactor`  | 2          | 2            | 3            | 12       |

That is 24 variants. The parameters follow the message and response types, in the order `kDelivery`, `kSubmission`,
`kPacing`, and only `kPacing` has a default. So `ActiveBidiReactor<RequestT, ResponseT, kDelivery, kSubmission>` uses
`kPerMessage`, and the RouteGuide adapters take the same parameters in the same order, for example
`routeguide::RouteChat::ClientReactor<Delivery::kPull, Submission::kReactorPulls>`.

### What each choice changes in the API

A parameter changes the reactor's type, not only its behaviour. A member that a variant does not offer does not
compile, so a misuse is caught at build time rather than at runtime.

| Member                                | Offered when                                                                |
|---------------------------------------|-----------------------------------------------------------------------------|
| `GetResponse()`                       | `kPull`. On the two reading reactors, only with `kPerMessage` or            |
|                                       | `kTurnByTurn`, the pacings that hold a message                              |
| `ResumeRead()`                        | The two reading reactors under `kPush`, with `kPerMessage` or `kTurnByTurn` |
| `read_ok` returning `bool`            | `kPerMessage`. It returns `void` under the two other pacings                |
| `SendRequest()`, `SendLastRequest()`, | The two writing reactors under `kApplicationPushes`                         |
| `CloseRequestStream()`                |                                                                             |
| The `next_request` slot               | The two writing reactors under `kReactorPulls`. An empty placeholder under  |
|                                       | `kApplicationPushes`, which a function cannot be bound to                   |
| `Status()`, `TryCancel()`,            | Every variant                                                               |
| `CancelAndWait()`                     |                                                                             |

The full API, member by member, is under [Application-facing API](#application-facing-api).

### What each variant asks of the application

| Variant                             | The application owes                           | Otherwise              |
|-------------------------------------|------------------------------------------------|------------------------|
| Every variant                       | Destroying the reactor only once `OnDone()`    | Use after free         |
|                                     | has run, or once `CancelAndWait()` returned    |                        |
|                                     | `true`                                         |                        |
| `kPush`                             | Reclaiming each message handed to the event    | Leak or double free    |
|                                     | queue exactly once, and draining the queue     |                        |
|                                     | before `Halt()`                                |                        |
| A held message: every message under | Exactly one `GetResponse()` (`kPull`) or       | The RPC never ends     |
| `kTurnByTurn`, a kept one under     | `ResumeRead()` (`kPush`) for it                |                        |
| `kPerMessage`                       |                                                |                        |
| `ActiveWriteReactor` under          | Ending the write side: `SendLastRequest()`,    | `OnDone()` never fires |
| `kApplicationPushes`                | `CloseRequestStream()`, `TryCancel()`, or a    |                        |
|                                     | failed write                                   |                        |
| `kReactorPulls`                     | A `next_request` that answers at once, without | A stalled gRPC thread  |
|                                     | blocking, since it runs inside a reaction      |                        |

Everything else is the reactor's job. The reasoning behind each row is under
[Hold requirements](#hold-requirements) and [The obligation a held message adds](#the-obligation-a-held-message-adds).

Every reactor starts its RPC in its constructor, as the last statement of the specialized reactor's constructor, so a
specialization cannot forget to start it, and every reactor that exists is a running RPC. The price is a short window:
a callback can run on a gRPC thread before the `std::make_unique` that creates the reactor has returned, so before the
application's own pointer to the reactor is set. A callback therefore uses the reactor pointer it is given, never the
application's, and leaves everything else to a handler on the application thread. When the reactor is created on that
same thread, as in the demo, the handler runs only after the creating code has returned. Under
`Submission::kReactorPulls`, the first `next_request` call even runs inside the constructor, as described under
[Request submission](#request-submission).

### Choosing a variant

| Parameter    | Choose               | When                                                                         |
|--------------|----------------------|------------------------------------------------------------------------------|
| `Delivery`   | `kPull`              | The message is consumed in place, or pulled when the application is ready,   |
|              |                      | and the event queue carries only reactor pointers                            |
|              | `kPush`              | The message must outlive the next read, or travel on to another component    |
| `ReadPacing` | `kPerMessage`        | By default: `read_ok` decides for each message                               |
|              | `kContinuous`        | Every message is consumed as it arrives, and the stream never waits          |
|              | `kTurnByTurn`        | The stream must wait for the application after every message                 |
| `Submission` | `kReactorPulls`      | Each next request is available at once: a prepared list, a generator, a file |
|              | `kApplicationPushes` | Requests appear later, on the application's own thread                       |

The constraints behind each choice are argued in architecture.md, under
[Choosing for a single terminal response](/docs/architecture.md#choosing-for-a-single-terminal-response),
[Choosing for a stream of messages](/docs/architecture.md#choosing-for-a-stream-of-messages) and
[Why the writing reactors also let the reactor pull](/docs/architecture.md#why-the-writing-reactors-also-let-the-reactor-pull).

### Keeping only the newest message

Some streams are realtime readings: a vehicle's position on a map, a sensor feeding a display, a price. Each message is
worth handling only until a newer one exists, and a consumer slower than the stream should skip to the newest rather
than fall behind. No reactor drops a message: they deliver every one, and dropping the stale ones is the application's
choice. `kPull` with `kContinuous` is the variant for it, with `RpcReactor::LatestSlot`, from `latest_slot.h`, on the
application side:

```cpp
cbs.read_ok = [this](auto*, const Feature& response) {                      // gRPC thread
  if (latest_.Put(Fix{++seq_, response})) EventLoop::TriggerEvent(kOnLatestFix);
};
void OnLatestFix(const EventLoop::Event*) {                                 // application thread
  if (auto fix = latest_.Take()) Draw(*fix);
}
```

- `kContinuous` re-arms each read as soon as `read_ok` returns, so the stream never waits for the application.
- `read_ok` copies the lent message into the slot, replacing one the application has not taken yet. `Put()` only takes
  a short lock, so the reaction never waits either.
- `Put()` asks for an event only when the slot was empty, so at most one event waits in the queue, however fast the
  messages come. The event carries no payload: the message waits in the slot, so no event owns it.
- Every message is either handled or counted by `Replaced()`, and the newest is never lost: it stays in the slot until
  taken. Every reaction returns before `OnDone()`, so a done handler that takes once more always ends on the last one.

`kPush` with `kContinuous` serves the same consumer without the copy, by putting the owned message into the slot. The
`live_vehicle_position` use case runs the pattern, with a map that takes 5 ms to redraw a vehicle's position, and
[latest_slot_test.cpp](/applications/reactor/tests/latest_slot_test.cpp) tests it.

### Handling every message without slowing the stream

Other streams carry records that all matter, but that the server must be able to send at its own pace: the position
reports a vehicle's trip recorder stores, telemetry ingested into a journal, audit events, a large result downloaded at
full speed. `kPush` with `kContinuous`
is the variant for it, with no helper needed:

```cpp
cbs.read_ok = [](auto*, std::unique_ptr<Feature> response) {                // gRPC thread
  EventLoop::TriggerEvent(kOnReport, response.release());
};
void OnReport(const EventLoop::Event* event) {                              // application thread
  const std::unique_ptr<Feature> report{static_cast<Feature*>(event->getData())};
  trip_log_.Store(*report);
}
```

- `kContinuous` re-arms each read as soon as `read_ok` returns, so the stream never waits for the application, however
  slow storage is.
- `kPush` moves each message out of the reactor, so the next read cannot overwrite it, and the event queue owns it until
  the handler reclaims it.
- The event queue is FIFO and the reactor reads one message at a time, so every record reaches the handler once, in
  order, and every one of them is ahead of the `OnDone` event.

The price is the backlog: the records queued but not yet stored. Nothing bounds it but the length of the stream, so
storage slower than the server for long enough runs out of memory. The RPC also ends before storage has caught up, so
the end of the RPC is not the end of the work. A consumer that must bound its backlog keeps `kPush` but chooses
`kPerMessage`, whose `read_ok` holds the stream once the backlog passes a threshold, as `list_features_push_per_message`
does. The `trip_recorder` use case runs the pattern, with storage that takes 1 ms per position report, and
[continuous_journal_test.cpp](/applications/reactor/tests/continuous_journal_test.cpp) tests it.

### Where each variant runs

The demo client, `route_guide_active_reactor_client`, runs 18 of the 24 variants at once, from 18 scenario files under
`applications/reactor/variants/`. The use cases run in their own application, `route_guide_reactor_usecases`, from
`applications/reactor/usecases/`:

| Reactor              | Scenario files                                                            |
|----------------------|---------------------------------------------------------------------------|
| `ActiveUnaryReactor` | `variants/get_feature_pull`, `variants/get_feature_push`                  |
| `ActiveReadReactor`  | `variants/list_features_<delivery>_<pacing>`, for each of the six pairs,  |
|                      | such as `variants/list_features_pull_per_message`                         |
| `ActiveWriteReactor` | `variants/record_route_pull`, `variants/record_route_push`,               |
|                      | `variants/record_route_pull_producer`                                     |
| `ActiveBidiReactor`  | `variants/route_chat_<delivery>_<pacing>`, for each of the six pairs, and |
|                      | `variants/route_chat_pull_per_message_producer`                           |
| Use cases, in        | `usecases/live_vehicle_position` and `usecases/trip_recorder`, on         |
| their own executable | `ActiveReadReactor`                                                       |

A file name gives the RPC, then the `Delivery`, then the `ReadPacing` for a reading reactor. A `_producer` file runs
`Submission::kReactorPulls`, and every other writing scenario `kApplicationPushes`. Neighbouring files differ along one
parameter only, so a `diff` between two of them shows exactly what that parameter changes. The six variants left out
all use `kReactorPulls`, which combines with every `Delivery` and `ReadPacing` exactly as `kApplicationPushes` does, so
one producer scenario per writing reactor shows it. The two use cases are named after a real-life situation instead,
and each runs a `kContinuous` variant for what it is best at: `live_vehicle_position` uses `kPull` for a realtime
reading, as described under [Keeping only the newest message](#keeping-only-the-newest-message), and
`trip_recorder` uses `kPush` for ingestion, as described under
[Handling every message without slowing the stream](#handling-every-message-without-slowing-the-stream).
[The variants' README](/applications/reactor/variants/README.md) lists every scenario with its variant, what it does
with each message and the file to diff it against, and
[the use cases' README](/applications/reactor/usecases/README.md) the situation behind each use case, including the
ones still planned.

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
| `StartRead(&response_)` | Begins async read operation      | Constructor, then `OnReadDone()`, or the call     |
|                         |                                  | that releases a read hold (see below)             |
| `StartWrite()`          | Begins async write operation     | Inside `SendRequest()`. Under `kReactorPulls`:    |
|                         |                                  | `StartCall()` and `OnWriteDone()`                 |
| `StartWriteLast()`      | Write + implied close, one op    | Inside `SendLastRequest()`                        |
| `StartWritesDone()`     | Signals end of client writes     | Inside `CloseRequestStream()`. Under              |
|                         |                                  | `kReactorPulls`: where a pull gets no request     |
| `AddHold()`             | Prevents OnDone until RemoveHold | Inside `OnReadDone()`, before the ok callback,    |
|                         |                                  | under `kPerMessage` and `kTurnByTurn`. Writing    |
|                         |                                  | reactors under `kApplicationPushes` only:         |
|                         |                                  | `StartCall()`, and bidi `OnWriteDone()`           |
| `RemoveHold()`          | Releases hold, allows OnDone     | `GetResponse()` (`kPull`), `ResumeRead()`         |
|                         |                                  | (`kPush`), or `OnReadDone()` when a `kPerMessage` |
|                         |                                  | callback declines. Writing reactors under         |
|                         |                                  | `kApplicationPushes` only: the write methods,     |
|                         |                                  | `TryCancel()` and the reactions ending the write  |
|                         |                                  | side                                              |

### gRPC callbacks (protected overrides)

These callbacks are invoked by gRPC and handled internally by the reactor classes:

| gRPC Callback               | When it fires                        | Invokes user callback          |
|-----------------------------|--------------------------------------|--------------------------------|
| `OnReadDone(bool ok)`       | Read operation completed             | `cbs_.read_ok`/`cbs_.read_nok` |
| `OnWriteDone(bool ok)`      | Write operation completed            | `cbs_.write_done`              |
| `OnWritesDoneDone(bool ok)` | Explicit StartWritesDone() completed | none (internal only)           |
| `OnDone(Status)`            | RPC terminated                       | `cbs_.done`                    |

`ActiveReadReactor` and `ActiveBidiReactor` name these two slots identically, so a reader of either finds the
same `read_ok`/`read_nok` pair. The slot names are the same under both `Delivery` approaches too; only the signature
of the slot that carries a message changes.

### Stream completion tracking

All three streaming reactors track whether further read/write operations are still valid via an internal
`stream_no_more_` flag. Only calls issued from outside a reaction consult it, since a reaction already carries
the `ok` flag that answers the same question:

- Set by `OnReadDone(false)`, `OnWriteDone(false)`, `OnWritesDoneDone()`, and `OnDone()` (whichever apply to that
  reactor's direction). On `ActiveBidiReactor`, `OnWritesDoneDone()` sets it only when the close failed: a
  successful close ends the write side alone, which `writes_done_` already keeps shut under `kApplicationPushes`,
  and no further pull reopens under `kReactorPulls`, while the server may still be sending. Setting the flag there
  would stop a held read from ever being re-armed, and the RPC would never end.
- Checked before issuing a new `StartRead()`, `StartWrite()`, `StartWriteLast()`, or `StartWritesDone()`. On the
  write paths of `ActiveWriteReactor` and `ActiveBidiReactor`, the check happens after the claim is won rather than
  before, see below. On `ActiveWriteReactor`, `TryCancel()` sets it too, since no write can succeed after it, and
  so does `ActiveBidiReactor`'s under `Submission::kReactorPulls`, where the flag is what stops the reactor pulling.
- On the read side, only `GetResponse()` (`Delivery::kPull`) and `ResumeRead()` (`Delivery::kPush`) consult it,
  under `ReadPacing::kPerMessage` or `kTurnByTurn`, since they are the only calls that arm a read from outside a
  reaction. Every other `StartRead()`, including the one after a declined `kPerMessage` message, runs inside a
  reaction and does not consult it. A read-only reactor cannot in practice reach the flag with a hold outstanding,
  because the events that set it all require an operation the hold has already prevented from being armed.
  `ActiveBidiReactor` can, because a write failing while a read hold stands sets the same flag.
- `ActiveBidiReactor` uses a single flag for both directions instead of one per direction. Per gRPC's own
  contract (`grpcpp/support/client_callback.h`), a failure on either read or write means no new read/write
  operation will succeed, so tracking the two directions separately would not add information.
- `OnWritesDoneDone()` fires only for an explicit `StartWritesDone()` (i.e. `CloseRequestStream()`), not for a
  close implied via `StartWriteLast()` (i.e. `SendLastRequest()`). This is per gRPC's own documented distinction.
- Checking the flag alone would narrow, but not close, a race with a concurrent `OnDone()`, because the flag can
  flip to true right after a call already read it, and gRPC destroys the call before `OnDone()` even runs. Both write
  paths therefore stop relying on the check for safety. Each first claims a hold, then re-reads the flag with that
  hold outstanding, so the read can no longer go stale in a way that matters. `ActiveBidiReactor` claims the hold it
  keeps while its write stream is idle, see [Write-side idle hold](#write-side-idle-hold). `ActiveWriteReactor`
  claims the hold it keeps for its whole write flow, see [Write-flow hold](#write-flow-hold).

### Application-facing API

These methods are exposed to application code with naming that reflects application-level semantics:

| Method                 | Purpose                                      | Notes                                        |
|------------------------|----------------------------------------------|----------------------------------------------|
| `SendRequest()`        | Send a request message on the stream         | Takes ownership; caller gives up the request |
| `SendLastRequest()`    | Send the final request and close, atomically | For a known-last message                     |
| `CloseRequestStream()` | Signal end of client requests                | Returns false if rejected (see below)        |
| `next_request` slot    | Hand the reactor the next request, or none   | Called by the reactor, not by the            |
|                        |                                              | application, see below                       |
| `GetResponse()`        | Pull the received message out, by swap       | `Delivery::kPull` only, see below            |
| `ResumeRead()`         | Arm the read following a held message        | `Delivery::kPush` only, see below            |
| `TryCancel()`          | Cancel the entire RPC                        | Thread-safe, any thread                      |
| `CancelAndWait()`      | Cancel, then wait until `OnDone()` has run   | For tearing down mid-RPC, see below          |
| `Status()`             | Get final RPC status                         | Valid after `OnDone`                         |

The three write methods exist only under `Submission::kApplicationPushes`, and the `next_request` slot only under
`Submission::kReactorPulls`, where the reactor issues every write and the close itself. Using one under the other does
not compile. See [Request submission](#request-submission).

`CloseRequestStream()` returns `false` if already closed, if a write is still in flight, or if the RPC has already
failed/finished. While a write is in flight it does nothing, and callers should wait for `OnWriteDone()` and retry, or
use `SendLastRequest()` instead when the last message is known in advance. On `ActiveWriteReactor`, a refusal because
the RPC is over still ends the write side, so that `OnDone()` can follow. See [Write-flow hold](#write-flow-hold).

`CancelAndWait(timeout)` is for an application that must destroy a reactor before its RPC ends, such as at
shutdown, once its event loop no longer runs the `OnDone` handler. gRPC still calls a reactor until its `OnDone()` has
run, and still reads its `ClientContext`, so destroying it earlier is a use-after-free, which `TryCancel()` alone does
not prevent: it does not wait. `CancelAndWait()` cancels the RPC, releases every hold nobody is left to release, and
waits for `OnDone()`:

- a message still held, as `GetResponse()` or `ResumeRead()` would, but without arming a read;
- under `Submission::kApplicationPushes`, the writer's write-flow hold, as `TryCancel()` does, and the bidi reactor's
  idle-window hold, which `OnReadDone(false)` cannot release when no read is armed.

It returns `true` once `OnDone()` has run, when the reactor may be destroyed, and `false` on a timeout, when it may
not. It must not be called from another reaction, during which `OnDone()` cannot run. From inside `OnDone()`, such as
from the `done` callback, it returns `true` at once.

Which reactor offers `GetResponse()` and `ResumeRead()` depends on both template parameters. A call that the table
marks as not offered does not compile.

| Reactor                                    | `ReadPacing`                 | `GetResponse()` | `ResumeRead()` |
|--------------------------------------------|------------------------------|-----------------|----------------|
| `ActiveUnaryReactor`, `ActiveWriteReactor` | None                         | Under `kPull`   | Not offered    |
| `ActiveReadReactor`, `ActiveBidiReactor`   | `kContinuous`                | Not offered     | Not offered    |
| `ActiveReadReactor`, `ActiveBidiReactor`   | `kPerMessage`, `kTurnByTurn` | Under `kPull`   | Under `kPush`  |

`GetResponse()` swaps the held message out into the caller's object, and returns `false` (does nothing) when no message
is ready. On the two reading reactors it first claims the held message's token in `response_ready_`, by atomically
exchanging it for zero, so a duplicate call returns `false`. It then arms the next read, unless the stream is over, and
releases the hold `OnReadDone()` took. It returns `true` whenever it swapped a message out, including when the stream is
over. See [Delivery approaches](#delivery-approaches).

`ResumeRead()` claims the held message's token in `read_held_` the same way, and returns `false` (does nothing) when no
hold is outstanding, which covers a duplicate call. Once the claim is won, it arms the next read unless the stream is
over, then releases the hold. It returns `true` only when it armed a read, so it also returns `false` when the stream is
over, after releasing the hold. See [Read pacing modes](#read-pacing-modes).

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

| RPC Type          | Send            | Close Stream           | Receive, `kPull`  | Receive, `kPush`  | Cancel        |
|-------------------|-----------------|------------------------|-------------------|-------------------|---------------|
| **Unary**         | (constructor)   | N/A                    | done callback,    | done callback,    | `TryCancel()` |
|                   |                 |                        | `GetResponse()`   | owned response    |               |
| **Server-Stream** | (constructor)   | N/A                    | read_ok callback, | read_ok callback, | `TryCancel()` |
|                   |                 |                        | lent message,     | owned message,    |               |
|                   |                 |                        | `GetResponse()`   | `ResumeRead()`    |               |
| **Client-Stream** | `SendRequest()` | `CloseRequestStream()` | done callback,    | done callback,    | `TryCancel()` |
|                   | or the          | or `std::nullopt` from | `GetResponse()`   | owned response    |               |
|                   | `next_request`  | `next_request`         |                   |                   |               |
|                   | slot            |                        |                   |                   |               |
| **Bidi**          | `SendRequest()` | `CloseRequestStream()` | read_ok callback, | read_ok callback, | `TryCancel()` |
|                   | or the          | or `std::nullopt` from | lent message,     | owned message,    |               |
|                   | `next_request`  | `next_request`         | `GetResponse()`   | `ResumeRead()`    |               |
|                   | slot            |                        |                   |                   |               |

Every reactor exposes `Status()` under both approaches. On the two reading reactors, `GetResponse()` and
`ResumeRead()` exist only under `ReadPacing::kPerMessage` and `kTurnByTurn`. On the two writing reactors, the write
methods exist only under `Submission::kApplicationPushes`, and the `next_request` slot only under `kReactorPulls`.

#### Server-side reactors (future)

| RPC Type          | Receive        | Send               | Close Stream            | Finish           |
|-------------------|----------------|--------------------|-------------------------|------------------|
| **Unary**         | (params)       | (via `Finish()`)   | N/A                     | `Finish(status)` |
| **Server-Stream** | (params)       | `SendResponse()`   | `CloseResponseStream()` | `Finish(status)` |
| **Client-Stream** | `GetRequest()` | (via `Finish()`)   | N/A                     | `Finish(status)` |
| **Bidi**          | `GetRequest()` | `SendResponse()`   | `CloseResponseStream()` | `Finish(status)` |

### Bidirectional stream message flow

`GetResponse()` applies under `Delivery::kPull` with a pacing that holds the read side, `kTurnByTurn` or
`kPerMessage`. Under `kPush` the `read_ok` callback is given the message instead.

```text
Client                              Server
  |                                   |
  |---SendRequest()----------------->|  GetRequest()
  |---SendRequest()----------------->|  GetRequest()
  |---CloseRequestStream()---------->|  (read stream ends)
  |                                   |
  |<------------------SendResponse()--|
  |  read_ok callback (+ GetResponse) |
  |<------------------SendResponse()--|
  |  read_ok callback (+ GetResponse) |
  |<-----------CloseResponseStream()--|
  |  read_nok callback                |
```

## Implementation map

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
| Future             | Reactor handle for retrieving results          | `Status()`, and `GetResponse()` under `kPull`  |
| Guards             | Keeps `OnDone()` from concluding an RPC        | The reaction boundary, plus a hold             |

The Scheduler and the Activation Queue come from the [EventLoop library][eventloop-lib]. Under `Delivery::kPull`
the queue carries a notification that a message is ready, as the reactor pointer. Under `Delivery::kPush` it
carries the message itself, which is what makes the obligations under [Message ownership](#message-ownership) the
application's. A `kPull` reader under `ReadPacing::kContinuous` has no message left in the reactor to notify about,
so what its queue carries is whatever the `read_ok` callback extracted.

## Delivery approaches

Every reactor takes a `Delivery` template parameter, which decides how a received message reaches the application.
It has no default, because neither approach is better in general: each one fits some constraints and not others.
The application states its choice per RPC, so both can live side by side in one application, even on the same RPC
method.

| Property                  | `Delivery::kPull`                           | `Delivery::kPush`                        |
|---------------------------|---------------------------------------------|------------------------------------------|
| What the callback gets    | `const ResponseT&`, lent                    | `std::unique_ptr<ResponseT>`, given      |
| Where the message waits   | In the reactor, until `GetResponse()`, when | In the callback, then wherever the       |
|                           | the reading side is held for it             | callback hands it                        |
| What an event carries     | The reactor pointer                         | The message itself                       |
| Response accessor         | `GetResponse()`                             | None                                     |
| Releases a read hold      | `GetResponse()`                             | `ResumeRead()`                           |
| What the application owes | One `GetResponse()` per held message        | One `ResumeRead()` per held message, and |
|                           |                                             | the obligations under                    |
|                           |                                             | [Message ownership](#message-ownership)  |

On the two reading reactors, whether a message is held at all is decided by the separate `ReadPacing` parameter,
under either approach. See [Read pacing modes](#read-pacing-modes).

Only the slot that carries a message changes its parameter between the two approaches. The slot names, the other
callbacks, the write side, `Status()` and `TryCancel()` are identical. What each approach costs, and which one suits
which RPC, is argued in [architecture.md](/docs/architecture.md#why-the-reactors-offer-two-delivery-approaches). Both
are shown side by side on a unary call in [walkthrough.md](/docs/walkthrough.md#part-1-a-unary-call).

## Message ownership

Under `Delivery::kPush`, every reactor hands its messages to a callback as owned objects, and the callback
typically releases them into the event queue. That makes the application responsible for each message. Four
obligations follow, each from a property of the [EventLoop library][eventloop-lib]:

| Obligation                              | Why                                      | Cost                 |
|-----------------------------------------|------------------------------------------|----------------------|
| Reclaim the pointer into a `unique_ptr` | The queue carries a bare `void*`         | Leak                 |
| Reclaim it in exactly one handler       | Every callback under one name is invoked | Double free          |
| Reclaim before anything can throw       | Nothing else owns it meanwhile           | Leak                 |
| Drain the queue before `Halt()`         | `Halt()` discards, it does not drain     | Queued messages leak |

The event data is one pointer, so an application running several concurrent RPCs of the same method cannot tell
which reactor produced a message. It must use a distinct event name per reactor instance, or pass the reactor
pointer the callback received alongside the message.

None of these apply under `Delivery::kPull`, where the reactor never gives a message away. The callback is only
lent it, and a held message stays in the reactor until `GetResponse()` swaps it out. A held message can be swapped out
as soon as the callback has handed it on, while the callback is still running, so handing it on is the last thing the
callback does with the lent message.

Under `Delivery::kPush` with `ReadPacing::kPerMessage`, the callback owns every message, the ones it declines as
much as the ones it keeps. The obligations apply to any message it hands on, whatever it returns.

Two execution strategies are available to a consumer, under either approach. A gRPC-thread callback
(`OnDoneCallback`, `OnReadDoneOkCallback`) can decide whether to hand the message on to the application thread or
drop it. Under `kPush` a drop costs nothing more than dropping the `std::unique_ptr`. Under `kPull` a reading
reactor's callback drops it by not forwarding it: under `kContinuous` it simply returns, and under `kPerMessage` it
returns `false`. A handler registered through `EventLoop::RegisterEvent()` then does the real processing on the
application thread. Processing inside the gRPC-thread callback is discouraged, because it occupies a thread gRPC
needs for other RPCs.

## Read pacing modes

`ActiveReadReactor` and `ActiveBidiReactor` take a `ReadPacing` template parameter after `Delivery`. It selects who
arms the read that follows a delivered message, which is how much the stream waits for the application. It applies
under both `Delivery` approaches, so each reading reactor has six combinations.

The three modes sit on one scale, and are declared in its order:

```text
kContinuous  ─────────────  kPerMessage  ─────────────  kTurnByTurn
never waits                 waits when read_ok          always waits
                            says so (the default)
```

| `ReadPacing`  | Decided                        | Arms the next read                                         |
|---------------|--------------------------------|------------------------------------------------------------|
| `kContinuous` | Once, for the whole RPC        | The reactor, as the last statement of the reaction         |
| `kPerMessage` | For each message, by `read_ok` | The application when `read_ok` returns `true`, the reactor |
|               |                                | when it returns `false`                                    |
| `kTurnByTurn` | Once, for the whole RPC        | The application, through `GetResponse()` or `ResumeRead()` |

`kPerMessage` is the default, so a reactor type that names only its `Delivery`, such as
`routeguide::ListFeatures::ClientReactor<Delivery::kPull>`, uses it. It reaches both ends of the scale at runtime:
a kept message follows the `kTurnByTurn` path, and a declined one the `kContinuous` path, with a hold taken and given
back. `kContinuous` and `kTurnByTurn` fix one end for the whole RPC, for a design that has already made that decision.

The `read_ok` slot's parameter follows `Delivery`, and its return type follows `ReadPacing`:

| `read_ok` signature | `kContinuous`, `kTurnByTurn`                | `kPerMessage`                               |
|---------------------|---------------------------------------------|---------------------------------------------|
| `Delivery::kPull`   | `void(reactor, const ResponseT&)`           | `bool(reactor, const ResponseT&)`           |
| `Delivery::kPush`   | `void(reactor, std::unique_ptr<ResponseT>)` | `bool(reactor, std::unique_ptr<ResponseT>)` |

Both are template parameters, so a reactor's pacing is part of its type and cannot change during the RPC. Why the pacing
is chosen at compile time, and which combination suits which stream, is argued in
[architecture.md](/docs/architecture.md#why-the-reading-reactors-offer-three-pacing-modes).

`OnReadDone(true)` hands a message over in two steps, in this order:

| Step     | Selected by  | What it does                                                                       |
|----------|--------------|------------------------------------------------------------------------------------|
| Delivery | `Delivery`   | `kPull` lends the read target in place. `kPush` swaps it into a new owned message. |
| Pacing   | `ReadPacing` | `kContinuous` calls `read_ok`, then arms the next read. The two other modes take a |
|          |              | hold and publish its token, then call `read_ok`.                                   |

The delivery step completes before any hold is published. Under `kPerMessage`, the pacing step ends by releasing the
hold itself when `read_ok` returns `false`.

### kContinuous

| Property                  | `Delivery::kPull`                         | `Delivery::kPush`                       |
|---------------------------|-------------------------------------------|-----------------------------------------|
| Arms the next read        | The reactor, once `read_ok` returns       | The reactor, once `read_ok` returns     |
| Holds per message         | None                                      | None                                    |
| Messages in flight        | Unbounded: whatever `read_ok` forwards    | Unbounded: whatever `read_ok` forwards  |
| What the application owes | A copy, made before `read_ok` returns, of | The obligations under                   |
|                           | whatever it keeps                         | [Message ownership](#message-ownership) |

Under `kPull`, the message lent to `read_ok` is the reactor's read target. The next read, armed as soon as `read_ok`
returns, overwrites it, so keeping a whole message takes a deep copy. Neither `GetResponse()` nor `ResumeRead()` is
offered under `kContinuous`.

### kPerMessage

| Property                  | `Delivery::kPull`                          | `Delivery::kPush`                           |
|---------------------------|--------------------------------------------|---------------------------------------------|
| Arms the next read        | Kept: the application, in `GetResponse()`. | Kept: the application, in `ResumeRead()`.   |
|                           | Declined: the reactor, in the reaction     | Declined: the reactor, in the reaction      |
| Holds per message         | One, taken before `read_ok` runs, whether  | One, taken before `read_ok` runs, whether   |
|                           | the message is kept or declined            | the message is kept or declined             |
| Messages in flight        | One while a kept message waits, unbounded  | One while a kept message waits, unbounded   |
|                           | otherwise                                  | otherwise                                   |
| What the application owes | One `GetResponse()` per kept message, and  | One `ResumeRead()` per kept message, and    |
|                           | a copy of whatever it keeps from a         | the obligations under                       |
|                           | declined one                               | [Message ownership](#message-ownership)     |

`read_ok` returns `true` to keep the message, which then holds the stream exactly as under
[kTurnByTurn](#kturnbyturn). It returns `false` to decline it, which then lets the stream run on as under
[kContinuous](#kcontinuous). The reactor takes the hold and publishes the message's token (`response_ready_` under
`kPull`, `read_held_` under `kPush`) before every call to `read_ok`, since the answer is not known yet. Each held
message gets its own non-zero token, and zero means that nothing is held.

On a declined message, `OnReadDone()` claims the hold back by compare-exchange against that message's own token,
arms the next read, then releases the hold, all inside the reaction. If the application has already claimed the
token through `GetResponse()` or `ResumeRead()`, the reactor loses the claim and does nothing more. Either way, the
hold is released exactly once. The claim matches only its own message: once the application has released message
N, message N+1 can be read, held, and published on another gRPC thread while the callback for N is still running,
and the decline of N then leaves the hold of N+1 untouched.
Under `kPull`, the re-arm overwrites the declined message. Under `kPush`, the callback owns the declined message as
much as a kept one.

### kTurnByTurn

| Property                  | `Delivery::kPull`                   | `Delivery::kPush`                       |
|---------------------------|-------------------------------------|-----------------------------------------|
| Arms the next read        | The application, in `GetResponse()` | The application, in `ResumeRead()`      |
| Holds per message         | One, taken before `read_ok` runs    | One, taken before `read_ok` runs        |
| Messages in flight        | One                                 | One                                     |
| What the application owes | One `GetResponse()` per message     | One `ResumeRead()` per message, and the |
|                           |                                     | obligations under                       |
|                           |                                     | [Message ownership](#message-ownership) |

The reactor arms no read between `read_ok` and the call that releases the hold, so the stream waits for the
application meanwhile.

### Unbound read_ok slot

When the `read_ok` slot is unbound, `OnReadDone(true)` arms the next read inside the reaction and takes no hold,
under every `ReadPacing` and either `Delivery`. The message is neither lent nor given, and the next read overwrites
it.

### The obligation a held message adds

A held message is one delivered under `kTurnByTurn`, or kept under `kPerMessage`. An application that drops a held
message without its release call strands the RPC. The release call is `GetResponse()` under `kPull` and
`ResumeRead()` under `kPush`. The hold suppresses `OnDone()`, and no outstanding read is left to report the stream
ending, so nothing can conclude the RPC and the reactor can never be legally destroyed. `kContinuous`, and a message
declined under `kPerMessage`, carry no such obligation.

`GetResponse()` and `ResumeRead()` claim the held message's token, by atomically exchanging it for zero, before
releasing anything, so a duplicate or spurious call is rejected rather than acted on. Releasing a hold that was never
taken would drop gRPC's outstanding-callback count below what the RPC actually has in flight, and conclude the RPC
early.

That claim protects the hold count, not the application's intent. Either call releases whichever message is held at
that moment, not a particular one, and once a release has armed the next read, that next message can already be held,
even while its own `read_ok` is still running. So the application calls one of them exactly once per message the
callback kept: every message under `kTurnByTurn`, and those for which `read_ok` returned `true` under `kPerMessage`. A
stray call, such as one for a declined message, may release a message the application meant to keep.

## Request submission

`ActiveWriteReactor` and `ActiveBidiReactor` take a `Submission` template parameter after `Delivery`, and before the
bidi reactor's `ReadPacing`. It selects who starts the transfer of each request, which is the push or pull of the
outgoing direction, as `Delivery` is that of the incoming one. It has no default: neither value contains the other.

| `Submission`         | Starts each transfer                          | Closes the request stream | Write-side hold |
|----------------------|-----------------------------------------------|---------------------------|-----------------|
| `kApplicationPushes` | The application, with `SendRequest()` or      | The application, with     | Yes, see below  |
|                      | `SendLastRequest()`, from any thread          | `SendLastRequest()` or    |                 |
|                      |                                               | `CloseRequestStream()`    |                 |
| `kReactorPulls`      | The reactor, which asks the `next_request`    | The reactor, when         | None            |
|                      | callback for the request                      | `next_request` returns    |                 |
|                      |                                               | `std::nullopt`            |                 |

Under `kReactorPulls`, the `next_request` slot has the signature
`std::optional<RequestT>(reactor)`, and the reactor calls it:

- once in its shadowed `StartCall()`, before the RPC is activated, so inside the specialized reactor's constructor, on
  the thread that constructs it, before the application holds the reactor. The answer is written, or the request
  stream closed, before the RPC starts, which gRPC allows.
- again in `OnWriteDone()`, on a gRPC thread, after each successful write, and right after the `write_done` callback,
  so the application has seen a write complete before it is asked for the next one.

It is never called twice at once, and never after a failed write, after `TryCancel()`, or, on the bidi reactor, after
the read stream ended. An unbound slot counts as a source with nothing to send, so the reactor closes the request stream
at once. The callback runs inside a reaction, so it must answer at once, without blocking, and it cannot answer "not
yet": a source that may have nothing ready while it is not over needs `kApplicationPushes`.

Every write is then issued before the RPC starts or inside a reaction, so the write side takes no hold, and the RPC ends
on its own, as a read-only one does. [Write-side idle hold](#write-side-idle-hold) and
[Write-flow hold](#write-flow-hold) describe `kApplicationPushes` only. The reasoning is in
[architecture.md](/docs/architecture.md#why-the-writing-reactors-also-let-the-reactor-pull).

## Write-side idle hold

Under `Submission::kApplicationPushes`, `ActiveBidiReactor` keeps one hold outstanding exactly while its write stream
is idle, and claiming that hold is what grants permission to write. `ActiveWriteReactor` cannot release a hold that way,
so it holds its whole write flow instead, described under [Write-flow hold](#write-flow-hold). The two classes differ
here rather than mirroring each other.

| Moment                          | What happens to the hold                                   |
|---------------------------------|------------------------------------------------------------|
| `StartCall()`                   | Taken, and published before the RPC is activated           |
| `SendRequest()`                 | Claimed, the write is issued, then released                |
| `SendLastRequest()`             | Claimed, the terminal write is issued, then released       |
| `CloseRequestStream()`          | Claimed, `StartWritesDone()` is issued, then released      |
| `OnWriteDone(true)`, still open | Re-taken in the reaction, before the `write_done` callback |
| `OnWriteDone()`, side is over   | Not re-taken, so `OnDone()` is free to conclude the RPC    |
| `OnReadDone(false)`             | Released, if this reaction is the one that claims it       |

Because the hold is the permission to write, a lost claim is what rejects a call, and no separate write-pending
flag exists. The three application-facing write methods return false when the claim is lost, or when the
post-claim `stream_no_more_` re-check finds the RPC over, in which case they release the hold before refusing.

The two guards answer different questions and neither replaces the other. The claim proves the gRPC-bound state is
still alive, which is what makes the call safe. The re-check reports whether the stream is dead, which is what
stops a doomed operation from being issued at all. A write issued into a terminating RPC is safe but pointless: gRPC
fails it, with `OnWriteDone(false)`, and the RPC still ends. The re-check reports the end synchronously instead, as a
`false` return.

A caller that wants to retry after a lost claim waits for `OnWriteDone()`. A caller refused by the re-check must
not wait, because no write is in flight and no `OnWriteDone()` will arrive. The bare `false` does not distinguish
the two, so an application that needs to tell them apart has to track the RPC's end through `read_nok` or `done`.

The release in `OnReadDone(false)` is what keeps an idle write side from stalling the RPC. Per gRPC's contract a
failure on either direction means neither succeeds any more, so the end of the read stream is a reliable signal
that no application write action will ever arrive. Both the arming and the releasing side publish their flag
before testing the other's, because gRPC may run the two reactions concurrently.

While a message is held, that trigger is not unconditional. Between a held message and its release call
(`GetResponse()` under `kPull`, `ResumeRead()` under `kPush`), no read is armed, so nothing reports the RPC ending.
The idle write hold then waits on the same release call the application already owes. The obligation described
under [The obligation a held message adds](#the-obligation-a-held-message-adds) therefore covers both holds, not just
the read one.

| `ReadPacing`  | A read is armed after each message | The trigger                                  |
|---------------|------------------------------------|----------------------------------------------|
| `kContinuous` | Always, inside the reaction        | Always arrives                               |
| `kTurnByTurn` | Only after the release call        | Waits on the release call                    |
| `kPerMessage` | Declined: inside the reaction.     | Declined: always arrives. Kept: waits on the |
|               | Kept: only after the release call  | release call                                 |

The table holds under either `Delivery`.

## Write-flow hold

Under `Submission::kApplicationPushes`, `ActiveWriteReactor` keeps one hold outstanding for its whole write flow, from
`StartCall()` until the write side ends. This is the pattern gRPC documents for operations started outside the
reactions, under `AddHold()` in `grpcpp/support/client_callback.h`: a hold taken before `StartCall()`, and released once
no more writes will be issued. Each write call claims the flow before it writes, so nothing can release the hold under
it.

| Moment                                        | What happens to the write flow and its hold                   |
|-----------------------------------------------|---------------------------------------------------------------|
| `StartCall()`                                 | Opened: the hold is taken, before the RPC is activated        |
| `SendRequest()`                               | Claimed, the write is issued, then returned open              |
| `SendLastRequest()`                           | Claimed, the last write is issued, then ended: hold released  |
| `CloseRequestStream()`                        | Claimed, `StartWritesDone()` is issued, then ended: released  |
| A write call that finds the RPC over          | Claimed, then ended without writing: hold released            |
| `OnWriteDone(false)`, `TryCancel()`           | Ended, or, if a write call holds the claim, ended by that     |
|                                               | call when it returns the flow                                 |

A write call returns `false` when it cannot claim the flow, because the write side has ended or a call on another
thread holds the claim. With the claim won, it still returns `false` when a write is in flight, which gRPC forbids, and
then the caller retries after `OnWriteDone()`. It also returns `false` when the RPC is over or cancelled, and then it
ends the flow, so no retry is needed and `OnDone()` follows.

`ActiveBidiReactor` can release its idle hold when the RPC ends on its own, because `OnReadDone(false)` always reports
that. A client-streaming RPC has no read side, so no reaction reports a deadline, a server error, or a lost connection
while no write is in flight. The hold then stays, and so does `OnDone()`. This is the obligation the writer adds:

> An `ActiveWriteReactor` completes only once its write side has ended: through `SendLastRequest()`,
> `CloseRequestStream()`, or `TryCancel()`, or through a write that fails.

An application that is still writing learns of such an ending from its next write, which fails, or which the reactor
refuses once a failure was seen. An application that is idle learns of it when it closes or cancels. Without the hold,
gRPC would destroy the call as soon as the status arrived, before calling `OnDone()`, and a write issued in between
would reach a destroyed call.

Ending the flow from another thread is safe because both sides publish before they test. `TryCancel()` and
`OnWriteDone(false)` set `stream_no_more_` before trying to end an open flow, and a write call reopens the flow before
reading `stream_no_more_`. Whichever side looks last ends the flow, so the hold is released exactly once.

## Hold requirements

gRPC's `AddHold()`/`RemoveHold()` keeps `OnDone()` from concluding an RPC while an operation on its state is still
being issued. One rule decides where the library needs one:

> An operation issued from inside a reaction needs no hold. One issued from outside a reaction needs one.

Applied per direction, the rule accounts for every gRPC call the library makes:

| Operation                                  | Issued by                           | Hold                     |
|--------------------------------------------|-------------------------------------|--------------------------|
| `StartRead()`, `kPull`, `kContinuous`      | The reactor, in `OnReadDone()`      | Not needed, none taken   |
| `StartRead()`, `kPush`, `kContinuous`      | The reactor, in `OnReadDone()`      | Not needed, none taken   |
| `StartRead()`, `kPull`, `kTurnByTurn`      | The application, in `GetResponse()` | Required, and taken      |
| `StartRead()`, `kPush`, `kTurnByTurn`      | The application, in `ResumeRead()`  | Required, and taken      |
| `StartRead()`, `kPull`, `kPerMessage` kept | The application, in `GetResponse()` | Required, and taken      |
| `StartRead()`, `kPull`, `kPerMessage`      | The reactor, in `OnReadDone()`      | Not needed, taken anyway |
| declined                                   |                                     |                          |
| `StartRead()`, `kPush`, `kPerMessage` kept | The application, in `ResumeRead()`  | Required, and taken      |
| `StartRead()`, `kPush`, `kPerMessage`      | The reactor, in `OnReadDone()`      | Not needed, taken anyway |
| declined                                   |                                     |                          |
| `StartRead()`, unbound `read_ok`           | The reactor, in `OnReadDone()`      | Not needed, none taken   |
| `StartWrite()`, bidi, `kApplicationPushes` | The application, in `SendRequest()` | Required, and taken      |
| `StartWrite()`, write reactor,             | The application, in `SendRequest()` | Required, and taken      |
| `kApplicationPushes`                       |                                     |                          |
| `StartWrite()`, `kReactorPulls`            | The reactor, in `StartCall()`,      | Not needed, none taken   |
|                                            | before the RPC starts, then in      |                          |
|                                            | `OnWriteDone()`                     |                          |

Under `kPerMessage` the hold is taken before the `read_ok` callback runs, so before the reactor knows whether the
message is kept or declined. A declined message has its hold claimed back and released in the same reaction, right
after `OnReadDone()` re-arms the read itself.

Under `kApplicationPushes`, `ActiveBidiReactor` takes its write-side hold for the window where the write stream sits
idle, so claiming that hold is what grants permission to write, under either `Delivery`. `ActiveWriteReactor` has no
read stream, so no reaction could release an idle hold when the RPC ends on its own. It holds its whole write flow
instead, and claiming that flow is what grants permission to write, as described under
[Write-flow hold](#write-flow-hold). The reasoning behind every row is in
[architecture.md](/docs/architecture.md#why-one-rule-decides-every-hold).

`Status()` and `TryCancel()` are safe from any thread at any time, because neither touches gRPC's internal
callback object: `Status()` reads a member of the reactor, and `TryCancel()` goes through `ClientContext`.

### Why OnReadDone holds before returning

When a read hold is outstanding, under `kTurnByTurn` or for a message kept under `kPerMessage`, the application thread
calls `StartRead()` later, from `GetResponse()` under `kPull` or from `ResumeRead()` under `kPush`, not inside the gRPC
callback. Between the end of the reaction and that later `StartRead()` call, a concurrent `OnDone()` could still arrive
and destroy the reactor's underlying gRPC-bound callback state. Calling `StartRead()` at that point would forward
through that now-destroyed object and segfault. Other reactor operations, `Status()` and `TryCancel()`, do not touch
gRPC's internal object at all, so they remain safe to call regardless of hold state; only the deferred `StartRead()`
needs the hold's protection. `AddHold()` prevents `OnDone()` from firing during this window, and the `RemoveHold()` in
`GetResponse()` or `ResumeRead()` releases it once the new `StartRead()` is issued (or skipped, if the stream is done).
This is the gap gRPC's own callback API leaves open by design, and why it added the [hold mechanism][grpc-hold-pr].

### Hold before handing off, not after

`OnReadDone()` takes the hold before it calls the `OnReadDoneOkCallback`, never once the callback has returned. It
does so under `kPerMessage` and `kTurnByTurn`, with either `Delivery`. The callback is what hands the message off to
the application thread (e.g. `EventLoop::TriggerEvent()`), so from that moment the application thread may already
run `GetResponse()` or `ResumeRead()`, and its `StartRead()` and `RemoveHold()`, before the gRPC thread is back from
the callback. gRPC's hold count is a plain counter, not a lock: a `RemoveHold()` that lands before its `AddHold()` does
not fail, it releases a hold that is not there yet. On the last message of a stream, that early release can bring
the RPC's outstanding-operation count to zero, so gRPC finishes the call and calls `OnDone()`, and the late
`AddHold()` and its own release then finish the same call a second time. TSan reports that as a data race inside
`ClientCallbackReaderImpl::MaybeFinish`, and UBSan as a call through a freed object.

So the order is: hold, then hand off. Publishing the token the releasing call claims counts as handing off too:
`OnReadDone()` sets `response_ready_` (claimed by `GetResponse()`) or `read_held_` (claimed by `ResumeRead()`) only once
the hold is taken. The delivery step comes before both: under `kPush` the message is swapped out of the read target
before the hold is published, so a `ResumeRead()` racing ahead cannot arm a read into a read target still being swapped.
When a `kPerMessage` callback declines the message (returns `false`), `OnReadDone()` claims the token back, restarts
reading itself, and then releases the hold it took, since no `GetResponse()` or `ResumeRead()` will.

### Hold semantics per RPC, not per direction

gRPC's hold count (`AddHold()`/`RemoveHold()`) is a single counter shared by the entire RPC, not one counter per
read/write direction, per gRPC's documented public contract (`grpcpp/support/client_callback.h`, not vendored in
this repository). This has a direct consequence for `ActiveBidiReactor`:

- A read hold does not block other, independent reactions from firing. In particular, `OnWriteDone(false)` on an
  unrelated in-flight write can still fire and set `stream_no_more_` while that hold is outstanding. The hold only
  gates `OnDone()`, not other callbacks.
- Consequently, `GetResponse()` and `ResumeRead()` must call `RemoveHold()` unconditionally, whether or not they
  restart reading. Only the restart is conditional on `stream_no_more_`. Skipping `RemoveHold()` when
  `stream_no_more_` happens to already be true would leak the hold and stall the RPC (`OnDone()` would never fire)
  rather than fail loudly.

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
function depend on the `Delivery` the reactor was instantiated with.

Under `Delivery::kPull`, the callback is lent the response, which stays in the reactor:

````cpp
using OnDoneCallback = std::function<void(grpc::ClientUnaryReactor* reactor, const grpc::Status&, const ResponseT&)>;
````

- the pointer to the active reactor instance (i.e. `this`)
- a reference to the `grpc::Status` content
- a reference to the `Response` content

For the sake of the gRPC processing, it is strongly discouraged to process the response during that callback event. They
are provided to ease some early-reaction logics or to select situations where the response handling is worthy or not.
The application pulls the response later, from its own thread, through `GetResponse()`.

Under `Delivery::kPush`, the callback is given the response:

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

Three public functions can be called by the application side:

- `bool GetResponse(ResponseT& response)`, under `Delivery::kPull` only
- `const grpc::Status& Status()`
- `void TryCancel()`

`GetResponse()` function swaps the underlying data storage of the response object. The swap mechanism is important to
avoid a deep-copy of the content of the response. For the `ActiveUnaryReactor`, having the response swapped is
acceptable because the unary RPC is meant to one response only, so the swap is a good technique to speed up the response
proceeding time. The function will return true when the returned response is valid, which requires the RPC to have
completed with an OK status: after a failed RPC it returns false and leaves `response` untouched. Under
`Delivery::kPush` there is no such accessor: the response has already left the reactor through the `OnDoneCallback`.

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

Each snippet keeps its reactor in a typed `std::unique_ptr`, so no snippet ever recovers a reactor type with a cast.
The snippets name that slot after the RPC, such as `get_feature_`, to stay readable out of context. In the demo
itself the slot is a `reactor_` member of a per-variant scenario class under
[scenarios](/applications/reactor/variants), one class per reactor variant.

#### Instantiation of the ActiveUnaryReactor class

The following snippets instance a `ActiveUnaryReactor` dedicated to the `GetFeature` RPC of the `routeguide` API, once
per `Delivery` approach. Each fills a callback structure with the related `OnDoneCallback`. In these examples, the
callback is bound to the eventloop notification function as an `kGetFeatureOnDone` event.

Under `Delivery::kPull`, the event carries the reactor pointer. From the PlantUML sequence diagram, the corresponding
points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 3.4 : the `cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void GetFeature(routeguide::Point point) {
  using routeguide::GetFeature::Delivery;
  using ClientReactor = routeguide::GetFeature::ClientReactor<Delivery::kPull>;
  using Callbacks = routeguide::GetFeature::Callbacks<Delivery::kPull>;
  using routeguide::GetFeature::ResponseT;
  Callbacks cbs;
  cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
    EventLoop::TriggerEvent(kGetFeatureOnDone, reactor);  // Signal OnDoneCallback event from gRPC thread
  };
  get_feature_ = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), point, std::move(cbs));
}
````

Under `Delivery::kPush`, the event carries the response. From the PlantUML sequence diagram, the corresponding points
are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 3.4 : the `cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void GetFeature(routeguide::Point point) {
  using routeguide::GetFeature::Delivery;
  using ClientReactor = routeguide::GetFeature::ClientReactor<Delivery::kPush>;
  using Callbacks = routeguide::GetFeature::Callbacks<Delivery::kPush>;
  using routeguide::GetFeature::ResponseT;
  Callbacks cbs;
  cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
    // Signal OnDoneCallback from gRPC thread, handing the response ownership to the queue
    EventLoop::TriggerEvent(kGetFeatureOnDone, response.release());
  };
  get_feature_ = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), point, std::move(cbs));
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

Under `Delivery::kPull`, the handler reads `Status()` and pulls the response out of the reactor. From the PlantUML
sequence diagram, the corresponding points are from 3.6 to 3.8.

````cpp
EventLoop::RegisterEvent(kGetFeatureOnDone, [this](const Event*) {
  if (get_feature_->Status().ok()) {
    routeguide::Feature response;
    bool valid = get_feature_->GetResponse(response);
    /**  proceeding of the content of response **/
  }
  get_feature_.reset();
});
````

Under `Delivery::kPush`, the handler reclaims the response the gRPC-thread callback released into the queue, and
reads `Status()` from the reactor it already holds. The event data is the response, so the reactor pointer does not
ride with it. From the PlantUML sequence diagram, the corresponding points are from 3.5 to 3.7.

````cpp
EventLoop::RegisterEvent(kGetFeatureOnDone, [this](const Event* event) {
  const std::unique_ptr<routeguide::Feature> response{static_cast<routeguide::Feature*>(event->getData())};
  if (get_feature_->Status().ok()) {
    /**  proceeding of the content of response **/
  }
  get_feature_.reset();
});
````

### PlantUML sequence flow

#### Delivery::kPull

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50
title gRPC Client reactor for unary RPC, Delivery::kPull

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

#### Delivery::kPush

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50
title gRPC Client reactor for unary RPC, Delivery::kPush

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

When `ClientReadReactor::OnDone` event is handled in the active reactor, the `OnDoneCallback` callback function is
called with the following arguments:

- the pointer to the active reactor instance (i.e. `this`)
- a reference to the `grpc::Status` content

````cpp
using OnReadDoneNOkCallback = std::function<void(grpc::ClientReadReactor<ResponseT>* reactor)>;
````

When `ClientReadReactor::OnReadDone` event with a negative OK is handled in the active reactor, the
`OnReadDoneNOkCallback` callback function is called with the following argument:

- the pointer to the active reactor instance (i.e. `this`)

The `OnReadDoneOkCallback` is the one slot whose signature depends on the template parameters. Its parameter follows
the `Delivery`, and its return type follows the `ReadPacing`:

````cpp
using OnReadDoneOkCallback = std::function<detail::ReadOkResult<kPacing>(
    grpc::ClientReadReactor<ResponseT>* reactor, detail::DeliveredMessage<ResponseT, kDelivery>)>;
````

| `Delivery` | The callback's second argument                                      |
|------------|---------------------------------------------------------------------|
| `kPull`    | `const ResponseT&`, the reactor's read target, lent to the callback |
| `kPush`    | `std::unique_ptr<ResponseT>`, whose ownership transfers to it       |

| `ReadPacing`  | Return type | When the reactor arms the next read                                    |
|---------------|-------------|------------------------------------------------------------------------|
| `kContinuous` | `void`      | As soon as the callback returns                                        |
| `kTurnByTurn` | `void`      | Not at all: `GetResponse()` (`kPull`) or `ResumeRead()` (`kPush`) does |
| `kPerMessage` | `bool`      | `true`: as under `kTurnByTurn`. `false`: as soon as the callback       |
|               |             | returns                                                                |

When `ClientReadReactor::OnReadDone` event with a positive OK is handled in the active reactor, the
`OnReadDoneOkCallback` callback function is called with the following arguments:

- the pointer to the active reactor instance (i.e. `this`)
- the received message, lent (`kPull`) or with its ownership transferred to the callback (`kPush`)

Under `kPull`, the message stays in the reactor's read target. Once the reactor arms the next read, that read
overwrites it, so any data the callback wants to keep past that point must be copied out before the callback
returns. That happens under `kContinuous`, and on a message declined under `kPerMessage`. A held message stays
intact until `GetResponse()` swaps it out.

Under `kPush`, the reactor moves the message out of its read target before the call, so it never refers to that
message again. That holds under every `ReadPacing`, including a message declined under `kPerMessage`.

Whenever the reactor holds the RPC, the hold is already taken before the callback is called, see
[Hold before handing off, not after](#hold-before-handing-off-not-after). The per-mode facts are under
[Read pacing modes](#read-pacing-modes).

Processing the message inside the callback stays discouraged, because the callback runs on a gRPC thread. The
callback exists to hand the message to the application thread, to keep only what the application needs, or to
discard it. Under `kPush`, dropping the `std::unique_ptr` is all a discard takes.

#### Class functions

Four public functions can be called by the application side:

- `bool GetResponse(ResponseT& response)`, under `Delivery::kPull` with `kPerMessage` or `kTurnByTurn`
- `bool ResumeRead()`, under `Delivery::kPush` with `kPerMessage` or `kTurnByTurn`
- `const grpc::Status& Status()`
- `void TryCancel()`

Under `ReadPacing::kContinuous` only `Status()` and `TryCancel()` remain, since the reactor arms every read itself.

`GetResponse()` function does two important things: it swaps the underlying data storage of the response variable and
then (if the stream allows it) it triggers immediately a new read on the stream and resume the RPC. The swap mechanism
is important to avoid a deep-copy of the content of the response. For the `ActiveReadReactor`, the content of that
response is not valuable because it overwrites it on each reading, so the swap is a good technique to speed up the
response proceeding time. It is the call that releases the hold `OnReadDone()` took for a held message. It first
claims the held message's token in `response_ready_`, and returns `false` without acting when no message is held, which
covers a duplicate call. Otherwise it returns `true`, even when the stream is over and no read is armed.

Under `Delivery::kPush` the reactor exposes no response accessor. Its internal `response_` read target keeps a stable
address across every read, and each message is swapped out of it by pointer, without a deep-copy, before the next read
is armed.

`ResumeRead()` arms the read that `OnReadDone()` left unarmed for a held message, and releases the hold it took. It
first claims the held message's token in `read_held_`, and returns `false` without acting when no hold is outstanding,
which covers a duplicate call. Once the claim is won it always releases the hold, and returns `true` only when it armed
a read: `false` also reports that the stream was already over.

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

The following snippets instance a `ActiveReadReactor` dedicated to the `ListFeatures` RPC of the `routeguide` API, once
per `Delivery` approach, each matching one of the two diagrams below. Each fills a callback structure with the
related bound functions. In these examples, the callbacks are bound to the eventloop notification function as an
`kListFeaturesOnReadDoneOk`, `kListFeaturesOnReadDoneNOk`, or `kListFeaturesOnDone` event. The pacing is a template
parameter, so the constructor takes no pacing argument. The first example relies on the default pacing, and the
second names the fixed one it commits to.

Under `Delivery::kPull` with `ReadPacing::kPerMessage`, the `OnReadDoneOkCallback` returns whether the reactor holds
the message. This one keeps the features that have a name, and declines the others. From the PlantUML sequence
diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.6 and 2.16 : the `cbs.read_ok = [](auto* reactor, const ResponseT&) -> bool {...}` lines, for a kept and a
  declined feature
- 4.3 : the `cbs.read_nok = [](auto* reactor) {...}` lines
- 4.6 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void ListFeatures(routeguide::Rectangle rect) {
  using routeguide::ListFeatures::Delivery;
  // ReadPacing::kPerMessage, the default, so only the Delivery is named
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull>;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull>;
  Callbacks cbs;
  cbs.read_ok = [](auto* reactor, const routeguide::Feature& feature) -> bool {
    if (feature.name().empty()) return false;  // false: decline, the reactor reads on at once
    EventLoop::TriggerEvent(kListFeaturesOnReadDoneOk, reactor);  // Signal OnReadDoneOkCallback from gRPC thread
    return true;  // true: hold the RPC until the application pulled the response
  };
  cbs.read_nok = [](auto* reactor) {
    EventLoop::TriggerEvent(kListFeaturesOnReadDoneNOk, reactor);  // Signal OnReadDoneNOkCallback from gRPC thread
  };
  cbs.done = [](auto* reactor, const grpc::Status&) {
    EventLoop::TriggerEvent(kListFeaturesOnDone, reactor);  // Signal OnDoneCallback event from gRPC thread
  };
  list_features_ = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), rect, std::move(cbs));
}
````

Under `Delivery::kPush` with `ReadPacing::kContinuous`, the `OnReadDoneOkCallback` receives the message as an owned
object, releases that ownership into the event queue, and returns nothing. The two other callbacks pass the reactor
pointer instead. From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.14 : the `cbs.read_ok = [](auto*, std::unique_ptr<ResponseT> response) {...}` lines, the declined branch that
  `kContinuous` follows for every message
- 4.3 : the `cbs.read_nok = [](auto* reactor) {...}` lines
- 4.6 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void ListFeatures(routeguide::Rectangle rect) {
  using routeguide::ListFeatures::Delivery;
  using routeguide::ListFeatures::ReadPacing;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush, ReadPacing::kContinuous>;
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
  list_features_ = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), rect, std::move(cbs));
}
````

The four other combinations change only the type aliases and the `read_ok` lambda:

| Combination            | `read_ok` lambda                                                                 |
|------------------------|----------------------------------------------------------------------------------|
| `kPull`, `kContinuous` | Returns nothing, and copies out what it keeps, since the next read overwrites it |
| `kPull`, `kTurnByTurn` | Returns nothing, and passes the reactor pointer, as the `kPerMessage` one does   |
|                        | when it keeps a message                                                          |
| `kPush`, `kTurnByTurn` | Identical to the `kContinuous` one                                               |
| `kPush`, `kPerMessage` | Releases the message into the queue, or drops it, then returns `true` to hold    |
|                        | the stream until `ResumeRead()`, or `false` to read on                           |

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
EventLoop::RegisterEvent(kListFeaturesOnDone, [this](const Event*) {
  const auto status = list_features_->Status();
  /**  proceeding of the status **/
  list_features_.reset();
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

The following snippets are example code of the application-side callback. That code is meant to be executed on the main
application thread, scheduled by the eventloop of the application. In these examples, that lambda is used as the
callback and given to the eventloop when the `kListFeaturesOnReadDoneOk` is notified.

Under `Delivery::kPull`, with a held message, the main goal of that callback is to read the response from the active
reactor and starting a new read operation on the stream. From the PlantUML sequence diagram, the corresponding points
are from 2.8 to 2.11.

````cpp
EventLoop::RegisterEvent(kListFeaturesOnReadDoneOk, [this](const Event*) {
  routeguide::Feature response;
  bool valid = list_features_->GetResponse(response);  // when called, a new stream reading is automatically triggered
  /**  proceeding of the content of response **/
});
````

Under `Delivery::kPush` with `ReadPacing::kContinuous`, the main goal of that callback is to reclaim the ownership of
the message the gRPC-thread callback released into the queue, and then to process it. The handler needs no access to
the reactor, because there is no response left to extract from it and no read to restart: the reactor already armed
its next read before this handler ran. Under `kTurnByTurn`, and for a message kept under `kPerMessage`, the handler
also calls `list_features_->ResumeRead()` once it is done with the message.

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

Both diagrams draw the default `ReadPacing::kPerMessage`, once per `Delivery`, because its two branches are the
two other modes: a kept message is what `kTurnByTurn` does with every message, and a declined one is what
`kContinuous` does with every message, without the hold. The hold is taken before the branch, since the reactor
takes it before `read_ok` answers. Every combination maps onto those branches:

| Combination            | Where the diagrams show it                                                       |
|------------------------|----------------------------------------------------------------------------------|
| `kPull`, `kContinuous` | The `kPull` declined branch, without the `AddHold()` before the branch and its   |
|                        | `RemoveHold()`                                                                   |
| `kPull`, `kPerMessage` | The `kPull` diagram, both branches                                               |
| `kPull`, `kTurnByTurn` | The `kPull` kept branch, for every message                                       |
| `kPush`, `kContinuous` | The `kPush` declined branch, without the `AddHold()` before the branch and its   |
|                        | `RemoveHold()`                                                                   |
| `kPush`, `kPerMessage` | The `kPush` diagram, both branches                                               |
| `kPush`, `kTurnByTurn` | The `kPush` kept branch, for every message                                       |

The steps before the branch carry the same numbers in both diagrams: 2.4 is the delivery step, 2.5 the hold, and 2.6
the hand-off of a kept message.

#### Delivery::kPull, ReadPacing::kPerMessage

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for server-side stream RPC, Delivery::kPull, ReadPacing::kPerMessage

boundary    app      as "Application\nside"
box "ActiveReadReactor" #beige
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
group #lightgreen (grpc-thread callback) onreaddoneok
    data o-[#darkgreen]> reactor : <i>lends response\nto read_ok
    reactor [#darkgreen]-\ grpc : AddHold()\nbefore read_ok answers
end
alt Kept | read_ok returns true, as ReadPacing::kTurnByTurn does for every message
group #lightgreen (grpc-thread callback) onreaddoneok
    {start1} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk
end
    deactivate grpc
    &grpc -> grpc : holding RPC
    activate grpc
    {end1} app -[#darkblue]> reactor : ProceedEvent: OnReadDoneOk
    {start1} <-> {end1} : Eventloop
group #lightblue (app-thread callback) onreaddoneok
    app -[#darkblue]> reactor : GetResponse()
    data o-[#darkblue]> reactor : <i>swaps response out
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
else Declined | read_ok returns false, as ReadPacing::kContinuous does for every message, without the hold
    activate data #yellow
group #lightgreen (grpc-thread callback) onreaddoneok
    data o-[#darkgreen]> reactor : <i>read_ok copies\nwhat it keeps
    deactivate data
    {start21} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ copy, if any
    data o-[#darkgreen]->> reactor : Response holder
    & reactor -[#darkgreen]\ grpc : StartRead()
    reactor [#darkgreen]-\ grpc : RemoveHold()
end
    grpc -> grpc : reading continues
group #lightblue (app-thread callback) onreaddoneok
    {end21} app -[#darkblue]> app : <i>update application\nwith the copy
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

#### Delivery::kPush, ReadPacing::kPerMessage

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for server-side stream RPC, Delivery::kPush, ReadPacing::kPerMessage

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
    reactor <[#darkgreen]- grpc : OnReadDone : true
group #lightgreen (grpc-thread callback) onreaddoneok
    reactor -[#darkgreen]> reactor : <i>extracts response\ninto owned message
    reactor -[#darkgreen]\ grpc : AddHold()\nbefore read_ok answers
end
alt Kept | read_ok returns true, as ReadPacing::kTurnByTurn does for every message
group #lightgreen (grpc-thread callback) onreaddoneok
    {start1} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ owned message
    activate equeue #blue
    deactivate reactor
end
    deactivate grpc
    &grpc -> grpc : holding RPC\n(no read armed)
    activate grpc
group #lightblue (app-thread callback) onreaddoneok
    {end1} equeue -[#darkblue]> app : ProceedEvent: OnReadDoneOk\n+ owned message
    {start1} <-> {end1} : Eventloop
    deactivate equeue
    app -[#darkblue]> app : <i>processes owned message
    activate app #blue
    deactivate app
    app -[#darkblue]> reactor : ResumeRead()
    & reactor -[#darkblue]\ grpc : StartRead()
    reactor -[#darkblue]\ grpc : RemoveHold()
end
    deactivate grpc
    &grpc -> grpc : resuming RPC
    activate grpc #green
else Declined | read_ok returns false, as ReadPacing::kContinuous does for every message, without the hold
    activate reactor #gold
group #lightgreen (grpc-thread callback) onreaddoneok
    {start2} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ owned message
    activate equeue #blue
    reactor -[#darkgreen]\ grpc : StartRead()
    reactor -[#darkgreen]\ grpc : RemoveHold()
    deactivate reactor
end
    grpc -> grpc : reading continues
group #lightblue (app-thread callback) onreaddoneok
    {end2} equeue -[#darkblue]> app : ProceedEvent: OnReadDoneOk\n+ owned message
    {start2} <-> {end2} : Eventloop
    deactivate equeue
    app -[#darkblue]> app : <i>processes owned message
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

The write direction is what distinguishes it, and its `Submission` decides who drives it. gRPC allows one write in
flight at a time, which makes the write flow a turn-by-turn exchange either way: one request, then `OnWriteDone`, then
the next.

- Under `Submission::kApplicationPushes`, the application drives it, so `SendRequest()` issues a gRPC write from the
  application thread, outside any reaction. Because those writes are issued outside the reactions, the reactor holds
  the RPC for its whole write flow, and the RPC completes only once the application ends the write side. See
  [Write-flow hold](#write-flow-hold).
- Under `Submission::kReactorPulls`, the reactor drives it: it asks the `next_request` callback for the first request
  before the RPC starts, then for each next one from `OnWriteDone`, and closes the request stream when the callback has
  none. Every write is issued before the RPC or inside a reaction, so the reactor takes no hold, and the RPC completes
  on its own. See [Request submission](#request-submission).

A client-streaming RPC receives exactly one response, and gRPC writes it as part of completing the call rather
than through a read operation. There is therefore no `OnReadDone` here, and the terminal response is delivered
with the status through `OnDoneCallback`.

The callbacks are `OnWriteDoneCallback` and `OnDoneCallback`, and `NextRequestCallback` under
`Submission::kReactorPulls`.

````cpp
using OnWriteDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>* reactor, bool ok)>;
````

When `ClientWriteReactor::OnWriteDone` is handled in the active reactor, the `OnWriteDoneCallback` callback
function is called with the following arguments:

- the pointer to the active reactor instance (i.e. `this`)
- the `ok` flag, false when the write failed and no further operation will succeed

Under `Submission::kReactorPulls`, the reactor asks for each request through the `next_request` slot:

````cpp
using NextRequestCallback = std::function<std::optional<RequestT>(grpc::ClientWriteReactor<RequestT>* reactor)>;
````

It is called with the pointer to the active reactor instance, and returns the next request, which the reactor takes
over, or `std::nullopt` to close the request stream. The first call runs inside the reactor's constructor, on the
thread that constructs it, and each later one on a gRPC thread, right after `OnWriteDoneCallback` for a successful
write. Its full contract is under [Request submission](#request-submission). Under `kApplicationPushes` the slot is an
empty placeholder, and binding a function to it does not compile.

The `OnDoneCallback` signature depends on the `Delivery` the reactor was instantiated with, exactly as on
`ActiveUnaryReactor`. Under `Delivery::kPull`, the callback is lent the terminal response, which stays in the reactor
until the application pulls it through `GetResponse()`:

````cpp
using OnDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>* reactor,
                                          const grpc::Status&,
                                          const ResponseT&)>;
````

Under `Delivery::kPush`, the callback is given the terminal response:

````cpp
using OnDoneCallback = std::function<void(grpc::ClientWriteReactor<RequestT>* reactor,
                                          const grpc::Status&,
                                          std::unique_ptr<ResponseT>)>;
````

When `ClientWriteReactor::OnDone` is handled in the active reactor, the `OnDoneCallback` callback function is
called with the following arguments:

- the pointer to the active reactor instance (i.e. `this`)
- a reference to the `grpc::Status` content
- the terminal response, lent (`kPull`) or with its ownership transferred to the callback (`kPush`)

Under `kPush`, the response is never null, on the same basis as the unary case: a failed RPC yields an empty message,
so the status is what tells the callback whether the content is meaningful. See
[Message ownership](#message-ownership) for the obligations that come with handing it on to an event queue.

#### Class functions

Six public functions can be called by the application side:

- `bool SendRequest(RequestT&&)`, under `Submission::kApplicationPushes` only
- `bool SendLastRequest(RequestT&&)`, under `Submission::kApplicationPushes` only
- `bool CloseRequestStream()`, under `Submission::kApplicationPushes` only
- `bool GetResponse(ResponseT& response)`, under `Delivery::kPull` only
- `const grpc::Status& Status()`
- `void TryCancel()`

Under `Submission::kReactorPulls`, only the last three remain: the reactor issues every write and the close itself, and
`TryCancel()` also stops it from pulling another request.

`SendRequest()` takes ownership of the request: the caller passes a temporary or an explicit `std::move()`, and
must not keep using the object afterwards. It returns false when the reactor cannot accept the write, and leaves the
request untouched. While a write is in flight, that is all it does, and the caller may retry the same object after the
next `OnWriteDone`. Once the RPC is over or cancelled, the refusal also ends the write flow, so `OnDone()` can follow.

`SendLastRequest()` sends the final request and closes the request stream in one operation, which avoids the gap a
separate `SendRequest()` then `CloseRequestStream()` pair leaves open. It still produces an `OnWriteDone` event,
so a handler that pumps the next request must check whether any request remains rather than sending
unconditionally.

`CloseRequestStream()` signals the end of the request stream on its own, for the case where the last request is
not known in advance. It is refused while a write is in flight.

`SendLastRequest()` and `CloseRequestStream()` end the write flow, and release its hold. One of them, a failed write,
or `TryCancel()` must end it, or the RPC never completes. Under `kReactorPulls` there is no such obligation.

`GetResponse()` and `Status()` behave as described for the unary reactor. `TryCancel()` also ends the write flow,
since no write can succeed after it, so that `OnDone()` follows even when the write side is idle. It is safe from any
thread, including while a write call runs on another.

### Code snippet

#### Instantiation of the ActiveWriteReactor class

The two `Delivery` approaches differ only in the `done` callback, so the snippet below shows `kPush`. Under
`Delivery::kPull`, the type aliases name `Delivery::kPull` and the `done` callback passes the reactor pointer, as
`write_done` does:

````cpp
cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
  EventLoop::TriggerEvent(kRecordRouteOnDone, reactor);  // Signal OnDoneCallback from gRPC thread
};
````

The following snippet instances an `ActiveWriteReactor` dedicated to the `RecordRoute` RPC of the `routeguide`
API. The `done` callback releases the terminal response into the queue, while `write_done` passes the reactor
pointer, because there is no message to carry.

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.8 : the `cbs.write_done = [](auto* reactor, bool) {...}` lines
- 5.4 : the `cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void RecordRoute(std::vector<routeguide::Point> points) {
  using routeguide::RecordRoute::Delivery;
  using routeguide::RecordRoute::Submission;
  using Callbacks = routeguide::RecordRoute::Callbacks<Delivery::kPush, Submission::kApplicationPushes>;
  using ClientReactor = routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kApplicationPushes>;
  using routeguide::RecordRoute::ResponseT;
  record_route_pending_ = std::move(points);
  Callbacks cbs;
  cbs.write_done = [](auto* reactor, bool) {
    EventLoop::TriggerEvent(kRecordRouteOnWriteDone, reactor);  // Signal OnWriteDoneCallback from gRPC thread
  };
  cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {
    // Signal OnDoneCallback from gRPC thread, handing the response ownership to the queue
    EventLoop::TriggerEvent(kRecordRouteOnDone, response.release());
  };
  record_route_ = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), std::move(cbs));
  SendNextRecordRoutePoint();  // Kick the write flow with the first request
}
````

#### Pumping the request stream

The write flow advances one request per `OnWriteDone`. The pump sends the final request through
`SendLastRequest()`, so no separate close is needed.

From the PlantUML sequence diagrams, the corresponding points are 2.1 and 2.10, and 3.1 for the last request.

````cpp
void SendNextRecordRoutePoint() {
  auto point = std::move(record_route_pending_.front());
  record_route_pending_.erase(record_route_pending_.begin());
  if (record_route_pending_.empty()) {
    record_route_->SendLastRequest(std::move(point));  // Last request also closes the stream
  } else {
    record_route_->SendRequest(std::move(point));
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

Under `Delivery::kPull`, the handler pulls the terminal response out of the reactor. From the `Delivery::kPull`
sequence diagram, the corresponding points are from 5.5 to 5.9.

````cpp
EventLoop::RegisterEvent(kRecordRouteOnDone, [this](const Event*) {
  if (record_route_->Status().ok()) {
    routeguide::RouteSummary response;
    bool valid = record_route_->GetResponse(response);
    /**  proceeding of the content of response **/
  }
  record_route_.reset();
});
````

Under `Delivery::kPush`, the handler reclaims the terminal response the gRPC-thread callback released into the
queue, and reads `Status()` from the reactor it already holds.

From the `Delivery::kPush` sequence diagram, the corresponding points are from 5.5 to 5.7.

````cpp
EventLoop::RegisterEvent(kRecordRouteOnDone, [this](const Event* event) {
  const std::unique_ptr<routeguide::RouteSummary> response{
      static_cast<routeguide::RouteSummary*>(event->getData())};
  if (record_route_->Status().ok()) {
    /**  proceeding of the content of response **/
  }
  record_route_.reset();
});
````

### PlantUML sequence flow

The first two diagrams draw `Submission::kApplicationPushes`, once per `Delivery`. They differ only in section 5, where
the terminal response reaches the application, exactly as the two unary diagrams do. Sections 1 to 4 show the
write-flow hold: taken before `StartCall()`, claimed by each write, and released when the write side ends. It is
described under [Write-flow hold](#write-flow-hold).

The third diagram draws `Submission::kReactorPulls`, under `Delivery::kPull`. Its section 5 is the first diagram's,
and sections 1 to 4 show the reactor pulling each request instead, with no hold. The pull is independent of the
delivery, so under `Delivery::kPush` only section 5 changes, as in the second diagram.

#### Delivery::kPull

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for client-side stream RPC, Delivery::kPull

boundary    app      as "Application\nside"
box "ActiveWriteReactor" #beige
collections data     as "Data"
control     reactor  as "ClientWriteReactor"
end box
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
    data o-[#darkblue]->> reactor : Response holder
    & reactor -[#darkblue]\ grpc : async RPC service call\nstub.async()->RpcMethod()
    activate grpc #lightgreen
    reactor -[#darkblue]\ grpc : AddHold()
    note right of reactor
      Covers the whole write flow, since the
      application writes from its own thread.
      Held until the write side ends.
    end note
    reactor -[#darkblue]\ grpc : StartCall()
    activate grpc #green

...
autonumber 2.1
== 2. Request streaming ==
loop one request at a time, while more than one remains
    app -[#darkblue]> reactor : SendRequest()
alt Accepted | the write flow is claimed, no write is in flight, and the RPC is not over
    reactor -[#darkblue]> reactor : <i>claims the write flow,\n<i>so nothing can release the hold
    reactor -[#darkblue]> reactor : <i>moves request into\nthe write buffer
    reactor -[#darkblue]\ grpc : StartWrite()
    & grpc --\? : send Request\nto server
    reactor -[#darkblue]> reactor : <i>returns the write flow, open
    deactivate grpc
group #lightgreen (grpc-thread callback) onwritedone
    reactor <[#darkgreen]- grpc : OnWriteDone : true
    activate grpc #green
    activate reactor #gold
    {start2} app /[#darkgreen]- reactor : TriggerEvent : OnWriteDone
    deactivate reactor
end
    {end2} app -[#darkblue]> reactor : ProceedEvent: OnWriteDone
    {start2} <-> {end2} : Eventloop
group #lightblue (app-thread callback) onwritedone
    app -[#darkblue]> app : <i>sends the next request
    activate app #blue
    deactivate app
end
else Rejected | a write is in flight, or the write side has ended
    reactor -[#darkblue]-> app : false
end
end

autonumber 3.1
== 3. Request stream closing ==
alt the last request closes the stream with it
    app -[#darkblue]> reactor : SendLastRequest()
    reactor -[#darkblue]\ grpc : StartWriteLast()
    & grpc --\? : send the last Request\nand close the stream
    reactor -[#darkblue]\ grpc : RemoveHold()\n<i>ends the write flow
    reactor <[#darkgreen]- grpc : OnWriteDone : true\n<i>handled as in section 2,\n<i>with nothing left to send
else the stream is closed after the last request
    app -[#darkblue]> reactor : CloseRequestStream()
    reactor -[#darkblue]\ grpc : StartWritesDone()
    & grpc --\? : close the\nrequest stream
    reactor -[#darkblue]\ grpc : RemoveHold()\n<i>ends the write flow
    reactor <[#darkgreen]- grpc : OnWritesDoneDone
end

...
autonumber 4.1
== 4. Client-side cancellation ==
    app -[#darkblue]> reactor : TryCancel()
    & reactor -[#darkblue]\ grpc : TryCancel()
    & grpc --\? : RPC termination\nbehest
    reactor -[#darkblue]\ grpc : RemoveHold()\n<i>ends the write flow,\n<i>unless a write call holds it

autonumber 5.1
== 5. RPC completion ==
note across
  OnDone fires only once the write side has ended: closed in section 3, cancelled in section 4,
  or by a failed write. If the RPC ends on its own first, the write-flow hold keeps the call alive,
  and the application learns of it from its next write or close.
end note
    grpc <--? : receive Response\nand RPC termination
    &grpc -[#darkgreen]> data : <i>writes terminal response
    activate data #yellow
    deactivate grpc
    reactor <[#darkgreen]- grpc : OnDone
    deactivate grpc
group #lightgreen (grpc-thread callback) ondone
    {start5} app /[#darkgreen]- reactor : TriggerEvent : OnDone
end
    ...
    {end5} app -[#darkblue]> reactor : ProceedEvent: OnDone
    {start5} <-> {end5} : Eventloop
group #lightblue (app-thread callback) ondone
    app -[#darkblue]> reactor : GetResponse()
    data o-[#darkblue]> reactor : <i>swaps response out
    deactivate data
    reactor -[#darkblue]> app : <i>update application with response
    activate app #blue
    deactivate app
    app -[#darkblue]> reactor : Destroy Reactor
    destroy reactor
end
```

#### Delivery::kPush

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for client-side stream RPC, Delivery::kPush

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
    reactor -[#darkblue]\ grpc : AddHold()
    note right of reactor
      Covers the whole write flow, since the
      application writes from its own thread.
      Held until the write side ends.
    end note
    reactor -[#darkblue]\ grpc : StartCall()
    activate grpc #green

...
autonumber 2.1
== 2. Request streaming ==
loop one request at a time, while more than one remains
    app -[#darkblue]> reactor : SendRequest()
alt Accepted | the write flow is claimed, no write is in flight, and the RPC is not over
    reactor -[#darkblue]> reactor : <i>claims the write flow,\n<i>so nothing can release the hold
    reactor -[#darkblue]> reactor : <i>moves request into\nthe write buffer
    reactor -[#darkblue]\ grpc : StartWrite()
    & grpc --\? : send Request\nto server
    reactor -[#darkblue]> reactor : <i>returns the write flow, open
    deactivate grpc
group #lightgreen (grpc-thread callback) onwritedone
    reactor <[#darkgreen]- grpc : OnWriteDone : true
    activate grpc #green
    activate reactor #gold
    {start2} equeue /[#darkgreen]- reactor : TriggerEvent : OnWriteDone
    activate equeue #blue
    deactivate reactor
end
    {end2} equeue -[#darkblue]> app : ProceedEvent: OnWriteDone
    {start2} <-> {end2} : Eventloop
    deactivate equeue
group #lightblue (app-thread callback) onwritedone
    app -[#darkblue]> app : <i>sends the next request
    activate app #blue
    deactivate app
end
else Rejected | a write is in flight, or the write side has ended
    reactor -[#darkblue]-> app : false
end
end

autonumber 3.1
== 3. Request stream closing ==
alt the last request closes the stream with it
    app -[#darkblue]> reactor : SendLastRequest()
    reactor -[#darkblue]\ grpc : StartWriteLast()
    & grpc --\? : send the last Request\nand close the stream
    reactor -[#darkblue]\ grpc : RemoveHold()\n<i>ends the write flow
    reactor <[#darkgreen]- grpc : OnWriteDone : true\n<i>handled as in section 2,\n<i>with nothing left to send
else the stream is closed after the last request
    app -[#darkblue]> reactor : CloseRequestStream()
    reactor -[#darkblue]\ grpc : StartWritesDone()
    & grpc --\? : close the\nrequest stream
    reactor -[#darkblue]\ grpc : RemoveHold()\n<i>ends the write flow
    reactor <[#darkgreen]- grpc : OnWritesDoneDone
end

...
autonumber 4.1
== 4. Client-side cancellation ==
    app -[#darkblue]> reactor : TryCancel()
    & reactor -[#darkblue]\ grpc : TryCancel()
    & grpc --\? : RPC termination\nbehest
    reactor -[#darkblue]\ grpc : RemoveHold()\n<i>ends the write flow,\n<i>unless a write call holds it

autonumber 5.1
== 5. RPC completion ==
note across
  OnDone fires only once the write side has ended: closed in section 3, cancelled in section 4,
  or by a failed write. If the RPC ends on its own first, the write-flow hold keeps the call alive,
  and the application learns of it from its next write or close.
end note
    grpc <--? : receive Response\nand RPC termination
    &grpc -[#darkgreen]> reactor : <i>writes terminal response
    activate reactor #gold
    deactivate grpc
    reactor <[#darkgreen]- grpc : OnDone
    deactivate grpc
group #lightgreen (grpc-thread callback) ondone
    {start5} equeue /[#darkgreen]- reactor : TriggerEvent : OnDone\n+ owned response
    activate equeue #blue
    deactivate reactor
end
    ...
    {end5} equeue -[#darkblue]> app : ProceedEvent: OnDone\n+ owned response
    {start5} <-> {end5} : Eventloop
    deactivate equeue
group #lightblue (app-thread callback) ondone
    app -[#darkblue]> app : <i>update application with response
    activate app #blue
    deactivate app
    app -[#darkblue]> reactor : Destroy Reactor
    destroy reactor
end
```

#### Delivery::kPull, Submission::kReactorPulls

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for client-side stream RPC, Delivery::kPull, Submission::kReactorPulls

boundary    app      as "Application\nside"
box "ActiveWriteReactor" #beige
collections data     as "Data"
control     reactor  as "ClientWriteReactor"
end box
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
    data o-[#darkblue]->> reactor : Response holder
    & reactor -[#darkblue]\ grpc : async RPC service call\nstub.async()->RpcMethod()
    activate grpc #lightgreen
    reactor -[#darkblue]> reactor : <i>next_request() returns\n<i>the first Request
    note right of reactor
      The first pull runs in the constructor,
      on the application thread, before the
      application holds the reactor.
    end note
    reactor -[#darkblue]\ grpc : StartWrite()
    note right of reactor
      No hold: issued before the RPC starts,
      and every later write inside a reaction.
    end note
    reactor -[#darkblue]\ grpc : StartCall()
    activate grpc #green
    & grpc --\? : send Request\nto server

...
autonumber 2.1
== 2. Request streaming ==
loop while next_request() returns a request
group #lightgreen (grpc-thread callback) onwritedone
    reactor <[#darkgreen]- grpc : OnWriteDone : true
    activate reactor #gold
    {start2} app /[#darkgreen]- reactor : TriggerEvent : OnWriteDone
    reactor -[#darkgreen]> reactor : <i>next_request() returns\n<i>the next Request
    reactor -[#darkgreen]\ grpc : StartWrite()
    & grpc --\? : send Request\nto server
    deactivate reactor
end
    {end2} app -[#darkblue]> reactor : ProceedEvent: OnWriteDone
    {start2} <-> {end2} : Eventloop
group #lightblue (app-thread callback) onwritedone
    app -[#darkblue]> app : <i>update application,\n<i>with nothing to send
    activate app #blue
    deactivate app
end
end

autonumber 3.1
== 3. Request stream closing ==
group #lightgreen (grpc-thread callback) onwritedone
    reactor <[#darkgreen]- grpc : OnWriteDone : true\n<i>write_done handled as in section 2
    activate reactor #gold
    reactor -[#darkgreen]> reactor : <i>next_request() returns\n<i>std::nullopt
    reactor -[#darkgreen]\ grpc : StartWritesDone()
    & grpc --\? : close the\nrequest stream
    deactivate reactor
end
    reactor <[#darkgreen]- grpc : OnWritesDoneDone

...
autonumber 4.1
== 4. Client-side cancellation ==
    app -[#darkblue]> reactor : TryCancel()
    & reactor -[#darkblue]\ grpc : TryCancel()
    & grpc --\? : RPC termination\nbehest
    reactor -[#darkblue]> reactor : <i>no more pulls\n(stream_no_more_)

autonumber 5.1
== 5. RPC completion ==
note across
  OnDone fires as soon as the RPC ends, however it ends: closed in section 3, cancelled in section 4,
  or ended by the server or a deadline at any time, since the reactor holds nothing on the write side.
end note
    grpc <--? : receive Response\nand RPC termination
    &grpc -[#darkgreen]> data : <i>writes terminal response
    activate data #yellow
    deactivate grpc
    reactor <[#darkgreen]- grpc : OnDone
    deactivate grpc
group #lightgreen (grpc-thread callback) ondone
    {start5} app /[#darkgreen]- reactor : TriggerEvent : OnDone
end
    ...
    {end5} app -[#darkblue]> reactor : ProceedEvent: OnDone
    {start5} <-> {end5} : Eventloop
group #lightblue (app-thread callback) ondone
    app -[#darkblue]> reactor : GetResponse()
    data o-[#darkblue]> reactor : <i>swaps response out
    deactivate data
    reactor -[#darkblue]> app : <i>update application with response
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
side as `ActiveWriteReactor`'s, except for the hold that covers the writes.

The two directions are independent. The write side is driven one request at a time, by the application under
`Submission::kApplicationPushes` or by the reactor under `kReactorPulls`, exactly as on `ActiveWriteReactor`. gRPC
drives the read side, and neither waits for the other. A bidirectional RPC carries no terminal response, so `OnDoneCallback`
delivers only the status, unlike the client-streaming case.

The four callbacks are `OnReadDoneOkCallback`, `OnReadDoneNOkCallback`, `OnWriteDoneCallback`, and
`OnDoneCallback`. The read pair and the write callback carry the same signatures and the same meaning as in the
reactors above:

````cpp
// Parameter follows kDelivery, return type follows kPacing, as on ActiveReadReactor
using OnReadDoneOkCallback = std::function<detail::ReadOkResult<kPacing>(
    grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, detail::DeliveredMessage<ResponseT, kDelivery>)>;
// Every combination
using OnReadDoneNOkCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor)>;
using OnWriteDoneCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, bool ok)>;
using OnDoneCallback =
    std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, const grpc::Status&)>;
// Submission::kReactorPulls only, as on ActiveWriteReactor
using NextRequestCallback =
    std::function<std::optional<RequestT>(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor)>;
````

The read side behaves exactly as on `ActiveReadReactor`, in all six combinations. Under `Delivery::kPull`, each
received message stays in the internal read target, and `OnReadDoneOkCallback` is lent it. Under `Delivery::kPush`,
each received message is swapped out of the internal read target and handed to `OnReadDoneOkCallback` as an owned
object. Which side arms the read that follows depends on the `ReadPacing` the reactor was instantiated with, as
described under [Read pacing modes](#read-pacing-modes). Any hold the read side takes covers the read side only.
Processing the message inside that callback stays discouraged, because it runs on a gRPC thread: the callback
exists to hand the message to the application thread, or to discard it.

`OnReadDoneNOkCallback` reports the end of the response stream. Unlike the server-streaming case, it also means
the RPC itself is ending, because a server closes a bidirectional response stream only by finishing the call.

#### Class functions

Seven public functions can be called by the application side:

- `bool SendRequest(RequestT&&)`, under `Submission::kApplicationPushes` only
- `bool SendLastRequest(RequestT&&)`, under `Submission::kApplicationPushes` only
- `bool CloseRequestStream()`, under `Submission::kApplicationPushes` only
- `bool GetResponse(ResponseT& response)`, under `Delivery::kPull` with `kPerMessage` or `kTurnByTurn`
- `bool ResumeRead()`, under `Delivery::kPush` with `kPerMessage` or `kTurnByTurn`
- `const grpc::Status& Status()`
- `void TryCancel()`

`GetResponse()` and `ResumeRead()` behave exactly as on `ActiveReadReactor`, and neither is offered under
`ReadPacing::kContinuous`. The three write functions claim a hold before they issue anything, as
`ActiveWriteReactor`'s do, but a different one: the write-side idle hold, described under
[Write-side idle hold](#write-side-idle-hold), which the read side releases when the RPC ends on its own. So, unlike
the writer, a bidirectional RPC completes without the application ending its write side. The write side is identical
in all six combinations.

Under `Submission::kReactorPulls` the three write functions do not exist, and neither does the idle hold: the reactor
pulls each request from `next_request` and writes it from `OnWriteDone`, as `ActiveWriteReactor` does, and closes the
request stream when the callback has none, while the read side keeps running. It stops pulling once the read stream
ends, a write fails, or `TryCancel()` is called. The read side, its pacing and its holds are unchanged.

Under `Delivery::kPush`, like the three other reactors, `ActiveBidiReactor` exposes no response accessor: its
`response_` read target keeps a stable address across every read, and each message is swapped out of it by pointer,
without a deep-copy, before the next read is armed.

A read hold, under `ReadPacing::kTurnByTurn` or for a message kept under `kPerMessage`, stalls only the read
direction. gRPC counts holds and outstanding operations on one per-RPC counter, but that counter gates `OnDone()`
alone, so the application can keep sending requests while it is behind on responses. The read hold and the
write-side idle hold can therefore both stand at once, and each is claimed and released independently of the other.

### Code snippet

#### Instantiation of the ActiveBidiReactor class

The following snippet instances an `ActiveBidiReactor` dedicated to the `RouteChat` RPC of the `routeguide` API,
under `Delivery::kPush` with `ReadPacing::kContinuous`. Only `read_ok` carries a message; the three other callbacks
pass the reactor pointer. Under `Delivery::kPull` with `ReadPacing::kTurnByTurn`, the type aliases name both, and
`read_ok` passes the reactor pointer too, so the message waits in the reactor for `GetResponse()`:

````cpp
cbs.read_ok = [](auto* reactor, const ResponseT&) {
  EventLoop::TriggerEvent(kRouteChatOnReadDoneOk, reactor);  // Signal OnReadDoneOkCallback from gRPC thread
};  // kTurnByTurn: returns nothing, and the read side stays held until GetResponse()
````

The other combinations change the `read_ok` lambda as listed for `ActiveReadReactor`.

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.9 : the `cbs.write_done = [](auto* reactor, bool) {...}` lines
- 3.14 : the `cbs.read_ok = [](auto*, std::unique_ptr<ResponseT> response) {...}` lines, the declined branch that
  `kContinuous` follows for every message
- 5.5 : the `cbs.read_nok = [](auto* reactor) {...}` lines
- 5.8 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void RouteChat(std::vector<routeguide::RouteNote> notes) {
  using routeguide::RouteChat::Delivery;
  using routeguide::RouteChat::Submission;
  using routeguide::RouteChat::ReadPacing;
  using Callbacks =
      routeguide::RouteChat::Callbacks<Delivery::kPush, Submission::kApplicationPushes, ReadPacing::kContinuous>;
  using ClientReactor =
      routeguide::RouteChat::ClientReactor<Delivery::kPush, Submission::kApplicationPushes, ReadPacing::kContinuous>;
  using routeguide::RouteChat::ResponseT;
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
  route_chat_ = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), std::move(cbs));
  SendNextRouteChatNote();  // Kick the write flow with the first request
}
````

#### OnReadDoneOkCallback

Under `Delivery::kPull`, with a held message, the handler pulls the message out of the reactor, which also re-arms
the read and releases the read hold:

````cpp
EventLoop::RegisterEvent(kRouteChatOnReadDoneOk, [this](const Event*) {
  routeguide::RouteNote response;
  bool valid = route_chat_->GetResponse(response);  // when called, a new stream reading is automatically triggered
  /**  proceeding of the content of response **/
});
````

Under `Delivery::kPush` with `ReadPacing::kContinuous`, the handler reclaims the message the gRPC-thread callback
released into the queue. It needs no access to the reactor: there is no response left to extract and no read to
restart, because the reactor already armed its next read before this handler ran. Under `kTurnByTurn`, and for a
message kept under `kPerMessage`, the handler also calls `route_chat_->ResumeRead()` once it is done with the
message.

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

From the PlantUML sequence diagram, the corresponding points are 2.1 and 2.11.

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

From the PlantUML sequence diagram, the corresponding points are 5.12 and 5.13.

````cpp
EventLoop::RegisterEvent(kRouteChatOnDone, [this](const Event*) {
  const auto status = route_chat_->Status();
  /**  proceeding of the status **/
  route_chat_.reset();
});
````

### PlantUML sequence flow

The two diagrams draw the default `ReadPacing::kPerMessage`, once per `Delivery`. Their kept and declined branches in
section 3 are what `kTurnByTurn` and `kContinuous` do with every message, exactly as the mapping table under the
`ActiveReadReactor` diagrams describes, and sections 1, 2, 4 and 5 differ only in what each event carries. The write
side keeps running while a read hold is outstanding.

#### Delivery::kPull, ReadPacing::kPerMessage

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for bidirectional stream RPC, Delivery::kPull, ReadPacing::kPerMessage

boundary    app      as "Application\nside"
box "ActiveBidiReactor" #beige
collections data     as "Data"
control     reactor  as "ClientBidiReactor"
end box
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
    data o-[#darkblue]->> reactor : Response holder
    & reactor -[#darkblue]\ grpc : StartRead()
    reactor -[#darkblue]\ grpc : AddHold()
    note right of reactor
      Covers the writes the application
      issues from its own thread. Held
      only while the write stream is idle.
    end note
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
    reactor -[#darkblue]> reactor : <i>claims the idle hold,\nwhich permits the write
    reactor -[#darkblue]> reactor : <i>moves request into\nthe write buffer
    reactor -[#darkblue]\ grpc : StartWrite()
    & grpc --\? : send Request\nto server
    reactor -[#darkblue]\ grpc : RemoveHold()
group #lightgreen (grpc-thread callback) onwritedone
    reactor <[#darkgreen]- grpc : OnWriteDone : true
    activate reactor #gold
    reactor -[#darkgreen]\ grpc : AddHold()
    {start2} app /[#darkgreen]- reactor : TriggerEvent : OnWriteDone
    deactivate reactor
end
    {end2} app -[#darkblue]> reactor : ProceedEvent: OnWriteDone
    {start2} <-> {end2} : Eventloop
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
    &grpc -[#darkgreen]> data : <i>writes response
    activate data #yellow
    reactor <[#darkgreen]- grpc : OnReadDone : true
group #lightgreen (grpc-thread callback) onreaddoneok
    data o-[#darkgreen]> reactor : <i>lends response\nto read_ok
    reactor [#darkgreen]-\ grpc : AddHold()\nbefore read_ok answers
end
alt Kept | read_ok returns true, as ReadPacing::kTurnByTurn does for every message
group #lightgreen (grpc-thread callback) onreaddoneok
    {start3} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk
end
    deactivate grpc
    &grpc -> grpc : holding the read side\n(no read armed)\nthe write side keeps running
    activate grpc
    {end3} app -[#darkblue]> reactor : ProceedEvent: OnReadDoneOk
    {start3} <-> {end3} : Eventloop
group #lightblue (app-thread callback) onreaddoneok
    app -[#darkblue]> reactor : GetResponse()
    data o-[#darkblue]> reactor : <i>swaps response out
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
else Declined | read_ok returns false, as ReadPacing::kContinuous does for every message, without the hold
    activate data #yellow
group #lightgreen (grpc-thread callback) onreaddoneok
    data o-[#darkgreen]> reactor : <i>read_ok copies\nwhat it keeps
    deactivate data
    {start31} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ copy, if any
    data o-[#darkgreen]->> reactor : Response holder
    & reactor -[#darkgreen]\ grpc : StartRead()
    reactor [#darkgreen]-\ grpc : RemoveHold()
end
    grpc -> grpc : reading continues
group #lightblue (app-thread callback) onreaddoneok
    {end31} app -[#darkblue]> app : <i>update application\nwith the copy
    {start31} <-> {end31} : Eventloop
    activate app #blue
    deactivate app
end
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
    reactor -[#darkgreen]\ grpc : RemoveHold()\n<i>only if still held
    note right of reactor
      Frees a write side left idle. Claimed first,
      so a write already in flight, or a stream
      closed via SendLastRequest(), is a no-op here.
      Without it, an RPC ending with no application
      write pending could never reach OnDone().
    end note
    {start5} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneNok
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
    app -[#darkblue]> app : <i>update application
    activate app #blue
    deactivate app
end
    {end5} app -[#darkblue]> reactor : ProceedEvent: OnDone
group #lightblue (app-thread callback) ondone
    app -[#darkblue]> app : <i>update application with status
    activate app #blue
    deactivate app
    {start5} <-> {end5} : Eventloop
    app -[#darkblue]> reactor : Destroy Reactor
    destroy reactor
end
```

#### Delivery::kPush, ReadPacing::kPerMessage

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for bidirectional stream RPC, Delivery::kPush, ReadPacing::kPerMessage

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
    reactor -[#darkblue]\ grpc : AddHold()
    note right of reactor
      Covers the writes the application
      issues from its own thread. Held
      only while the write stream is idle.
    end note
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
    reactor -[#darkblue]> reactor : <i>claims the idle hold,\nwhich permits the write
    reactor -[#darkblue]> reactor : <i>moves request into\nthe write buffer
    reactor -[#darkblue]\ grpc : StartWrite()
    & grpc --\? : send Request\nto server
    reactor -[#darkblue]\ grpc : RemoveHold()
group #lightgreen (grpc-thread callback) onwritedone
    reactor <[#darkgreen]- grpc : OnWriteDone : true
    activate reactor #gold
    reactor -[#darkgreen]\ grpc : AddHold()
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
    reactor <[#darkgreen]- grpc : OnReadDone : true
group #lightgreen (grpc-thread callback) onreaddoneok
    reactor -[#darkgreen]> reactor : <i>extracts response\ninto owned message
    reactor -[#darkgreen]\ grpc : AddHold()\nbefore read_ok answers
end
alt Kept | read_ok returns true, as ReadPacing::kTurnByTurn does for every message
group #lightgreen (grpc-thread callback) onreaddoneok
    {start3} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ owned message
    activate equeue #blue
    deactivate reactor
end
    deactivate grpc
    &grpc -> grpc : holding the read side\n(no read armed)\nthe write side keeps running
    activate grpc
group #lightblue (app-thread callback) onreaddoneok
    {end3} equeue -[#darkblue]> app : ProceedEvent: OnReadDoneOk\n+ owned message
    {start3} <-> {end3} : Eventloop
    deactivate equeue
    app -[#darkblue]> app : <i>processes owned message
    activate app #blue
    deactivate app
    app -[#darkblue]> reactor : ResumeRead()
    & reactor -[#darkblue]\ grpc : StartRead()
    reactor -[#darkblue]\ grpc : RemoveHold()
end
    deactivate grpc
    &grpc -> grpc : resuming RPC
    activate grpc #green
else Declined | read_ok returns false, as ReadPacing::kContinuous does for every message, without the hold
    activate reactor #gold
group #lightgreen (grpc-thread callback) onreaddoneok
    {start4} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ owned message
    activate equeue #blue
    reactor -[#darkgreen]\ grpc : StartRead()
    reactor -[#darkgreen]\ grpc : RemoveHold()
    deactivate reactor
end
    grpc -> grpc : reading continues
group #lightblue (app-thread callback) onreaddoneok
    {end4} equeue -[#darkblue]> app : ProceedEvent: OnReadDoneOk\n+ owned message
    {start4} <-> {end4} : Eventloop
    deactivate equeue
    app -[#darkblue]> app : <i>processes owned message
    activate app #blue
    deactivate app
end
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
    reactor -[#darkgreen]\ grpc : RemoveHold()\n<i>only if still held
    note right of reactor
      Frees a write side left idle. Claimed first,
      so a write already in flight, or a stream
      closed via SendLastRequest(), is a no-op here.
      Without it, an RPC ending with no application
      write pending could never reach OnDone().
    end note
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
