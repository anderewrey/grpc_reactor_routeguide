# Coroutine implementation of gRPC clients

The coroutine level lets an application consume the reactors of
[reactor_client.md](/applications/reactor/reactor_client.md) from a C++20 coroutine: one function per RPC, which
suspends at each `co_await` and resumes on the application thread. This document is its reference. Why the level
exists, and why each choice was made, is in the [design draft](/docs/client-coroutines-design.md).

> [!NOTE]
> The level is built step by step, as the design draft's
> [suggested order](/docs/client-coroutines-design.md#suggested-order) lists. Today it has the task core, the unary
> awaitable, and the readers without a buffer and with each of the three buffers: unbounded, latest-only and
> bounded. The writers and the bidirectional join come next, and each adds its section here.

## The words you need

| Word | What it is here |
|------|-----------------|
| Coroutine | A function containing `co_await` or `co_return`, such as a scenario's `Run()`, that can stop and resume |
| Frame | The heap memory holding a coroutine's locals and arguments, which survives while it is stopped |
| Suspend | The coroutine stops at a `co_await`, and the thread returns to whoever started or resumed it |
| Resume | The coroutine continues where it suspended, with its frame intact |
| Handle | `std::coroutine_handle<>`, the address of a suspended frame, to resume once |
| Promise | The object in each frame that decides how the coroutine starts, ends and returns its value |
| Awaitable | What a `co_await` waits on, such as `Unary::Finish()`: it decides whether to suspend, and the result |
| Executor | What resumes a suspended handle later, on the thread of its choice: here, the application thread |
| Task | `Task<T>`, a coroutine that produces one `T` for the one coroutine that awaits it |
| Root task | A task no coroutine awaits, started by `RootTasks::Spawn()`, such as one scenario per RPC |

## The files

The level has two headers of its own, and shares the EventLoop adapter with the callback level:

| Header | Knows | Holds |
|--------|-------|-------|
| [coro_task.h](/applications/reactor/coro_task.h) | C++20 coroutines only | `Task`, `RootTasks`, `Executor`, `ResumeHandover`, `CurrentTask` |
| [reactor_coro.h](/applications/reactor/reactor_coro.h) | Generic reactors, task core | `Unary`, `Reader`, `Buffer`, and helpers |
| [reactor_eventloop.h](/applications/reactor/reactor_eventloop.h) | EventLoop | `EventLoopExecutor`, beside the callback level's `EventConnection` |

Everything is in the `RpcReactor::coro` namespace. A helper used by one class only is a private member of that class,
such as `Task`'s promise. A helper several classes use is a named class of the level, such as `ResumeHandover`.

## The life of one `co_await`

What happens when a coroutine awaits a unary RPC, as
[get_feature_pull_coro.cpp](/applications/reactor/variants/get_feature_pull_coro.cpp) does:

```cpp
coro::Task<> GetFeature::Run(routeguide::Point point) {
  Call call{executor_, stub_, point, CreateClientContext()};  // 1. the request is sent
  const auto [status, response] = co_await call.Finish();     // 2 to 7
}
```

1. **Application thread.** Constructing the `Unary` constructs its reactor, which sends the request.
2. **Application thread.** `co_await Finish()` asks `ResumeHandover::Suspend()` whether to suspend. If the RPC has
   already ended, it does not suspend, and goes to step 7.
3. **Application thread.** The coroutine suspends, and the thread returns to what started or resumed it, such as
   `RootTasks::Spawn()` or an event handler.
4. **gRPC thread.** `OnDone()` calls the awaitable's done callback. Under `kPush` it keeps the message, then
   `ResumeHandover::Complete()` hands the handle to the executor.
5. **gRPC thread.** `EventLoopExecutor::Post()` queues the handle as an EventLoop event.
6. **Application thread.** The event's one handler asserts it runs on the application thread, and resumes the handle.
7. **Application thread.** `Finish()` takes the status and the owned response out of the reactor, destroys the reactor
   once `OnDone()` has returned, and gives back a `UnaryResult`.

The only code that runs on the gRPC thread, steps 4 and 5, is the level's own. The coroutine's code always runs on the
application thread, which is the level's boundary rule.

## The Active Object mapping

The coroutine level changes how the application consumes a reactor, and nothing below:

| Component | Callback level | Coroutine level |
|-----------|----------------|-----------------|
| Proxy | A scenario's `Start()` creates the reactor | A scenario's `Start()` spawns its coroutine |
| Method Request | The reactor | The same reactor, inside an awaitable |
| Scheduler, Activation Queue | EventLoop | EventLoop, which `EventLoopExecutor` posts to |
| Future | `Status()` and `GetResponse()`, queried by a handler | The awaitable: `co_await Read()` or `Finish()` |
| Servant | One handler per event | One coroutine per RPC |

## Rules a coroutine follows

- **Arguments by value.** A coroutine outlives the call that started it, so a reference argument dangles once it
  suspends. clang-tidy enforces it with `cppcoreguidelines-avoid-reference-coroutine-parameters` and
  `cppcoreguidelines-avoid-capturing-lambda-coroutines`.
- **One awaiter.** A `Task` is awaited once at most, and a `Unary`'s or a `Reader`'s `Finish()` exactly once. Asserts
  in `Task::operator co_await()` and in each `Finish()` enforce it. A `Reader`'s `Read()` is awaited one at a time,
  which the shape of the code enforces: a coroutine cannot await a second one before the first resumes it.
- **Errors are values.** A failed RPC is its `grpc::Status`, never an exception. An exception that escapes a coroutine
  body is a bug: it is logged, and the process terminates.
- **Frames end on the application thread.** `RootTasks` spawns, resumes and destroys its tasks on the application
  thread. Destroy it only once nothing can post one of its handles any more, after the event loop has stopped.
- **Every suspension goes through `ResumeHandover`.** An awaitable that suspends another way posts nothing to the
  executor, and leaves the logs showing a task that is no longer running.

## Telling coroutines apart in the logs

Every coroutine resumes on the application thread, so the thread id in the log pattern no longer tells two coroutines
apart. The coroutine applications log the running task after it, from `SetCoroutineLogPattern()` in
[demo_support.h](/applications/reactor/demo_support.h):

```text
[16:10:16.935163][Main][14743][task#-][I] -------------- Use case: what's here --------------
[16:10:16.936166][WhatsHere.coro][14743][task#1][I]          | rpc started
[16:10:16.937982][WhatsHere.coro][14743][task#1][I] card     | Patriots Path, Mendham, NJ 07945, USA
```

`[task#N]` is the root task the line runs in, the number `Spawn()` returned. `[task#-]` is code outside any task, such
as `main()` or a callback scenario's handler.

## API

### `Task<T>`

A lazily started coroutine that produces one `T`, or nothing for `Task<>`, for the one coroutine that awaits it.

| Member | Use |
|--------|-----|
| `co_await std::move(task)` | Starts the body, and gives its `T` once it ends |
| Destructor | Destroys the frame, finished, not started or suspended, and any task it awaits |

Move-only, and `[[nodiscard]]`. A body that ends without suspending lets its awaiter go on without any transfer, so a
loop of such awaits keeps a flat stack on every compiler, see
[the task type](/docs/client-coroutines-design.md#the-task-type-written-here-not-taken-from-a-library).

### `RootTasks`

Owns the root tasks: the ones no coroutine awaits.

| Member | Use |
|--------|-----|
| `Spawn(Task<> task)` | Runs the task up to its first suspension, keeps it until it ends, and returns its number |
| `Size()` | How many root tasks have started and not ended yet |
| Destructor | Destroys the frames still suspended, which releases everything they own |

Non-copyable and non-movable. A scenario holds one, declared last, so its suspended coroutine is destroyed before
anything it uses.

### `Executor`

The interface an awaitable hands a suspended coroutine to. `Post(handle)` queues the handle for resumption, and is
callable from any thread. The tests implement it over the `AppLoop` task queue, and the applications use
`EventLoopExecutor`.

### `EventLoopExecutor`

The executor of the applications, in [reactor_eventloop.h](/applications/reactor/reactor_eventloop.h).

| Member | Use |
|--------|-----|
| Constructor | Registers the one handler of `kResumeEvent`, for the application thread, by default the calling one |
| `Post(handle)` | Triggers `kResumeEvent` with the handle's address |
| `co_await After(delay)` | Static: resumes the coroutine once an EventLoop timer of that delay has fired |
| Its handler | Asserts it runs on the application thread, then resumes the handle |

One per process, created in `main()` before any coroutine starts: an assert refuses a second one, which would resume
every handle twice. EventLoop's queue is FIFO, so a coroutine's resumptions run in the order they were posted.

`After()` is how a coroutine waits for time, such as for a driver to confirm a stop, so a scenario never calls
EventLoop itself. Its timer triggers `kResumeEvent` too, so it needs the process's executor, and the suspension goes
through a `ResumeHandover`, which carries the task number across it. Like any resumption still to come, the timer
must not outlive the frame: destroy the frame only once the event loop no longer runs.

### `ResumeHandover`

The hand-over between the thread that ends an operation and the coroutine awaiting it, which every awaitable uses.
Lock-free. Once ended, it stays ended until `Rearm()` starts the next operation, so one hand-over serves a coroutine
that awaits operations one after the other, such as each read of a stream.

| Member | Called by | Use |
|--------|-----------|-----|
| `Suspend(waiter)` | The awaitable's `await_suspend()` | Whether to suspend: false once the operation has ended |
| `Complete(executor)` | Whatever ends the operation, on any thread | Posts the waiting coroutine, if any |
| `Abandon()` | The awaitable's destructor | Makes a later `Complete()` post nothing |
| `Resumed()` | The awaitable's `await_resume()`, first | Restores the task number for the logs |
| `Rearm()` | The awaitable, before it starts the next operation | Starts afresh, once the last operation has ended |

`Rearm()` comes before whatever starts the next operation, never after: the operation can end on another thread as
soon as it starts, and an end that finds the hand-over still ended posts nothing, which leaves the coroutine suspended
for good. An assert checks that the last operation has ended. Each `Suspend()` records the task again, so each
resumption restores it.

### `CurrentTask`

`CurrentTask::Get()` gives the number of the root task the calling thread runs, or `CurrentTask::kNone`. Only
`RootTasks` and `ResumeHandover` set it.

### `Unary<ReactorT>`

A unary RPC a coroutine awaits. `ReactorT` is a service-specific reactor deriving from `ActiveUnaryReactor`, such as
`routeguide::GetFeature::ClientReactor<Delivery::kPull>`.

| Member | Use |
|--------|-----|
| `Unary(executor, stub, request, context)` | Sends the request. The context is optional, for a deadline or metadata |
| `co_await Finish()` | Resumes once the RPC has ended, with a `UnaryResult` |
| `TryCancel()` | Cancels an RPC still in flight: `Finish()` then gives `CANCELLED` |
| Destructor | Cancels an RPC still in flight, and destroys the reactor once `OnDone()` has run |

Non-copyable and non-movable, since the reactor's callback refers to it. The response is always owned by the coroutine,
as a `ResponseT` under both deliveries, empty unless the status is OK. `Delivery` decides only how it becomes owned:

| Delivery | How it becomes owned |
|----------|----------------------|
| `kPull` | Swapped out of the reactor by `GetResponse()` |
| `kPush` | Moved out of the message the done callback hands over, with no copy |

### `Reader<ReactorT>`

A server-streaming RPC a coroutine reads. `ReactorT` is a service-specific reactor deriving from `ActiveReadReactor`,
and its `Delivery` and `ReadPacing` choose the buffer between the reactor and the coroutine, with no parameter of
their own. `BufferFor(delivery, pacing)` is the one place that maps them, and `Reader` has one specialization per
`Buffer`:

| `Buffer` | Reactor | Section |
|----------|---------|---------|
| `kNone` | `ReadPacing::kTurnByTurn`, either delivery | [Without a buffer](#without-a-buffer) |
| `kUnbounded` | `Delivery::kPush`, `ReadPacing::kContinuous` | [With an unbounded buffer](#with-an-unbounded-buffer) |
| `kLatest` | `Delivery::kPull`, `ReadPacing::kContinuous` | [With a latest-only buffer](#with-a-latest-only-buffer) |
| `kBounded` | `Delivery::kPush`, `ReadPacing::kPerMessage` | [With a bounded buffer](#with-a-bounded-buffer) |

Any other combination does not compile: the primary template's `static_assert` names the ones that do.
`BufferOf(reactor_pointer)` gives the buffer of a reactor type at compile time, or nothing, which the tests check.
Every reader's `Read()` gives a `std::optional<ResponseT>`, empty at the end of the stream, and its `Finish()` a
`grpc::Status`.

#### Without a buffer

A server-streaming RPC a coroutine reads one message at a time. Its reactor runs under `ReadPacing::kTurnByTurn`,
such as `routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kTurnByTurn>`.

```cpp
coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kTurnByTurn>> reader{
    executor, stub, rect};
while (auto feature = co_await reader.Read()) Show(*feature);
const grpc::Status status = co_await reader.Finish();
```

| Member | Use |
|--------|-----|
| `Reader(executor, stub, request, context)` | Sends the request, and arms the first read. The context is optional |
| `co_await Read()` | Resumes with the next message, owned, or with nothing once the stream has ended |
| `co_await Finish()` | Resumes once the RPC has ended, with its `grpc::Status`. Before the end, it cancels first |
| `TryCancel()` | Cancels an RPC still in flight: `Read()` then gives nothing, and `Finish()` gives `CANCELLED` |
| Destructor | Cancels an RPC in flight, releases a message still held, destroys the reactor after `OnDone()` |

Non-copyable and non-movable, since the reactor's callbacks refer to it. A `Read()` awaited again after the end gives
nothing at once. To stop early, await `Finish()` without reading on: while the reactor holds a message, or a read is
armed, the RPC cannot end, so `Finish()` ends it with the reactor's `CancelAndRelease()`. It cancels the RPC, and
releases the message held, and any message a read already in flight delivers, without arming a read. The status is
then `CANCELLED`, unless the coroutine had read every message and the server's own status had already arrived. Then
`Finish()` gives that status, such as OK.

Every message is owned by the coroutine, as a `std::optional<ResponseT>` under both deliveries. `Delivery` decides how
it becomes owned, and when the next one is read:

| Delivery | How it becomes owned | Next read armed |
|----------|----------------------|-----------------|
| `kPull` | Swapped out of the reactor by `GetResponse()` | At once, by `GetResponse()` |
| `kPush` | Moved out of the message the read callback hands over, with no copy | By the next `Read()`: `ResumeRead()` |

So under `kPull` the next message can arrive while the coroutine works on this one, and under `kPush` nothing is read
until the coroutine asks for it, which is what a reader that waits for a person needs.

Inside, the reader has two hand-overs. One is re-armed for each read, and ended by `read_ok` or `read_nok`. The other
is ended by `OnDone()`, which can come before the coroutine has resumed from its last read, so it cannot share the
first one. The read hand-over is re-armed before the call that arms the next read: in `Read()` under `kPush`, before
`ResumeRead()`, and as the message is taken under `kPull`, before `GetResponse()`.

#### With an unbounded buffer

A server-streaming RPC whose reactor reads on at its own pace, while a coroutine takes the messages one at a time, in
order. Its reactor runs under `Delivery::kPush` and `ReadPacing::kContinuous`, such as
`routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>`.

```cpp
coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kContinuous>> reader{
    executor, stub, rect};
while (auto report = co_await reader.Read()) Store(*report);
const grpc::Status status = co_await reader.Finish();
```

| Member | Use |
|--------|-----|
| `Reader(executor, stub, request, context)` | Sends the request, and arms the first read. The context is optional |
| `co_await Read()` | Resumes with the oldest message queued, owned, or with nothing once the stream has ended |
| `co_await Finish()` | Resumes once the RPC has ended. Before the end, it drops what is queued and cancels first |
| `Queued()` | How many messages wait in the queue: a snapshot of how far the coroutine is behind |
| `TryCancel()` | Cancels an RPC still in flight: `Read()` gives what is queued, then nothing |
| Destructor | Cancels an RPC in flight, destroys the reactor after `OnDone()`, then frees the queue |

The reactor arms each next read itself, so the stream never waits for the coroutine. The read callback, on the gRPC
thread, queues each message it is handed, owned, under a lock, and `Read()` takes the oldest one under the same lock,
with no copy. Nothing bounds the queue: a coroutine slower than the stream lets messages pile up, which `Queued()`
shows, and which [trip_recorder_coro.cpp](/applications/reactor/usecases/trip_recorder_coro.cpp) logs.

A `Read()` that finds the queue empty marks itself waiting, under the lock, and suspends through a `ResumeHandover`.
The next callback that queues a message, or ends the stream, finds the mark, clears it, and completes the hand-over,
out of the lock, so no lock is held while the executor queues the coroutine. So each wait is completed exactly once,
before or after the coroutine suspends, and `Read()` re-arms the hand-over as it resumes, before it can mark the next
wait. `OnDone()` has a hand-over of its own, as for the reader without a buffer.

Once the stream has ended, `Read()` gives every message still queued, then nothing. To stop early, await `Finish()`
without reading on: it drops the messages queued, and any message still to come, then cancels the RPC with
`CancelAndRelease()`. It gives `CANCELLED` when a message was dropped, even if the server's status, already arrived, was
OK: the coroutine did not read everything the server sent. Its message is then the reader's `kDroppedMessage`. All of
that happens when `Finish()` is called, not when it is awaited, so a message arriving in between is dropped too. When
nothing was dropped, it gives the server's own status once that had arrived, as the reader without a buffer does. That
reader never needs the first case, since nothing can wait behind it.

#### With a latest-only buffer

A server-streaming RPC whose reactor reads on at its own pace, while a coroutine takes only the newest message, as a
display of a live feed needs. Its reactor runs under `Delivery::kPull` and `ReadPacing::kContinuous`, such as
`routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>`.

```cpp
coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>> reader{
    executor, stub, rect};
while (auto fix = co_await reader.Read()) Draw(*fix);  // reader.Overwritten(): fixes skipped before this one
const grpc::Status status = co_await reader.Finish();
```

| Member | Use |
|--------|-----|
| `Reader(executor, stub, request, context)` | Sends the request, and arms the first read. The context is optional |
| `co_await Read()` | Resumes with the newest message, owned, or with nothing once the stream has ended |
| `Overwritten()` | How many messages were overwritten, unread, between the last two messages `Read()` gave |
| `co_await Finish()` | Resumes once the RPC has ended. Before the end, it drops the slot and cancels first |
| `TryCancel()` | Cancels an RPC still in flight: `Read()` gives the message in the slot, then nothing |
| Destructor | Cancels an RPC in flight, destroys the reactor after `OnDone()`, then frees the slot |

The reactor lends each message to the read callback, and reads the next one into the same storage as soon as the
callback returns, so the stream never waits for the coroutine, and a lent message cannot survive the callback. The
read callback therefore copies it, on the gRPC thread, and puts the copy in a `LatestSlot`, under the reader's lock,
replacing the message the coroutine has not taken. That copy is the level's own code, and the cost of this buffer:
one copy per message, taken or not. `Read()` takes the newest message under the same lock, or waits for the next one
through a `ResumeHandover`, as the unbounded reader does. The slot's own lock is only ever taken under the reader's.

`Read()` gives the message only, as every reader does, and `Overwritten()` says how many were skipped before it, so a
coroutine that reads to the end has seen every message the server sent as either taken or overwritten:
[list_features_pull_continuous_coro.cpp](/applications/reactor/variants/list_features_pull_continuous_coro.cpp) logs
one `skipped` line for each. The newest message is never overwritten: once the stream has ended, `Read()` gives the one
left in the slot, if any, then nothing. An early `Finish()` follows the unbounded reader's rule: it drops the message in
the slot, and those still to come, cancels, and gives `CANCELLED` with `kDroppedMessage` when it dropped one.

#### With a bounded buffer

A server-streaming RPC whose reactor reads on while few messages wait for the coroutine, and holds the stream once a
given number of them do, as a download slower to store than to receive needs. Its reactor runs under `Delivery::kPush`
and `ReadPacing::kPerMessage`, such as `routeguide::ListFeatures::ClientReactor<Delivery::kPush>`, and the number is
the constructor's `capacity`, at least 1.

```cpp
coro::Reader<routeguide::ListFeatures::ClientReactor<Delivery::kPush>> reader{executor, stub, rect, 3};
while (auto place = co_await reader.Read()) Save(*place);
const grpc::Status status = co_await reader.Finish();
```

| Member | Use |
|--------|-----|
| `Reader(executor, stub, request, capacity, context)` | Sends the request, and arms the first read |
| `co_await Read()` | Resumes with the oldest message queued, owned, or with nothing once the stream has ended |
| `co_await Finish()` | Resumes once the RPC has ended. Before the end, it drops the queue, releases the hold, cancels |
| `Queued()` | How many messages wait in the queue: never more than the capacity |
| `Holds()` | How many times the stream was held so far |
| `TryCancel()` | Cancels an RPC still in flight: `Read()` gives what is queued, then nothing |
| Destructor | Cancels an RPC in flight, releases a hold, destroys the reactor after `OnDone()`, frees the queue |

Under `kPerMessage`, `read_ok` decides for each message whether the stream waits. The reader takes that decision
itself, so no code of the coroutine runs on the gRPC thread: the read callback queues the message, owned, under a lock,
then lets the reactor read on while fewer than `capacity` messages wait, and holds the stream once that many do. The
`Read()` that takes the queue below `capacity` releases the hold with `ResumeRead()`, out of the lock. While the stream
is held no read is armed, so no message can come, and the queue never holds more than `capacity`. The hold is recorded
under the lock after the message is queued, so the `Read()` that releases it always has that message to take; the
reactor publishes the hold before it calls `read_ok`, so a `ResumeRead()` that reaches it while `read_ok` still returns
is safe. A `Read()` woken from an empty queue takes the message that woke it, and releases the hold too: at a capacity
of 1, that is every message.

An early `Finish()` drops the messages queued, and those still to come, then cancels the RPC and releases the hold with
`CancelAndRelease()`. A hold left in place would keep `OnDone()` from ever coming, and `Finish()` from resuming, which
the tests provoke. The status follows the unbounded reader's rule. A stream that ends while held, on an error say,
reports it once a `Read()` has released the hold: the coroutine still reads every message queued, then nothing.

### `DestroyAfterOnDone(reactor, timeout)`

Cancels a reactor's RPC, and destroys the reactor once its `OnDone()` has run. Should `OnDone()` not come within the
timeout, it leaks the reactor rather than freeing it under gRPC, and returns false. Every awaitable uses it with
`kOnDoneTimeout`, 5 seconds.

## Scenarios and tests

- [Coroutine scenarios](/applications/reactor/variants/README.md#coroutine-scenarios) lists every coroutine scenario,
  and the two diffs each has.
- [Coroutine core coverage](/docs/testing.md#coroutine-core-coverage),
  [unary awaitable coverage](/docs/testing.md#unary-awaitable-coverage) and
  [reader coverage](/docs/testing.md#reader-coverage),
  [unbounded buffer coverage](/docs/testing.md#unbounded-buffer-coverage),
  [latest-only buffer coverage](/docs/testing.md#latest-only-buffer-coverage) and
  [bounded buffer coverage](/docs/testing.md#bounded-buffer-coverage) list what each test proves, and the mutants
  each caught.
- [The design draft](/docs/client-coroutines-design.md) explains why each choice was made.
