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

Every reactor takes a `Delivery` template parameter, with no default, that decides how a received message reaches
the application. Under `Delivery::kPull` the reactor keeps the message and lends it to the callback, and the
application pulls it out through `GetResponse()` once its event loop has dispatched the notification. Under
`Delivery::kPush` the reactor moves the message into the callback as an owned object, and keeps nothing:
`ActiveUnaryReactor` and `ActiveWriteReactor` move their single terminal response into the `done` callback, and
`ActiveReadReactor` and `ActiveBidiReactor` move each received message into the `read_ok` callback.

On the two reading reactors, what remains is who arms the read that follows. A second template parameter,
`ReadPacing`, answers it, independently of `Delivery`. Its modes sit on one scale of how much the stream waits for the
application, and `kPerMessage`, in the middle, is the default:

| `ReadPacing`  | Who arms the next read                                 | Hold                                       |
|---------------|--------------------------------------------------------|--------------------------------------------|
| `kContinuous` | The reactor, as the last action of the same reaction   | None: the next read cannot complete while  |
|               |                                                        | that reaction runs                         |
| `kPerMessage` | Either one, as the `read_ok` callback decides for each | One per message, whether kept or declined  |
|               | message                                                |                                            |
| `kTurnByTurn` | The application, through `GetResponse()` or            | One per message, which keeps the RPC alive |
|               | `ResumeRead()`, from outside any reaction              | across that gap                            |

Why these two choices, and why only these, follows from one model, described under
[The model behind the reactors' choices](#the-model-behind-the-reactors-choices).

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
| **Activation Queue** | EventLoop internal queue | Holds pending events, and the messages `kPush` reactors push |
| **Servant** | `EventLoop::RegisterEvent()` handlers | Processes RPC responses (application logic) |
| **Future** | `Status()`, and `GetResponse()` under `kPull` | Deferred result access |
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
[reactor_client.md](/applications/reactor/reactor_client.md#hold-requirements), and the reasoning behind each row is
under [Why one rule decides every hold](#why-one-rule-decides-every-hold).

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
application does. Under `Delivery::kPush`, all four reactors put ownership of the message itself into that
dispatch; under `Delivery::kPull`, the dispatch carries the reactor, and a held message waits in it to be pulled.

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
| The library | Two delivery approaches: a response pulled from the reactor, or pushed as an owned message |
| The library | Three read pacing modes on the reading reactors, combinable with either delivery approach |
| The library | Future-like access through `Status()`, and `GetResponse()` under `Delivery::kPull` |
| The application | Servant business logic, in `EventLoop::RegisterEvent()` handlers |
| The application | Domain processing that turns RPC responses into application state changes |

The demo application uses logging as placeholder Servant logic, where a production application would put real
work:

```cpp
// Demo: logging only
EventLoop::RegisterEvent(kGetFeatureOnDone, [](const Event* event) {
  logger.info("RESPONSE | {}", response);  // Placeholder Servant logic
});

// Production: real Servant logic, here under Delivery::kPush
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

### The model behind the reactors' choices

Every reactor moves messages in up to two directions, incoming and outgoing, and each direction carries either a
single message or a stream. Three rules decide which choices apply to a direction:

1. **Push or pull applies to every direction that carries a message.** It answers who starts each transfer between
   the reactor and the application. Incoming, that is `Delivery`: the reactor pushes the message into the callback
   (`kPush`), or the application pulls it with `GetResponse()` (`kPull`). Outgoing, the application pushes each request
   with `SendRequest()`, or the reactor could pull the next one from the application.
2. **Pacing applies only to an incoming stream.** Only there does the peer decide when messages arrive, so only there
   does the reactor have to decide whether the stream waits for the application. That decision is `ReadPacing`. An
   outgoing stream needs none: its messages exist only when the application produces them, so the producer already
   sets the pace, whichever side starts each transfer.
3. **A single message gets push or pull only.** A final response arrives once, together with `OnDone()`, so there is
   no next read to arm and nothing that could wait for the application.

| Reactor              | Incoming                                       | Outgoing                          |
|----------------------|------------------------------------------------|-----------------------------------|
| `ActiveUnaryReactor` | Final response: `Delivery`                     | None                              |
| `ActiveReadReactor`  | Stream of messages: `Delivery`, `ReadPacing`   | None                              |
| `ActiveWriteReactor` | Final response: `Delivery`                     | Stream of requests: push or pull  |
| `ActiveBidiReactor`  | Stream of messages: `Delivery`, `ReadPacing`   | Stream of requests: push or pull  |

Today the outgoing direction has one value: the application pushes. A reactor that pulls each request from the
application would need no write-side hold at all, since every write would then start inside a reaction.

The two directions name their values after who starts the transfer, and the words come out reversed. Incoming, the
application starts the transfer by pulling; outgoing, it starts it by pushing. So the outgoing axis cannot reuse
`kPull` and `kPush`, which would make one word mean opposite things in one reactor.

#### Where backpressure goes

gRPC carries backpressure from the reader to the writer by itself. A stream allows one write in flight, and HTTP/2
gives it a flow-control window. A reader that stops arming reads stops granting window, the writer's `OnWriteDone`
waits, and the writer cannot start another write. gRPC buffers about a window's worth per stream, never an unbounded
amount, and one slow stream stalls itself rather than its whole channel. The two choices then decide where the backlog
sits:

- **The reader's pacing decides whether backpressure reaches the writer at all.** Under `kContinuous` the reader keeps
  reading as fast as the network allows, so the writer feels nothing, and the backlog moves into the reader's own event
  queue. Under `kTurnByTurn`, or `kPerMessage` while a message is held, the reader stops reading, and the writer waits.
- **The writer's push or pull decides where its surplus waits.** When the application pushes, whatever it produces
  between two `OnWriteDone` waits in its own queue, which grows without limit if production outpaces the reader. When
  the reactor pulls, it asks the producer for the next message only once the previous one was accepted, so the
  backpressure reaches the production itself.

That is why no stream needs a write pacing: an outgoing stream's pace is set by its peer's reads.

#### The server side mirrors it

On the server, the incoming direction carries requests and the outgoing one carries responses, so the same three rules
give the mirror image of the client table. The single incoming message becomes an initial request, which arrives with
the call rather than with its end.

| Server reactor | Incoming                                     | Outgoing                             |
|----------------|----------------------------------------------|--------------------------------------|
| Unary          | Initial request: push or pull                | Final response, sent with `Finish()` |
| Read           | Stream of requests: push or pull, and pacing | Final response, sent with `Finish()` |
| Write          | Initial request: push or pull                | Stream of responses: push or pull    |
| Bidi           | Stream of requests: push or pull, and pacing | Stream of responses: push or pull    |

One difference makes the server simpler. gRPC's server reactors have no `AddHold()`: a server RPC cannot complete
before the server calls `Finish()`, and `OnDone()` comes only after it. An operation started from the application
thread is therefore safe until `Finish()`, with no hold, and the obligation the client writer carries, ending its write
side, is built into every server RPC as the obligation to finish it.

### Why the reactors offer two delivery approaches

How a message leaves a reactor and when the next read is armed are two independent questions, and the library
answers each with a choice rather than a fixed design. `Delivery` answers the first one, on every reactor.
`ReadPacing` answers the second, on the reading reactors only, and is argued in the next section. Both answers to
the delivery question are correct. They cost different things, and which cost an RPC can afford is a property of
that RPC, not of the library.

| Constraint                                       | Points to | Because                                         |
|--------------------------------------------------|-----------|-------------------------------------------------|
| The event queue cannot take owned payloads       | `kPull`   | The event carries the reactor: nothing leaks or |
| (shared event names, `Halt()` on exit)           |           | is freed twice                                  |
| Messages are consumed on the gRPC thread, inside | `kPull`   | The callback reads the message in place, with   |
| the reaction                                     |           | no allocation                                   |
| A message must outlive the next read, or be      | `kPush`   | Each message is its own object, not a read      |
| handed on to another component                   |           | target the next read overwrites                 |
| The application thread processes every message,  | `kPush`   | The message leaves the reactor at once, so no   |
| later and at its own pace                        |           | reactor keeps cross-thread response state       |

Pulling keeps the message in the reactor's read target, which costs nothing per message: the callback reads it in
place, and `GetResponse()` swaps it out without a copy. The price is that the message lives in a buffer the next
read overwrites. Either the stream holds until `GetResponse()`, which ties the read side to the consumer, or the
callback finishes with the message before its reaction ends. In exchange, the plain-pointer event payload keeps the
application free of ownership rules.

Pushing gives each message its own object and hands it over at once, so the message survives any later read, and no
reactor keeps cross-thread response state. The payload is owned, which brings the obligations listed under
[Message ownership](/applications/reactor/reactor_client.md#message-ownership).

Neither approach decides the backpressure. Whether the stream waits for its consumer is the `ReadPacing` choice,
and every pacing exists under both approaches.

The two coexist in one header, selected by a template parameter, so the shared parts (the write side, `Status()`,
`TryCancel()`, the hold rules) are written and fixed once. Each approach keeps its own hand-off step, which is what
lets either be read on its own.

#### Choosing for a single terminal response

A unary or client-streaming RPC delivers one terminal response, together with `OnDone()`. Pushing's main benefit,
a stream that never waits for its consumer, does not exist there, because the RPC is already over when the response
arrives. What remains is a single question: who owns the response while it waits in the event queue.

| Constraint                                          | Points to | Because                                      |
|-----------------------------------------------------|-----------|----------------------------------------------|
| The event queue cannot take owned payloads          | `kPull`   | The event carries the reactor, so nothing    |
| (shared event names, `Halt()` on exit, a handler    |           | can leak or be freed twice                   |
| that can throw before it reclaims)                  |           |                                              |
| The response must outlive the reactor, or be handed | `kPush`   | The handler already owns it, so it moves on  |
| on to another component                             |           | without a copy                               |
| A failed RPC must not look like a response          | `kPull`   | `GetResponse()` returns false unless the     |
|                                                     |           | status is OK                                 |
| The handler must always receive a message object    | `kPush`   | The pointer is never null, and a failed RPC  |
|                                                     |           | yields an empty message                      |
| Handlers may run on another thread than the one     | `kPull`   | The event carries the reactor, so a handler  |
| that creates the reactor                            |           | never depends on the slot being assigned yet |

Under `kPush` the handler still has to check `Status()` before trusting the content, since an empty message is also
what a failed RPC produces.

Reaching the reactor differs too. Under `kPull` it arrives as the event payload. Under `kPush` the handler must reach
it through the slot the application stored it in. That is safe only when the EventLoop runs on the thread that
assigned the slot, as it does in the demo. The specialized constructor calls `StartCall()`, so with the EventLoop on
another thread the handler can run before the constructing statement has assigned the slot. The integration tests in
[client_reactor_integration_test.cpp](/applications/reactor/tests/client_reactor_integration_test.cpp) run that
configuration, which is why they read `Status()` on the main thread only after the handler has signalled.

### Why the reading reactors offer three pacing modes

For a stream of messages, one question remains once delivery is chosen: who arms the next read, and therefore how
much the stream waits for the application, which is where the backpressure goes. The three modes answer it along one
scale, under either delivery approach:

```text
kContinuous  ─────────────  kPerMessage  ─────────────  kTurnByTurn
never waits                 waits when read_ok          always waits
                            says so (the default)
```

| `ReadPacing`  | Decided     | Who arms the next read                   | Holds per message     |
|---------------|-------------|------------------------------------------|-----------------------|
| `kContinuous` | Upfront     | The reactor                              | None                  |
| `kPerMessage` | Per message | Whichever one the `read_ok` answer names | One, kept or declined |
| `kTurnByTurn` | Upfront     | The application                          | One                   |

`ReadPacing::kPerMessage` is the general mode. The `read_ok` callback returns its decision, so each message is either
held as under `kTurnByTurn` or read past as under `kContinuous`, and the answer can depend on the message itself, or on
the consumer's state when the message arrives. It pays for that generality with a hold on every message, explained
under [Why kPerMessage takes one for every message](#why-kpermessage-takes-one-for-every-message), and with a rule
the application must follow: one release call per kept message, and none per declined one.

The two other modes are the two ends of the scale, fixed for the whole RPC. `ReadPacing::kContinuous` has the reactor
re-arm inside the reaction, so the stream never waits for the application. What it costs depends on the delivery
approach:

| Delivery | Cost                         | Cause                                   | Accepted because              |
|----------|------------------------------|-----------------------------------------|-------------------------------|
| `kPush`  | The queue is unbounded       | EventLoop has no backpressured delivery | The request bounds the volume |
| `kPush`  | `Halt()` leaks messages      | `Halt()` discards rather than drains    | The demo's loop never halts   |
| `kPull`  | Keeping a whole message      | The next read overwrites the lent       | The callback consumes the     |
|          | takes a deep copy            | message                                 | message in place instead      |

Neither push cost is acceptable for a stream whose volume the peer controls, which is what `ReadPacing::kTurnByTurn`
is for. There the application arms the next read through `GetResponse()` or `ResumeRead()`, so exactly one message
is ever in flight and the stream stalls for as long as the consumer is slow. The cost moves rather than
disappearing: an application that drops a held message without its release call strands the RPC, since the hold
suppresses `OnDone()` and no outstanding read is left to report the stream ending.

Before `ReadPacing` applied to both approaches, `kPull` had only a per-message `bool` from `read_ok`, and `kPush` had
only the two upfront modes. That `kPull` behaviour is the `kPull` and `kPerMessage` combination, so making the two axes
independent removed nothing. It added the three combinations that were missing: `kPull` with `kContinuous` or
`kTurnByTurn`, and `kPush` with `kPerMessage`. The one behaviour that changed is an unbound `read_ok` slot, for which
the reactor used to take a hold and give it back in the same reaction, and now takes none.

A fourth option, bounded buffering inside the reactor, was evaluated and rejected for an unresolved lost-wakeup
race.

#### Why kPerMessage is the default, and the two others still exist

`kPerMessage` can do everything the two fixed modes do. A callback that always returns `true` behaves as
`kTurnByTurn`, and one that always returns `false` behaves as `kContinuous`. That is why it is the default: an
application that names only its `Delivery` still gets every behaviour, decided one message at a time. `Delivery` has
no default for the opposite reason: neither approach contains the other, so each RPC has to pick one from its
constraints.

The two fixed modes are for a design that has already made that decision. They add no behaviour. What they add is
the decision fixed in the type, where the compiler enforces it and rules out the mistakes it leaves no room for:

| Guarantee                   | `kContinuous`         | `kPerMessage`                    | `kTurnByTurn`          |
|-----------------------------|-----------------------|----------------------------------|------------------------|
| Holds taken                 | None                  | One per message, declined or not | One per message        |
| Release call owed           | None, and none exists | One per kept message only        | One per message        |
| A handler bug can strand it | No                    | Yes: a missed release, or a      | Yes: a missed release  |
|                             |                       | `true` returned by mistake       |                        |
| A stray release can misfire | Cannot be written     | Yes, see below                   | No, while each handler |
|                             |                       |                                  | releases once          |

The stray release is the subtle one. `GetResponse()` and `ResumeRead()` release whichever message is held at that
moment, not a particular one. Under `kTurnByTurn` every message is held, so a handler that calls one of them once is
always right. Under `kPerMessage` the same handler is wrong for a declined message: by the time it runs, the next
message can already be held, while its own `read_ok` is still deciding, and the stray call releases it. The demo's
`kPush` and `kPerMessage` scenario was first written that way, and its log showed messages handled out of order and
four messages in flight against a bound of three. It now releases only from the handler of the message its `read_ok`
held.

So a fixed mode is chosen for what it rules out. `kContinuous` rules out holding, and with it every release call.
`kTurnByTurn` rules out deciding, and with it the difference between a kept message and a declined one.

#### Why the pacing is a template parameter

The pacing changes the `read_ok` callback's signature, which a runtime value cannot do. Under `kPerMessage` the
callback returns a decision, and under the two upfront modes there is no decision to return. A runtime pacing would
force one signature on all three, so every callback would return a `bool`, and the upfront modes would ignore it.
An ignored `bool` is a trap: a callback returning `false` under `kTurnByTurn` reads as "read on", while the reactor
holds anyway and waits for a release call the author never planned. As a template parameter, the pacing gives each
mode the signature that says what it does: `void` where nothing is decided, `bool` where something is.

The same choice turns meaningless calls into compile errors rather than runtime no-ops:

| Call            | Exists only where                            | Absent under `kContinuous` because      |
|-----------------|----------------------------------------------|-----------------------------------------|
| `GetResponse()` | `kPull`, with `kPerMessage` or `kTurnByTurn` | No message is ever held for it          |
| `ResumeRead()`  | `kPush`, with `kPerMessage` or `kTurnByTurn` | No hold is ever taken for it to release |

A pacing fixed in the type also stays out of the race between the application thread and `OnReadDone()`, since
nothing can change it while a reaction reads it.

#### Choosing for a stream of messages

A streaming RPC delivers many messages, so both choices apply, and the six combinations each fit a different kind
of consumer:

| Constraint                                     | Points to              | Because                                    |
|------------------------------------------------|------------------------|--------------------------------------------|
| Each message is consumed inside the reaction:  | `kPull`, `kContinuous` | The callback reads the lent message in     |
| an aggregate, a latest-value-wins snapshot, or |                        | place, with no allocation and no hold      |
| a filter that drops what it does not need      |                        |                                            |
| Messages are filtered on the gRPC thread: some | `kPull`, `kPerMessage` | A declined message costs no allocation and |
| kept for the application, the others declined  |                        | no event, and a kept one is swapped out    |
|                                                |                        | without a copy                             |
| One message in flight, with no ownership rules | `kPull`, `kTurnByTurn` | The stream waits for `GetResponse()`, and  |
| for the application                            |                        | the event carries only the reactor         |
| The consumer defers its processing, and the    | `kPush`, `kContinuous` | Each message leaves the reactor at once,   |
| stream must never wait for it                  |                        | at the cost of an unbounded queue          |
| The stream should run freely until the         | `kPush`, `kPerMessage` | The callback holds only while the backlog  |
| application falls behind                       |                        | is above a threshold: adaptive             |
|                                                |                        | backpressure                               |
| One message in flight, and each message must   | `kPush`, `kTurnByTurn` | The stream is bounded, and every message   |
| outlive the next read                          |                        | is its own object                          |

Under `kPull` with `kContinuous`, a callback that wants to keep a whole message pays a deep copy for it. Once most
messages are kept whole, `kPush` serves the same consumer without the copy.

### Why the generic layer offers every combination

`reactor_client.h` is the most generic layer of the library, and it offers every sound combination of its choices.
A more specific layer may restrict those choices, but only for a stated design reason, never for convenience. The
rule leaves each decision with the layer that has the information to make it, and it keeps a restriction from
outliving a reason nobody wrote down.

`reactor_client_routeguide.h` follows the rule by forwarding both axes untouched. It is pass-through code of the
kind a generator would produce from the service definition, and choosing a combination for an RPC is an
application decision, which that layer has no information to make. Its `GetFeature` and `RecordRoute` reactors take
no `ReadPacing`, and that is not a restriction: a single terminal response has no next read to arm, so the axis
does not apply to it.

### Why one rule decides every hold

A hold keeps `OnDone()` from concluding an RPC, and destroying its underlying gRPC-bound state, while an operation
on that state is still being issued. One rule decides where the library needs one:

> An operation issued from inside a reaction needs no hold. One issued from outside a reaction needs one.

A reaction in progress is itself proof that the gRPC-bound state still exists, which is exactly the guarantee a
hold buys. Which operation falls on which side is tabulated in
[reactor_client.md](/applications/reactor/reactor_client.md#hold-requirements); the subsections below are the
reasoning behind each row.

The rule says a hold is needed, not when it must be taken. Those are separate questions, and conflating them is
what made the write side look unsolvable for as long as it did. A hold protecting an application-thread call has to
be reserved earlier, either before `StartCall()` or from inside some reaction, because taking one needs the state
whose liveness is in question. What decides whether a design works is therefore not the operation it protects but
the trigger that releases it.

### Why kContinuous needs no hold

The first `StartRead()` is issued by the specialized constructor, before `StartCall()`, so the RPC has not started
yet and no `OnDone()` can be in flight. Every later one is issued as the last statement of `OnReadDone(true)`,
from inside the reaction, so the read path obtains the guarantee structurally rather than by asking for it. That
holds under either delivery approach.

Re-arming after the callback also bounds the reaction to one at a time, since a read cannot complete before it is
started. Messages therefore reach the event loop in network order. Under `kPull` it is also what makes the lent
message safe to read: the read that overwrites it is not armed until the callback has returned.

`kContinuous` is the only pacing that never takes a read hold, and it can skip it because the decision is made
upfront. The reactor knows before calling `read_ok` that it will re-arm the read itself, inside the reaction, so no
operation ever crosses the reaction boundary.

### Why kPerMessage takes one for every message

A message the `read_ok` callback keeps leaves the re-arm to `GetResponse()` or `ResumeRead()`. They run on the
application thread, after `OnReadDone(true)` has already returned. That is precisely the window a hold exists for: with
no read armed and no reaction in progress, gRPC's outstanding-callback count can otherwise reach zero, fire `OnDone()`,
and invalidate the state `StartRead()` is about to go through.

The ordering within the pair is what makes it correct:

- `AddHold()` runs before the `read_ok` callback, because that callback may release inline, or hand the message to
  a thread that releases before the reaction returns.
- `StartRead()` runs before `RemoveHold()`, because `StartRead()` raises the count first, so the count never
  reaches zero between the two calls.
- `RemoveHold()` runs whether or not a read was armed, because gRPC requires exactly one release per hold, and a
  hold left outstanding stalls the RPC forever.

The reactor learns whether a message is kept only when `read_ok` returns, but the hold has to exist before
`read_ok` runs. The callback is the hand-off: by the time it returns, it may already have passed the message to the
application thread, which may already have released the hold. A hold taken after the callback could land after its
own release, which is the failure described under
[Hold before handing off, not after](/applications/reactor/reactor_client.md#hold-before-handing-off-not-after).
So the reactor takes a hold for every message, and gives it back in the same reaction when the callback declines.
The declined path needs no hold in principle and costs one in practice: an `AddHold()`, a compare-exchange, and a
`RemoveHold()` per declined message. The upfront modes do not pay that, because neither waits on a decision it
does not make.

The hold is claimed back rather than cleared by a plain store, because the callback may hand a message off and
still decline it. The application thread can then claim the same hold through its release call. Whichever side wins
the claim releases the hold, and the other does nothing, so the hold is released exactly once even when the
application acts against the callback's answer. `GetResponse()` claims `response_ready_` the same way `ResumeRead()`
claims `read_held_`, so both delivery approaches share one protocol.

What the reactor publishes is a per-message token rather than a boolean, because a boolean claim cannot tell two
messages apart. Once the application has released message N, the next read can complete, and message N+1 can be
held and published on another gRPC thread while the callback for N is still deciding. If N is then declined, a
boolean claim would find the flag set and take the hold of N+1, which the application still owes a release for.
The decline instead compares against the token of N, so it can only ever take back its own hold, and a lost claim
simply means that hold is already gone. The release calls need no such match: each claims whichever message is held
at that moment, by exchanging its token for zero.

### Why kTurnByTurn needs one

`kTurnByTurn` is the kept branch of `kPerMessage`, taken for every message, so it needs the hold for the same reason,
with the same orderings. It skips only what the decision costs: no hold is ever given back inside the reaction, so
nothing has to be claimed back against a decline. The release calls still claim the token, which is what keeps a
duplicate call from releasing a hold twice.

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
direction failed. Under `ReadPacing::kContinuous` a read is always armed, from the constructor onward, so the
signal always arrives. Under `kTurnByTurn` it does not: between a delivered message and its release call nothing is
armed, so the idle write hold waits on the same `GetResponse()` or `ResumeRead()` the application already owes for
the read hold. That obligation now covers both holds rather than one. `kPerMessage` follows the same pattern message
by message: a kept message leaves no read armed until its release call, which the application owes anyway, and a
declined one re-arms inside the reaction, as under `kContinuous`. None of this depends on the delivery approach.

Two reactions can race to change the hold's state, since gRPC may run `OnWriteDone()` and `OnReadDone(false)`
concurrently. Both sides therefore publish their own flag before testing the other's, which guarantees at least one
of them observes the other and exactly one of them releases. That symmetry is the reason neither atomic may be
weakened to a relaxed ordering.

### Why ActiveWriteReactor holds its whole write flow

`ActiveWriteReactor` cannot use the design above, because the release trigger it depends on does not exist there.
A client-streaming RPC has no read stream, so an RPC ending without an application write action, a deadline
expiring or the server cancelling, produces no reaction that could free an idle hold.

For a long time the writer therefore kept only the `stream_no_more_` flag test, documented as a residual race that the
test narrowed. It narrowed less than it seemed. gRPC destroys a call as soon as nothing is pending on it, and calls
`OnDone()` only afterwards, so the flag, which `OnDone()` sets, is still clear while the call is already gone:

```cpp
// grpcpp/support/client_callback.h, ClientCallbackWriterImpl::MaybeFinish()
this->~ClientCallbackWriterImpl();
grpc_call_unref(call);
reactor->OnDone(s);
```

That window opens in practice. With the server ending the RPC while the writer is idle, `OnDone()` arrived within
milliseconds, while the application could still issue a write into the destroyed call. This is not a defect in gRPC
but its documented contract. Its `AddHold()` documentation states that holds are needed if, and only if, operations
take place after `StartCall()` from outside the reactions, and prescribes the pattern for a write flow: a hold taken
before `StartCall()`, and released after `StartWriteLast()` or `StartWritesDone()`, once no more writes will be issued.

The writer now follows that pattern. Its hold covers the whole write flow, and is released when the write side ends:
by `SendLastRequest()` or `CloseRequestStream()`, as gRPC prescribes, and also by a write that fails, since no write
can succeed after it, and by `TryCancel()`, whose purpose is to provoke `OnDone()`. A write call claims the flow before
it writes, exactly as a bidi write claims the idle hold, so a release from a reaction or from another thread can never
land between its guards and its `StartWrite()`. That claim is what makes reaction-side releases safe here, where an
earlier, unclaimed design had reopened the race.

The cost is the one gRPC's pattern carries. A whole-flow hold suppresses `OnDone()` until the write side ends, so an
RPC that ends on its own while the writer is idle is reported only when the application next writes, closes, or
cancels. That is why the same hold was first prototyped and rejected: applied to `ActiveBidiReactor`, it made
`RouteChat_DeadlineExceeded_PropagatesStatus` and `RouteChat_ServerClosesFirst_ClientContinues` hang for their full
timeouts. For a bidirectional RPC the cost buys nothing, since its read side reports the end and lets the idle hold go.
For a client-streaming RPC nothing else can report it, so the cost is what correctness takes. It becomes an obligation
of the same kind as releasing a held message: the RPC completes only once the application ends its write side.

Issuing every write from inside a reaction would need no hold at all, which is why gRPC calls out-of-reaction
operations the uncommon case. It works when the requests are known upfront: the first one can be written before
`StartCall()`, and each next one from the `write_done` callback, on the gRPC thread. It cannot serve an application
that produces requests later, on its own thread, while the stream is idle, since no reaction exists then to write
from. The write-flow hold covers both cases, and a `write_done` callback that writes the next request still works
under it, because the claim makes a write call safe from any thread.

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
| `ActiveUnaryReactor<ResponseT, kDelivery>` | Unary | Response type, `Delivery` |
| `ActiveReadReactor<ResponseT, kDelivery, kPacing>` | Server-streaming | Response type, `Delivery`, `ReadPacing` |
| `ActiveWriteReactor<RequestT, ResponseT, kDelivery>` | Client-streaming | Request and response types, `Delivery` |
| `ActiveBidiReactor<RequestT, ResponseT, kDelivery, kPacing>` | Bidirectional | Request and response types, |
| | | `Delivery`, `ReadPacing` |

`Delivery` has no default, so every instantiation names its approach. The two reading reactors also take
`ReadPacing` as a template parameter, which defaults to `kPerMessage`, so an instantiation names it only to fix the
pacing to `kContinuous` or `kTurnByTurn`. No constructor takes a pacing argument.

**Service-Specific Adapters** (`applications/reactor/reactor_client_routeguide.h`):

| Generic Class | RouteGuide Adapter | RPC Method |
| --------------- | ------------------- | ------------ |
| `ActiveUnaryReactor<Feature, D>` | `routeguide::GetFeature::ClientReactor<D>` | `GetFeature` |
| `ActiveReadReactor<Feature, D, P>` | `routeguide::ListFeatures::ClientReactor<D, P>` | `ListFeatures` |
| `ActiveWriteReactor<Point, RouteSummary, D>` | `routeguide::RecordRoute::ClientReactor<D>` | `RecordRoute` |
| `ActiveBidiReactor<RouteNote, RouteNote, D, P>` | `routeguide::RouteChat::ClientReactor<D, P>` | `RouteChat` |

Here `D` is the `Delivery` and `P` the `ReadPacing` the application chose for that RPC. The adapters forward both
untouched, for the reason given under
[Why the generic layer offers every combination](#why-the-generic-layer-offers-every-combination).

### Callback struct patterns

Each reactor type has a corresponding callback struct, templated on the same `Delivery` as its reactor, and on the
same `ReadPacing` for the two reading reactors. The slot names are the same in every combination; only the slot
that carries a message changes signature:

**Unary** (`ActiveUnaryCallbacks<ResponseT, kDelivery>`):

- `done`: called on RPC completion, receives the status and the response: a `const ResponseT&` under `kPull`, to be
  pulled later through `GetResponse()`, or ownership of it as a `std::unique_ptr<ResponseT>` under `kPush`

**Server-streaming** (`ActiveReadCallbacks<ResponseT, kDelivery, kPacing>`):

- `read_ok`: called on successful read. Its parameter follows `kDelivery`: a lent `const ResponseT&` under `kPull`,
  or ownership of the message as a `std::unique_ptr<ResponseT>` under `kPush`. Its return type follows `kPacing`:
  `bool` under `kPerMessage`, where `true` holds the stream and `false` reads on, and `void` under `kContinuous` and
  `kTurnByTurn`
- `read_nok`: called when stream ends (no more reads)
- `done`: called on RPC completion

**Client-streaming** (`ActiveWriteCallbacks<RequestT, ResponseT, kDelivery>`):

- `write_done`: called after each write completes
- `done`: called on RPC completion, receives the status and the terminal response, as for the unary `done`

**Bidirectional** (`ActiveBidiCallbacks<RequestT, ResponseT, kDelivery, kPacing>`):

- `read_ok`: called on successful read, as for the server-streaming `read_ok`
- `read_nok`: called when read stream ends
- `write_done`: called after each write completes
- `done`: called on RPC completion

### File organization

**New reactor implementations:**

| File Type | Location | Naming |
| ----------- | ---------- | -------- |
| Generic base class | `applications/reactor/reactor_client.h` | Add to existing file |
| Service adapter | `applications/reactor/reactor_client_routeguide.h` | Add to existing file |
| Unit tests | `applications/reactor/tests/` | `{reactor_name}_{pull,push}_test.cpp` |

**Test file naming:** one file per reactor and `Delivery` approach, each pinning its approach in a single
`kDelivery` constant.

- `active_write_reactor_pull_test.cpp`, `active_write_reactor_push_test.cpp`: client-streaming reactor tests
- `active_bidi_reactor_pull_test.cpp`, `active_bidi_reactor_push_test.cpp`: bidirectional reactor tests
- `read_pacing_test_support.h`: the `ReadPacing` tags and type lists the two reading reactors' typed tests run over

### Error handling pattern

Use `grpc::Status` passthrough instead of custom exceptions:

```cpp
// Delivery::kPush: the callback receives the status, and ownership of the response where the RPC has a terminal one.
// Under Delivery::kPull it receives a const ResponseT& instead, and the same status check applies.
cbs.done = [](auto* reactor, const grpc::Status& status, std::unique_ptr<ResponseT> response) {
  if (status.ok()) {
    // Process *response
  } else {
    // Handle error via status.error_code(), status.error_message()
    logger.error("RPC failed: {} - {}",
                 status.error_code(), status.error_message());
  }
};

// Under kPush the response pointer is never null, so no consumer needs a null check: a failed RPC
// yields an empty message, and the status is what tells the consumer whether the content is meaningful.
```

**Rationale:** Status-based error handling aligns with Google C++ Style Guide (which discourages exceptions),
matches gRPC's native pattern, and works cleanly with the async callback model where exceptions cannot cross
thread boundaries.

### Thread safety patterns

**Atomic flags for cross-thread state:**

```cpp
// Reading reactors: the token of the held message, 0 when none is
std::atomic<std::uint64_t> response_ready_{0};  // kPull: set by gRPC thread, claimed by GetResponse() or a decline
std::atomic<std::uint64_t> read_held_{0};       // kPush: set by gRPC thread, claimed by ResumeRead() or a decline
std::atomic_bool write_idle_{false};       // Bidi only: an unclaimed idle-window hold
std::atomic<int> write_flow_{kFlowEnded};  // Writer only: the write flow, open, claimed or ended
std::atomic_bool stream_no_more_{false};   // Set by gRPC thread, read by app thread
```

`response_ready_` exists in all four reactors and is used under `Delivery::kPull`, where it records that a message is
waiting for `GetResponse()`. On `ActiveUnaryReactor` and `ActiveWriteReactor` it is a plain `std::atomic_bool` that
`OnDone()` sets and `GetResponse()` clears. On the two reading reactors it is used only under `kTurnByTurn` and
`kPerMessage`, and it holds the token of an unclaimed read hold, as `read_held_` does under `Delivery::kPush`. The
release call claims the token by exchanging it for zero, which makes a duplicate or spurious `GetResponse()` or
`ResumeRead()` a rejected no-op while nothing is held. That keeps the hold count right, but the call releases whatever
message is held at the time, so the application still makes one call per message the callback kept, and none for a
declined one. A `kPerMessage` decline claims it by compare-exchange against its own message's token,
for the reason given under [Why kPerMessage takes one for every message](#why-kpermessage-takes-one-for-every-message).
`write_idle_` exists in `ActiveBidiReactor` only, under both approaches, where winning it by compare-exchange is the
permission to write, which is why that class needs no write-pending flag. `write_flow_` is its counterpart in
`ActiveWriteReactor`: open, claimed by one write call, or ended, and only the transition to ended releases the hold.
`stream_no_more_` reports that no further read or write can succeed: on both write paths it is re-read after the claim
is won rather than before, so a stale read cannot let a doomed operation through, and on the reading side it decides
whether `GetResponse()` or `ResumeRead()` re-arms. `ActiveUnaryReactor` has only
`response_ready_`, because it issues nothing from the application thread and owns no stream.

**Hold mechanism, read side (`ReadPacing::kPerMessage` and `kTurnByTurn`, under either `Delivery`):**

```cpp
// In OnReadDone, after the delivery step and before invoking the callback, since the callback may hand off
// or resume inline
this->AddHold();
const auto held = ++held_count_;  // this message's token, never 0
response_ready_ = held;           // kPull; kPush sets read_held_ instead

// In GetResponse (kPull) or ResumeRead (kPush), once the application is done with the message
if (token.exchange(0) == 0) return false;  // nothing held, or already claimed
this->StartRead(&response_);   // raises the count first, so it never reaches zero; skipped if the stream is over
this->RemoveHold();            // exactly one release per hold, armed or not

// In OnReadDone, kPerMessage only, when read_ok returned false
if (token.compare_exchange_strong(held, 0)) {  // lost: this message's hold was already released
  this->StartRead(&response_);
  this->RemoveHold();
}
```

**Hold mechanism, write flow (`ActiveWriteReactor`, under either `Delivery`):**

```cpp
// In StartCall(), before the RPC is activated
this->AddHold();
write_flow_ = kFlowOpen;

// In SendRequest(), and alike in SendLastRequest() and CloseRequestStream()
if (!write_flow_.compare_exchange_strong(open, kFlowBusy)) return false;  // ended, or another call holds it
this->StartWrite(&pending_request_);       // safe: the hold cannot be released while the claim is held
write_flow_ = kFlowOpen;                   // SendLastRequest() and CloseRequestStream() end it instead,
if (stream_no_more_) EndOpenWriteFlow();   // and release the hold

// In OnWriteDone(false) and TryCancel(), when the write side can no longer succeed
stream_no_more_ = true;                    // published before trying to end the flow
EndOpenWriteFlow();                        // ends it only if open; a claimer ends it when it returns it
```

`ReadPacing::kContinuous` needs no equivalent, and neither does `ActiveUnaryReactor`. See
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
