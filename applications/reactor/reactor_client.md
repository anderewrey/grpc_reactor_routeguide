# Reactor implementation of gRPC clients

This is the reference for the client-side reactor library: the naming conventions, the delivery contracts, and a
class-by-class walkthrough of all four reactors with a sequence diagram each. It states what the code does.

For why the library is built this way, which alternatives were rejected, and the reasoning behind the hold rules,
see [architecture.md](/docs/architecture.md). For build and test commands, see
[developing.md](/docs/developing.md) and [testing.md](/docs/testing.md).

Two choices orient everything below, and the application makes both per RPC, from that RPC's constraints: neither
has a default. The `Delivery` template parameter decides how a reactor hands a received message over. Under `kPull`
the reactor keeps the message for the application to pull with `GetResponse()`; under `kPush` it gives the message
to a callback as an owned object, and keeps nothing. On the two reading reactors, the second choice is who arms the
next read: under `kPush` the `ReadPacing` constructor argument decides it for the whole RPC, and under `kPull` the
`read_ok` callback decides it for each message through its return value. See
[Delivery approaches](#delivery-approaches) and [Read pacing modes](#read-pacing-modes).

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
| `StartWrite()`          | Begins async write operation     | Inside `SendRequest()`                            |
| `StartWriteLast()`      | Write + implied close, one op    | Inside `SendLastRequest()`                        |
| `StartWritesDone()`     | Signals end of client writes     | Inside `CloseRequestStream()`                     |
| `AddHold()`             | Prevents OnDone until RemoveHold | Inside `OnReadDone()`, before the ok callback:    |
|                         |                                  | always under `kPull`, `kTurnByTurn` under `kPush` |
| `RemoveHold()`          | Releases hold, allows OnDone     | `GetResponse()` (`kPull`), `ResumeRead()`         |
|                         |                                  | (`kPush`), or `OnReadDone()` if `kPull` declines  |

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
  successful close ends the write side alone, which `writes_done_` already keeps shut, while the server may still
  be sending. Setting the flag there would stop a held read from ever being re-armed, and the RPC would never end.
- Checked before issuing a new `StartRead()`, `StartWrite()`, `StartWriteLast()`, or `StartWritesDone()`. On
  `ActiveBidiReactor`'s write path the check happens after the hold claim is won rather than before, see below.
- On the read side, only `GetResponse()` (`Delivery::kPull`) and `ResumeRead()` (`Delivery::kPush`, under
  `ReadPacing::kTurnByTurn`) consult it, since they are the only calls that arm a read from outside a reaction. A
  read-only reactor cannot in practice reach it with a hold outstanding, because the events that set the flag all
  require an operation the hold has already prevented from being armed. `ActiveBidiReactor` can, because a write
  failing while a read hold stands sets the same flag.
- `ActiveBidiReactor` uses a single flag for both directions instead of one per direction. Per gRPC's own
  contract (`grpcpp/support/client_callback.h`), a failure on either read or write means no new read/write
  operation will succeed, so tracking the two directions separately would not add information.
- `OnWritesDoneDone()` fires only for an explicit `StartWritesDone()` (i.e. `CloseRequestStream()`), not for a
  close implied via `StartWriteLast()` (i.e. `SendLastRequest()`). This is per gRPC's own documented distinction.
- Checking the flag narrows, but does not close, a race with a concurrent `OnDone()`, because the flag can flip to
  true right after a call already read it. `ActiveBidiReactor`'s write path stops relying on the check for safety:
  it claims the idle-window hold first, then re-reads the flag with that hold outstanding, so the read can no
  longer go stale in a way that matters. See [Write-side idle hold](#write-side-idle-hold).
  `ActiveWriteReactor` still relies on the check alone, and the residual race is the last row of
  [Hold requirements](#hold-requirements). Its remaining `UseMultipleHolds()` sketch describes an
  approach that has since been prototyped and rejected, recorded in
  [architecture.md](/docs/architecture.md#why-the-write-side-of-activewritereactor-has-no-hold).

### Application-facing API

These methods are exposed to application code with naming that reflects application-level semantics:

| Method                 | Purpose                                      | Notes                                        |
|------------------------|----------------------------------------------|----------------------------------------------|
| `SendRequest()`        | Send a request message on the stream         | Takes ownership; caller gives up the request |
| `SendLastRequest()`    | Send the final request and close, atomically | For a known-last message                     |
| `CloseRequestStream()` | Signal end of client requests                | Returns false if rejected (see below)        |
| `GetResponse()`        | Pull the received message out, by swap       | `Delivery::kPull` only, see below            |
| `ResumeRead()`         | Arm the read following a delivered message   | `Delivery::kPush`, `kTurnByTurn` only        |
| `TryCancel()`          | Cancel the entire RPC                        | Thread-safe, any thread                      |
| `Status()`             | Get final RPC status                         | Valid after `OnDone`                         |

`CloseRequestStream()` returns `false` (does nothing) if already closed, if a write is still in flight, or if the
RPC has already failed/finished. Callers should wait for `OnWriteDone()` and retry, or use `SendLastRequest()`
instead when the last message is known in advance.

`GetResponse()` exists on all four reactors under `Delivery::kPull`, and does not compile under `kPush`. It swaps
the held message out into the caller's object and returns `false` (does nothing) when no message is ready. On the
two reading reactors it also arms the next read and releases the hold `OnReadDone()` took. See
[Delivery approaches](#delivery-approaches).

`ResumeRead()` exists on the two reading reactors under `Delivery::kPush`, and does not compile under `kPull`. It
returns `false` (does nothing) unless a `ReadPacing::kTurnByTurn` hold is outstanding, which makes it a rejected
no-op under `ReadPacing::kContinuous` and on a duplicate call. See [Read pacing modes](#read-pacing-modes).

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
|                   |                 |                        | `GetResponse()`   | owned message     |               |
| **Client-Stream** | `SendRequest()` | `CloseRequestStream()` | done callback,    | done callback,    | `TryCancel()` |
|                   |                 |                        | `GetResponse()`   | owned response    |               |
| **Bidi**          | `SendRequest()` | `CloseRequestStream()` | read_ok callback, | read_ok callback, | `TryCancel()` |
|                   |                 |                        | `GetResponse()`   | owned message     |               |

Every reactor exposes `Status()` under both approaches.

#### Server-side reactors (future)

| RPC Type          | Receive        | Send               | Close Stream            | Finish           |
|-------------------|----------------|--------------------|-------------------------|------------------|
| **Unary**         | (params)       | (via `Finish()`)   | N/A                     | `Finish(status)` |
| **Server-Stream** | (params)       | `SendResponse()`   | `CloseResponseStream()` | `Finish(status)` |
| **Client-Stream** | `GetRequest()` | (via `Finish()`)   | N/A                     | `Finish(status)` |
| **Bidi**          | `GetRequest()` | `SendResponse()`   | `CloseResponseStream()` | `Finish(status)` |

### Bidirectional stream message flow

`GetResponse()` applies under `Delivery::kPull`; under `kPush` the `read_ok` callback is given the message instead.

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
application's.

## Delivery approaches

Every reactor takes a `Delivery` template parameter, which decides how a received message reaches the application.
It has no default, because neither approach is better in general: each one fits some constraints and not others.
The application states its choice per RPC, so both can live side by side in one application, even on the same RPC
method.

| Property                  | `Delivery::kPull`                            | `Delivery::kPush`                         |
|---------------------------|----------------------------------------------|-------------------------------------------|
| Where the message waits   | In the reactor, until `GetResponse()`        | In the callback, as an owned `unique_ptr` |
| What an event carries     | The reactor pointer                          | The message itself                        |
| Who paces a reading RPC   | The `read_ok` return value, per message      | The `ReadPacing` argument, per RPC        |
| Response accessor         | `GetResponse()`                              | None                                      |
| Read holds                | One per kept message, until `GetResponse()`  | One per message under `kTurnByTurn` only  |
| What the application owes | One `GetResponse()` per message it kept      | The obligations under                     |
|                           |                                              | [Message ownership](#message-ownership)   |

Only the slot that carries a message changes signature between the two approaches. The slot names, the other
callbacks, the write side, `Status()` and `TryCancel()` are identical. What each approach costs, and which one suits
which RPC, is argued in [architecture.md](/docs/architecture.md#why-the-reactors-offer-two-delivery-approaches). For
a guided first look at both on a unary call, see [walkthrough.md](/docs/walkthrough.md).

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

None of these apply under `Delivery::kPull`, where the event carries the reactor pointer and the message stays in
the reactor until `GetResponse()` swaps it out.

Two execution strategies are available to a consumer, under either approach. A gRPC-thread callback
(`OnDoneCallback`, `OnReadDoneOkCallback`) can decide whether to hand the message on to the application thread or
drop it: under `kPush` that costs nothing more than dropping the `std::unique_ptr`, and under `kPull` a reading
reactor's callback declines it by returning `false`. A handler registered through `EventLoop::RegisterEvent()` then
does the real processing on the application thread. Processing inside the gRPC-thread callback is discouraged,
because it occupies a thread gRPC needs for other RPCs.

## Read pacing modes

Under `Delivery::kPush`, `ActiveReadReactor` and `ActiveBidiReactor` take a `ReadPacing` mode at construction,
fixed for the whole RPC, and with no default for the same reason `Delivery` has none. The mode selects when the
reactor arms the read that follows a delivered message, and nothing else. Both modes move an owned
`std::unique_ptr<ResponseT>` into the `read_ok` slot, so the callback signature and the surrounding application
wiring are identical under either one, and the only difference a consumer writes is whether it calls `ResumeRead()`
when it is done with the message.

| Property           | `kContinuous`                                 | `kTurnByTurn`                                |
|--------------------|-----------------------------------------------|----------------------------------------------|
| Arms the next read | The reactor, as the reaction's last statement | The application, through `ResumeRead()`      |
| Holds the RPC      | No                                            | Yes, one hold per delivered message          |
| Messages in flight | Unbounded                                     | One                                          |
| A slow consumer    | Never stalls the stream                       | Stalls the stream while it is slow           |
| The cost           | The event queue grows without bound           | The RPC stalls, and strands if never resumed |

The mode is a choice of where the backpressure goes; which mode suits which RPC is argued in
[architecture.md](/docs/architecture.md#why-the-reading-reactors-offer-two-pacing-modes).

The mode is fixed at construction rather than switchable, which keeps it out of the race between the application
thread and `OnReadDone()`. The branch costs one read per network message.

`Delivery::kPull` takes no `ReadPacing`, because its `read_ok` callback makes the same choice for each message
through its return value. Returning `false` behaves like `kContinuous` for that message: the reactor reads on at
once, so the callback must copy out what it wants first. Returning `true` behaves like `kTurnByTurn`: the RPC
holds until the application calls `GetResponse()`, which plays the role `ResumeRead()` plays under `kPush`.

The re-arm sits outside the `read_ok` guard under both modes. An unbound slot has nobody to call `ResumeRead()`,
so the reactor arms the read itself rather than take a hold that nothing would ever release.

### The obligation kTurnByTurn adds

Under `kTurnByTurn`, an application that drops a message without calling `ResumeRead()` strands the RPC. The hold
suppresses `OnDone()`, and no outstanding read is left to report the stream ending, so nothing can conclude the
RPC and the reactor can never be legally destroyed. `kContinuous` carries no such obligation. The same obligation
applies under `Delivery::kPull` to every message the `read_ok` callback kept: until `GetResponse()` pulls it, the
RPC stays on hold.

`ResumeRead()` claims an internal flag before releasing anything, so a duplicate or spurious call is rejected
rather than acted on. Releasing a hold that was never taken would drop gRPC's outstanding-callback count below
what the RPC actually has in flight, and conclude the RPC early.

## Write-side idle hold

`ActiveBidiReactor` keeps one hold outstanding exactly while its write stream is idle, and claiming that hold is
what grants permission to write. `ActiveWriteReactor` has no equivalent, so the two classes differ here rather
than mirroring each other.

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
stops a doomed operation from being issued at all. Issuing a write into a terminating RPC is safe but pointless,
and gRPC may never complete it, which would hold the outstanding-callback count above zero.

A caller that wants to retry after a lost claim waits for `OnWriteDone()`. A caller refused by the re-check must
not wait, because no write is in flight and no `OnWriteDone()` will arrive. The bare `false` does not distinguish
the two, so an application that needs to tell them apart has to track the RPC's end through `read_nok` or `done`.

The release in `OnReadDone(false)` is what keeps an idle write side from stalling the RPC. Per gRPC's contract a
failure on either direction means neither succeeds any more, so the end of the read stream is a reliable signal
that no application write action will ever arrive. Both the arming and the releasing side publish their flag
before testing the other's, because gRPC may run the two reactions concurrently.

Under `ReadPacing::kTurnByTurn` that trigger is not unconditional. Between a delivered message and the matching
`ResumeRead()` no read is armed, so nothing reports the RPC ending and the idle write hold waits on the same
`ResumeRead()` the application already owes. The obligation described under
[The obligation kTurnByTurn adds](#the-obligation-kturnbyturn-adds) therefore covers both holds, not just the read
one. Under `kContinuous` a read is always armed, so the trigger always arrives.

`Delivery::kPull` behaves the same way, message by message. While a message the `read_ok` callback kept waits for
`GetResponse()`, no read is armed, so the idle write hold waits on that same `GetResponse()`, which the application
already owes. A declined message has its read re-armed inside the reaction, as under `kContinuous`.

## Hold requirements

gRPC's `AddHold()`/`RemoveHold()` keeps `OnDone()` from concluding an RPC while an operation on its state is still
being issued. One rule decides where the library needs one:

> An operation issued from inside a reaction needs no hold. One issued from outside a reaction needs one.

Applied per direction, the rule accounts for every gRPC call the library makes, including the one defect:

| Operation                            | Issued by                           | Hold                     |
|--------------------------------------|-------------------------------------|--------------------------|
| `StartRead()`, `kPush` `kContinuous` | The reactor, in `OnReadDone()`      | Not needed               |
| `StartRead()`, `kPush` `kTurnByTurn` | The application, in `ResumeRead()`  | Required, and taken      |
| `StartRead()`, `kPull`, declined     | The reactor, in `OnReadDone()`      | Not needed, taken anyway |
| `StartRead()`, `kPull`, kept         | The application, in `GetResponse()` | Required, and taken      |
| `StartWrite()`, bidi                 | The application, in `SendRequest()` | Required, and taken      |
| `StartWrite()`, write reactor        | The application, in `SendRequest()` | Required, and missing    |

Under `kPull` the hold is taken before the `read_ok` callback runs, so before the reactor knows whether the
message is kept or declined; a declined message has its hold released in the same reaction, right after
`OnReadDone()` re-arms the read itself.

`ActiveBidiReactor` takes its write-side hold for the window where the write stream sits idle, so claiming that
hold is what grants permission to write, under either `Delivery`. `ActiveWriteReactor` still guards itself with
`stream_no_more_` only, described under [Stream completion tracking](#stream-completion-tracking), which narrows the
window without closing it: it has no read stream, so it has no reliable trigger to release an idle hold on, and the
last row stays a residual race. The reasoning behind every row is in
[architecture.md](/docs/architecture.md#why-one-rule-decides-every-hold).

`Status()` and `TryCancel()` are safe from any thread at any time, because neither touches gRPC's internal
callback object: `Status()` reads a member of the reactor, and `TryCancel()` goes through `ClientContext`.

### Why OnReadDone holds before returning

When a read hold is outstanding, the application thread calls `StartRead()` later, from `GetResponse()` under
`kPull` or from `ResumeRead()` under `kPush`, not inside the gRPC callback. Between the end of the reaction and that
later `StartRead()` call, a concurrent `OnDone()` could still arrive and destroy the reactor's underlying gRPC-bound
callback state. Calling `StartRead()` at that point would forward through that now-destroyed object and segfault.
Other reactor operations, `Status()` and `TryCancel()`, do not touch gRPC's internal object at all, so they remain
safe to call regardless of hold state; only the deferred `StartRead()` needs the hold's protection. `AddHold()`
prevents `OnDone()` from firing during this window, and the `RemoveHold()` in `GetResponse()` or `ResumeRead()`
releases it once the new `StartRead()` is issued (or skipped, if the stream is done). This is the gap gRPC's own
callback API leaves open by design, and why it added the [hold mechanism][grpc-hold-pr].

### Hold before handing off, not after

`OnReadDone()` takes the hold before it calls the `OnReadDoneOkCallback`, never once the callback has returned:
always under `kPull`, and with `kTurnByTurn` pacing under `kPush`. The callback is what hands the message off to the
application thread (e.g. `EventLoop::TriggerEvent()`), so from that moment the application thread may already run
`GetResponse()` or `ResumeRead()`, and its `StartRead()` and `RemoveHold()`, before the gRPC thread is back from the
callback. gRPC's hold count is a plain counter, not a lock: a `RemoveHold()` that lands before its `AddHold()` does
not fail, it releases a hold that is not there yet. On the last message of a stream, that early release can bring
the RPC's outstanding-operation count to zero, so gRPC finishes the call and calls `OnDone()`, and the late
`AddHold()` and its own release then finish the same call a second time. TSan reports that as a data race inside
`ClientCallbackReaderImpl::MaybeFinish`, and UBSan as a call through a freed object.

So the order is: hold, then hand off. Publishing the flag the releasing call checks counts as handing off too:
`OnReadDone()` sets `response_ready_` (checked by `GetResponse()`) or `read_held_` (claimed by `ResumeRead()`) only
once the hold is taken. When a `kPull` callback declines the message (returns `false`), `OnReadDone()` restarts
reading itself and then releases the hold it took, since no `GetResponse()` will.

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
avoid a deep-copy of the content of the response. For the `ActiveUnaryReactor`, having the response swapped is acceptable
because the unary RPC is meant to one response only, so the swap is a good technique to speed up the response proceeding
time. The function will return true when the returned response is valid, which requires the RPC to have completed
with an OK status: after a failed RPC it returns false and leaves `response` untouched. Under `Delivery::kPush` there
is no such
accessor: the response has already left the reactor through the `OnDoneCallback`.

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
[scenarios](/applications/reactor/scenarios), one class per reactor variant.

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

The `OnReadDoneOkCallback` is the one slot whose signature depends on the `Delivery` the reactor was instantiated
with.

Under `Delivery::kPull`:

````cpp
using OnReadDoneOkCallback = std::function<bool(grpc::ClientReadReactor<ResponseT>* reactor, const ResponseT&)>;
````

When `ClientReadReactor::OnReadDone` event with a positive OK is handled in the active reactor, the
`OnReadDoneOkCallback` callback function is called with the following argument:

- the pointer to the active reactor instance (i.e. `this`)
- a reference to the `Response` content

For the sake of the gRPC processing, it is strongly discouraged to process the response during that callback event. It
is provided to ease some early-reaction logics or to select situations where the response handling is worthy or not.
One difference is that callback requires a return value: a boolean flag.

When it returns:

- true, the active reactor keeps the RPC on hold (the hold is already taken before the callback is called, see
"Hold before handing off, not after") and no more concurrent gRPC events are possible. That way, it is
thread-safe to access the reactor from a different processing thread without concurrent events (e.g. RPC termination).
Once the proceeding of the response is done, the RPC must be signaled to start a new read and then the hold can be
removed, which is what `GetResponse()` does.
- false, the active reactor will immediately start a new response reading without waiting, reusing the same response
storage for that next read, then release the hold it took before the callback. Any data the callback wants to keep
must be extracted (copied or moved out) before the callback returns, since the next incoming response overwrites that
storage as soon as reading resumes. That case can be useful when the response is copied onto a queue for later,
application-thread processing, or when it is discarded outright. That extraction must stay minimal, since it still
executes on the gRPC thread; the response's actual processing should happen afterward, on the application thread.

Under `Delivery::kPush`:

````cpp
using OnReadDoneOkCallback =
    std::function<void(grpc::ClientReadReactor<ResponseT>* reactor, std::unique_ptr<ResponseT>)>;
````

When `ClientReadReactor::OnReadDone` event with a positive OK is handled in the active reactor, the
`OnReadDoneOkCallback` callback function is called with the following argument:

- the pointer to the active reactor instance (i.e. `this`)
- the received message, whose ownership transfers to the callback

The ownership transfer distinguishes that callback from the two others. The reactor moves the message out of its
read target before the call, so it never refers to that message again. Which side arms the read that follows
depends on the `ReadPacing` mode the reactor was constructed with, described under
[Read pacing modes](#read-pacing-modes).

Processing the message inside the callback stays discouraged, because the callback runs on a gRPC thread. The
callback exists to hand the message to the application thread, or to discard it: dropping the `std::unique_ptr` is
all a discard takes.

#### Class functions

Four public functions can be called by the application side:

- `bool GetResponse(ResponseT& response)`, under `Delivery::kPull` only
- `bool ResumeRead()`, under `Delivery::kPush` only
- `const grpc::Status& Status()`
- `void TryCancel()`

`GetResponse()` function does two important things: it swaps the underlying data storage of the response variable and
then (if the stream allows it) it triggers immediately a new read on the stream and resume the RPC. The swap mechanism
is important to avoid a deep-copy of the content of the response. For the `ActiveReadReactor`, the content of that
response is not valuable because it overwrites it on each reading, so the swap is a good technique to speed up the
response proceeding time. The function will return true when the returned response is valid. It is the call that
releases the hold `OnReadDone()` took for a message the `OnReadDoneOkCallback` kept.

Under `Delivery::kPush` the reactor exposes no response accessor. Its internal `response_` read target keeps a stable
address across every read, and each message is swapped out of it by pointer, without a deep-copy, before the next read
is armed.

`ResumeRead()` arms the read that `OnReadDone()` left unarmed under `ReadPacing::kTurnByTurn`, and releases the hold
it took. It returns `false` without acting when there is no such hold outstanding, which covers a reactor built
in `ReadPacing::kContinuous` mode and a duplicate call in either mode.

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
per `Delivery` approach. Each fills a callback structure with the related bound functions. In these examples, the
callbacks are bound to the eventloop notification function as an `kListFeaturesOnReadDoneOk`,
`kListFeaturesOnReadDoneNOk`, or `kListFeaturesOnDone` event.

Under `Delivery::kPull`, the implementation of the `OnReadDoneOkCallback` is different, because it requires a return
value. From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.5 : the `cbs.read_ok = [](auto* reactor, const ResponseT&) -> bool {...}` lines
- 4.3 : the `cbs.read_nok = [](auto* reactor) {...}` lines
- 4.6 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void ListFeatures(routeguide::Rectangle rect) {
  using routeguide::ListFeatures::Delivery;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull>;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull>;
  Callbacks cbs;
  cbs.read_ok = [](auto* reactor, const routeguide::Feature&) -> bool {
    EventLoop::TriggerEvent(kListFeaturesOnReadDoneOk, reactor);  // Signal OnReadDoneOkCallback from gRPC thread
    return true;  // true: hold the RPC until the application proceeded the response
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

Under `Delivery::kPush`, the implementation of the `OnReadDoneOkCallback` is different, because it receives the
message as an owned object and releases that ownership into the event queue. The two other callbacks pass the reactor
pointer instead. The constructor also takes the `ReadPacing` mode, here `kContinuous`. From the PlantUML sequence
diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.5 : the `cbs.read_ok = [](auto*, std::unique_ptr<ResponseT> response) {...}` lines
- 4.3 : the `cbs.read_nok = [](auto* reactor) {...}` lines
- 4.6 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void ListFeatures(routeguide::Rectangle rect) {
  using routeguide::ListFeatures::Delivery;
  using routeguide::ListFeatures::ReadPacing;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPush>;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPush>;
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
  list_features_ =
      std::make_unique<ClientReactor>(*stub_, CreateClientContext(), rect, std::move(cbs), ReadPacing::kContinuous);
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

Under `Delivery::kPull`, the main goal of that callback is to read the response from the active reactor and starting a
new read operation on the stream. From the PlantUML sequence diagram, the corresponding points are from 2.8 to 2.11.

````cpp
EventLoop::RegisterEvent(kListFeaturesOnReadDoneOk, [this](const Event*) {
  routeguide::Feature response;
  bool valid = list_features_->GetResponse(response);  // when called, a new stream reading is automatically triggered
  /**  proceeding of the content of response **/
});
````

Under `Delivery::kPush`, the main goal of that callback is to reclaim the ownership of the message the gRPC-thread
callback released into the queue, and then to process it. The handler needs no access to the reactor, because there is
no response left to extract from it and no read to restart: the reactor already armed its next read before this
handler ran.

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

#### Delivery::kPull

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for server-side stream RPC, Delivery::kPull

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
alt Turn-by-turn | onreaddoneok callback returns true
group #lightgreen (grpc-thread callback) onreaddoneok
    reactor [#darkgreen]-\ grpc : AddHold()
    {start1} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk
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
    reactor [#darkgreen]-\ grpc : AddHold()
    data o-[#darkblue]> reactor : <i>extracts response
    deactivate data
    {start21} app /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n + response
    data o-[#darkgreen]->> reactor : Response holder
    & reactor -[#darkgreen]\ grpc : StartRead()
    reactor [#darkgreen]-\ grpc : RemoveHold()
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

#### Delivery::kPush

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for server-side stream RPC, Delivery::kPush

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
alt ReadPacing::kContinuous
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
else ReadPacing::kTurnByTurn
    activate reactor #gold
group #lightgreen (grpc-thread callback) onreaddoneok
    reactor <[#darkgreen]- grpc : OnReadDone : true
    reactor -[#darkgreen]> reactor : <i>extracts response\ninto owned message
    reactor -[#darkgreen]\ grpc : AddHold()
    {start2} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ owned message
    activate equeue #blue
    deactivate reactor
end
    deactivate grpc
    &grpc -> grpc : holding RPC\n(no read armed)
    activate grpc
group #lightblue (app-thread callback) onreaddoneok
    {end2} equeue -[#darkblue]> app : ProceedEvent: OnReadDoneOk\n+ owned message
    {start2} <-> {end2} : Eventloop
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

- `bool SendRequest(RequestT&&)`
- `bool SendLastRequest(RequestT&&)`
- `bool CloseRequestStream()`
- `bool GetResponse(ResponseT& response)`, under `Delivery::kPull` only
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

`GetResponse()`, `Status()` and `TryCancel()` behave as described for the unary reactor.

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
- 2.6 : the `cbs.write_done = [](auto* reactor, bool) {...}` lines
- 4.4 : the `cbs.done = [](auto*, const grpc::Status&, std::unique_ptr<ResponseT> response) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void RecordRoute(std::vector<routeguide::Point> points) {
  using routeguide::RecordRoute::Delivery;
  using Callbacks = routeguide::RecordRoute::Callbacks<Delivery::kPush>;
  using ClientReactor = routeguide::RecordRoute::ClientReactor<Delivery::kPush>;
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

From the PlantUML sequence diagram, the corresponding points are 2.1 and 2.8.

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

Under `Delivery::kPull`, the handler pulls the terminal response out of the reactor:

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

From the PlantUML sequence diagram, the corresponding points are from 4.5 to 4.7.

````cpp
EventLoop::RegisterEvent(kRecordRouteOnDone, [this](const Event* event) {
  const std::unique_ptr<routeguide::RouteSummary> response{
      static_cast<routeguide::RouteSummary*>(event->getData())};
  if (reactor_->Status().ok()) {
    /**  proceeding of the content of response **/
  }
  reactor_.reset();
});
````

### PlantUML sequence flow

The diagram shows `Delivery::kPush`. Under `Delivery::kPull`, only section 4 differs: the `OnDone` event carries the
reactor pointer instead of the owned response, and the app-thread handler pulls the response through
`GetResponse()` before destroying the reactor, as in the unary `kPull` diagram.

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
// Delivery::kPull
using OnReadDoneOkCallback =
    std::function<bool(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, const ResponseT&)>;
// Delivery::kPush
using OnReadDoneOkCallback =
    std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, std::unique_ptr<ResponseT>)>;
// Both approaches
using OnReadDoneNOkCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor)>;
using OnWriteDoneCallback = std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, bool ok)>;
using OnDoneCallback =
    std::function<void(grpc::ClientBidiReactor<RequestT, ResponseT>* reactor, const grpc::Status&)>;
````

Under `Delivery::kPull`, each received message stays in the internal read target, and `OnReadDoneOkCallback` is lent
it: returning `true` keeps it there, holding the read side until `GetResponse()` pulls it, and returning `false`
declines it so the reactor reads on at once, exactly as on `ActiveReadReactor`.

Under `Delivery::kPush`, each received message is swapped out of the internal read target and handed to
`OnReadDoneOkCallback` as an owned object. Which side arms the read that follows depends on the `ReadPacing` mode the
reactor was constructed with, exactly as on `ActiveReadReactor`, and is described under
[Read pacing modes](#read-pacing-modes).
Processing the message inside that callback stays discouraged, because it runs on a gRPC thread: the callback
exists to hand the message to the application thread, or to discard it by dropping the `std::unique_ptr`.

`OnReadDoneNOkCallback` reports the end of the response stream. Unlike the server-streaming case, it also means
the RPC itself is ending, because a server closes a bidirectional response stream only by finishing the call.

#### Class functions

Seven public functions can be called by the application side:

- `bool SendRequest(RequestT&&)`
- `bool SendLastRequest(RequestT&&)`
- `bool CloseRequestStream()`
- `bool GetResponse(ResponseT& response)`, under `Delivery::kPull` only
- `bool ResumeRead()`, under `Delivery::kPush` only
- `const grpc::Status& Status()`
- `void TryCancel()`

`GetResponse()` and `ResumeRead()` behave exactly as on `ActiveReadReactor`. The three write functions differ from
`ActiveWriteReactor`'s: each one claims the write-side idle hold before it issues anything, and returns false when
that claim is lost, as described under [Write-side idle hold](#write-side-idle-hold). What a caller observes is
unchanged, since a lost claim covers the same three rejection cases the flag checks used to report. The write side is
identical under both `Delivery` approaches.

Under `Delivery::kPush`, like the three other reactors, `ActiveBidiReactor` exposes no response accessor: its
`response_` read target keeps a stable address across every read, and each message is swapped out of it by pointer,
without a deep-copy, before the next read is armed.

A read hold, whether kept for `GetResponse()` or taken under `ReadPacing::kTurnByTurn`, stalls only the read
direction. gRPC counts holds and outstanding operations on one per-RPC counter, but that counter gates `OnDone()`
alone, so the application can keep sending requests while it is behind on responses. The read hold and the
write-side idle hold can therefore both stand at once, and each is claimed and released independently of the other.

### Code snippet

#### Instantiation of the ActiveBidiReactor class

The following snippet instances an `ActiveBidiReactor` dedicated to the `RouteChat` RPC of the `routeguide` API,
under `Delivery::kPush` with `ReadPacing::kContinuous`. Only `read_ok` carries a message; the three other callbacks
pass the reactor pointer. Under `Delivery::kPull`, the type aliases name `Delivery::kPull`, the constructor takes no
`ReadPacing`, and `read_ok` passes the reactor pointer too, keeping the message for `GetResponse()`:

````cpp
cbs.read_ok = [](auto* reactor, const ResponseT&) -> bool {
  EventLoop::TriggerEvent(kRouteChatOnReadDoneOk, reactor);  // Signal OnReadDoneOkCallback from gRPC thread
  return true;  // true: hold the read side until the application pulled the response
};
````

From the PlantUML sequence diagram, the corresponding points are:

- 1.x : the `std::make_unique<ClientReactor>(...)` line
- 2.9 : the `cbs.write_done = [](auto* reactor, bool) {...}` lines
- 3.5 : the `cbs.read_ok = [](auto*, std::unique_ptr<ResponseT> response) {...}` lines
- 5.5 : the `cbs.read_nok = [](auto* reactor) {...}` lines
- 5.8 : the `cbs.done = [](auto* reactor, const grpc::Status&) {...}` lines

````cpp
#include "applications/reactor/reactor_client_routeguide.h"
void RouteChat(std::vector<routeguide::RouteNote> notes) {
  using routeguide::RouteChat::Delivery;
  using routeguide::RouteChat::ReadPacing;
  using Callbacks = routeguide::RouteChat::Callbacks<Delivery::kPush>;
  using ClientReactor = routeguide::RouteChat::ClientReactor<Delivery::kPush>;
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
  route_chat_ = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), std::move(cbs), ReadPacing::kContinuous);
  SendNextRouteChatNote();  // Kick the write flow with the first request
}
````

#### OnReadDoneOkCallback

Under `Delivery::kPull`, the handler pulls the message out of the reactor, which also re-arms the read and releases
the read hold:

````cpp
EventLoop::RegisterEvent(kRouteChatOnReadDoneOk, [this](const Event*) {
  routeguide::RouteNote response;
  bool valid = route_chat_->GetResponse(response);  // when called, a new stream reading is automatically triggered
  /**  proceeding of the content of response **/
});
````

Under `Delivery::kPush`, the handler reclaims the message the gRPC-thread callback released into the queue. It needs
no access to the reactor: there is no response left to extract and no read to restart, because the reactor already
armed its next read before this handler ran.

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

The diagram shows `Delivery::kPush`. Under `Delivery::kPull`, only section 3 differs, and it follows the
`ActiveReadReactor` `kPull` diagram: the hold is taken before the `read_ok` callback, the event carries the reactor
pointer, and the app-thread handler's `GetResponse()` re-arms the read and releases the hold. The write side keeps
running while that hold is outstanding.

```plantuml
!pragma teoz true
skinparam lifelineStrategy solid
skinparam ParticipantPadding 50

title gRPC Client reactor for bidirectional stream RPC, Delivery::kPush

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
alt ReadPacing::kContinuous
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
else ReadPacing::kTurnByTurn
    activate reactor #gold
group #lightgreen (grpc-thread callback) onreaddoneok
    reactor <[#darkgreen]- grpc : OnReadDone : true
    reactor -[#darkgreen]> reactor : <i>extracts response\ninto owned message
    reactor -[#darkgreen]\ grpc : AddHold()
    {start4} equeue /[#darkgreen]- reactor : TriggerEvent : OnReadDoneOk\n+ owned message
    activate equeue #blue
    deactivate reactor
end
    deactivate grpc
    &grpc -> grpc : holding the read side\n(no read armed)\nthe write side keeps running
    activate grpc
group #lightblue (app-thread callback) onreaddoneok
    {end4} equeue -[#darkblue]> app : ProceedEvent: OnReadDoneOk\n+ owned message
    {start4} <-> {end4} : Eventloop
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
