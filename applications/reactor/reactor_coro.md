# Coroutine implementation of gRPC clients

The coroutine level lets an application consume the reactors of
[reactor_client.md](/applications/reactor/reactor_client.md) from a C++20 coroutine: one function per RPC, which
suspends at each `co_await` and resumes on the application thread. This document is its reference. Why the level
exists, and why each choice was made, is in the [design draft](/docs/client-coroutines-design.md).

> [!NOTE]
> The level is built step by step, as the design draft's
> [suggested order](/docs/client-coroutines-design.md#suggested-order) lists. Today it has the task core, the unary
> awaitable and the reader without a buffer. The buffered readers, the writers and the bidirectional join come next,
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
| [reactor_coro.h](/applications/reactor/reactor_coro.h) | Generic reactors, task core | `Unary`, `UnaryResult`, `Reader`, `DestroyAfterOnDone()` |
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
| Its handler | Asserts it runs on the application thread, then resumes the handle |

One per process, created in `main()` before any coroutine starts: an assert refuses a second one, which would resume
every handle twice. EventLoop's queue is FIFO, so a coroutine's resumptions run in the order they were posted.

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
and `Delivery` decides how:

| Delivery | `UnaryResult::response` | How it becomes owned |
|----------|-------------------------|----------------------|
| `kPull` | `ResponseT` | Swapped out of the reactor by `GetResponse()`, empty unless the status is OK |
| `kPush` | `std::unique_ptr<ResponseT>` | Handed over by the done callback, never null, empty on a failed RPC |

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
| `co_await Finish()` | Once `Read()` has given nothing: resumes once the RPC has ended, with its `grpc::Status` |
| `TryCancel()` | Cancels an RPC still in flight: `Read()` then gives nothing, and `Finish()` gives `CANCELLED` |
| Destructor | Cancels an RPC in flight, releases a message still held, destroys the reactor after `OnDone()` |

Non-copyable and non-movable, since the reactor's callbacks refer to it. A `Read()` awaited again after the end gives
nothing at once. `Finish()` asserts that `Read()` has given nothing: while the reactor holds a message, the RPC cannot
end. To stop early, call `TryCancel()`, then read until `Read()` gives nothing.

Every message is owned by the coroutine, and `Delivery` decides how, and when the next one is read:

| Delivery | `Read()` gives | How it becomes owned | Next read armed |
|----------|----------------|----------------------|-----------------|
| `kPull` | `std::optional<ResponseT>` | Swapped out of the reactor by `GetResponse()` | At once, by `GetResponse()` |
| `kPush` | `std::unique_ptr<ResponseT>` | Handed over by the read callback | By the next `Read()`: `ResumeRead()` |

So under `kPull` the next message can arrive while the coroutine works on this one, and under `kPush` nothing is read
until the coroutine asks for it, which is what a reader that waits for a person needs.

Inside, the reader has two hand-overs. One is re-armed for each read, and ended by `read_ok` or `read_nok`. The other
is ended by `OnDone()`, which can come before the coroutine has resumed from its last read, so it cannot share the
first one. The read hand-over is re-armed before the call that arms the next read: in `Read()` under `kPush`, before
`ResumeRead()`, and as the message is taken under `kPull`, before `GetResponse()`.

### `DestroyAfterOnDone(reactor, timeout)`

Cancels a reactor's RPC, and destroys the reactor once its `OnDone()` has run. Should `OnDone()` not come within the
timeout, it leaks the reactor rather than freeing it under gRPC, and returns false. Every awaitable uses it with
`kOnDoneTimeout`, 5 seconds.

## Scenarios and tests

- [Coroutine scenarios](/applications/reactor/variants/README.md#coroutine-scenarios) lists every coroutine scenario,
  and the two diffs each has.
- [Coroutine core coverage](/docs/testing.md#coroutine-core-coverage),
  [unary awaitable coverage](/docs/testing.md#unary-awaitable-coverage) and
  [reader coverage](/docs/testing.md#reader-coverage) list what each test proves, and the mutants each caught.
- [The design draft](/docs/client-coroutines-design.md) explains why each choice was made.
