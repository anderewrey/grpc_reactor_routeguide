# Coroutine implementation of gRPC clients

The coroutine level lets an application consume the reactors of
[reactor_client.md](/applications/reactor/reactor_client.md) from a C++20 coroutine: one function per RPC, which
suspends at each `co_await` and resumes on the application thread. This document is its reference. Why the level
exists, and why each choice was made, is in the [design draft](/docs/client-coroutines-design.md).

> [!NOTE]
> The level is built step by step, as the design draft's
> [suggested order](/docs/client-coroutines-design.md#suggested-order) lists. Today it has the task core, the unary
> awaitable, the reader without a buffer, and the writers. The buffered readers and the bidirectional join come next,
> and each adds its section here.

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
| [reactor_coro.h](/applications/reactor/reactor_coro.h) | Generic reactors, task core | Every awaitable, its helpers |
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
- **One awaiter.** A `Task` is awaited once at most, and an awaitable's `Finish()` exactly once. Asserts in
  `Task::operator co_await()` and in each `Finish()` enforce it. A `Reader`'s `Read()` and a `Writer`'s `Write()` are
  awaited one at a time, which the shape of the code enforces: a coroutine cannot await a second one before the first
  resumes it.
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

A server-streaming RPC a coroutine reads one message at a time, with no buffer. `ReactorT` is a service-specific
reactor deriving from `ActiveReadReactor` under `ReadPacing::kTurnByTurn`, such as
`routeguide::ListFeatures::ClientReactor<Delivery::kPush, ReadPacing::kTurnByTurn>`. Another pacing does not compile.

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

### `Writer<ReactorT>`

A client-streaming RPC a coroutine writes one request at a time. `ReactorT` is a service-specific reactor deriving
from `ActiveWriteReactor` under `Submission::kApplicationPushes`, such as
`routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kApplicationPushes>`. Another submission does not
compile.

```cpp
coro::Writer<routeguide::RecordRoute::ClientReactor<Delivery::kPush, Submission::kApplicationPushes>> writer{
    executor, stub};
for (auto& point : points) {
  if (!co_await writer.Write(std::move(point))) break;  // the RPC has ended
}
static_cast<void>(co_await writer.Close());
auto [status, summary] = co_await writer.Finish();
```

| Member | Use |
|--------|-----|
| `Writer(executor, stub, context)` | Starts the RPC, which sends nothing until the first `Write()`. Optional context |
| `co_await Write(request)` | Resumes once the write has completed: true, or false when the RPC has ended |
| `co_await Close()` | Ends the request stream, without suspending: false when the RPC has ended |
| `co_await Finish()` | Resumes once the RPC has ended, with a `UnaryResult`. Before `Close()`, it cancels first |
| `TryCancel()` | Cancels an RPC still in flight: `Write()` then gives false, and `Finish()` gives `CANCELLED` |
| Destructor | Cancels an RPC in flight, which ends its write flow, destroys the reactor after `OnDone()` |

Non-copyable and non-movable, since the reactor's callbacks refer to it. One write is in flight at most, which the
shape of the code enforces. The write starts in `Write()` itself, and the awaitable it returns is awaited at once,
which `[[nodiscard]]` reminds. A `Write()` that gives false sent nothing, and every later one gives false at once: it
is never a reason to retry. A write can still succeed after the server has ended the RPC, while its status is on the
way, so `Finish()` says how the RPC ended, never the writes.

`Close()` never suspends: the close completing is seen by `Finish()`, which waits for the RPC to end. To stop early,
await `Finish()` without `Close()`: the write flow the reactor keeps open for the next write would hold the RPC
open, so `Finish()` cancels it. The status is then `CANCELLED`, unless the server's own status had already arrived.
After a write that failed, the RPC ends on its own, and `Finish()` gives its status without cancelling.

The final response is owned by the coroutine, as for `Unary`: swapped out of the reactor under `kPull`, moved out of
the message the done callback hands over under `kPush`.

Inside, the writer has two hand-overs, as the reader has. The write hand-over is re-armed as each completed write is
taken, in `await_resume()`, and not before the next write starts, as the reader does before `ResumeRead()`: a fresh
hand-over is not ended, so the first write could not re-arm it. Either way, it is re-armed before the call that
starts the next operation. `OnDone()` has its own, since it can come before the coroutine has resumed from a write
that failed.

### `Source<ReactorT>`

A client-streaming RPC whose requests are all at hand. `ReactorT` is a service-specific reactor deriving from
`ActiveWriteReactor` under `Submission::kReactorPulls`, such as
`routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>`. Another submission does not
compile.

```cpp
coro::Source<routeguide::RecordRoute::ClientReactor<Delivery::kPull, Submission::kReactorPulls>> upload{
    executor, stub, std::move(track)};
auto [status, summary] = co_await upload.Finish();
```

| Member | Use |
|--------|-----|
| `Source(executor, stub, requests, context)` | Takes the requests, and starts the RPC. The context is optional |
| `co_await Finish()` | Resumes once every request is sent and the RPC has ended, with a `UnaryResult` |
| `TryCancel()` | Cancels an RPC still in flight: no further request is pulled, and `Finish()` gives `CANCELLED` |
| Destructor | Cancels an RPC in flight, which stops the pulls, destroys the reactor after `OnDone()` |

The source takes any range of requests. A `std::vector<RequestT>` passed by rvalue is moved in, and any other range,
a view included, is copied into one on the constructing thread. The reactor's `next_request` then moves each request
out of that vector, once inside the reactor's constructor, then on a gRPC thread after each completed write, and
closes the request stream after the last one. So no code of the application's, such as a view's function, runs off
the application thread.

`Finish()` never cancels, unlike the other awaitables' `Finish()` awaited early. A `Source` has nothing to read or
write, so its `Finish()` comes at once, and the coroutine skips nothing by awaiting it: the reactor sends the whole
range on its own. `TryCancel()`, or destroying the `Source`, stops it.

### `DestroyAfterOnDone(reactor, timeout)`

Cancels a reactor's RPC, and destroys the reactor once its `OnDone()` has run. Should `OnDone()` not come within the
timeout, it leaks the reactor rather than freeing it under gRPC, and returns false. Every awaitable uses it with
`kOnDoneTimeout`, 5 seconds.

## Scenarios and tests

- [Coroutine scenarios](/applications/reactor/variants/README.md#coroutine-scenarios) lists every coroutine scenario,
  and the two diffs each has.
- [Coroutine core coverage](/docs/testing.md#coroutine-core-coverage),
  [unary awaitable coverage](/docs/testing.md#unary-awaitable-coverage),
  [reader coverage](/docs/testing.md#reader-coverage) and [writer coverage](/docs/testing.md#writer-coverage) list
  what each test proves, and the mutants each caught.
- [The design draft](/docs/client-coroutines-design.md) explains why each choice was made.
