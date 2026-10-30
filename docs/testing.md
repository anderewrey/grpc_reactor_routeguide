# Testing guide

This document describes the test architecture, coverage, and how to run and extend the test suite.

## Quick start

```bash
# Run all tests
ctest --test-dir cmake-build-vcpkg-debug-gcc

# Run with verbose output
ctest --test-dir cmake-build-vcpkg-debug-gcc -V

# Run specific test by name pattern
ctest --test-dir cmake-build-vcpkg-debug-gcc -R "ListFeatures"

# Run and show output only on failure
ctest --test-dir cmake-build-vcpkg-debug-gcc --output-on-failure
```

## Test architecture

The reactor client tests use two complementary approaches, matching the reactor type under test.

### Synchronous unit tests

Thirteen test binaries validate reactor callback logic directly, one per reactor type and `Delivery` approach, plus one
per writing reactor under `Submission::kReactorPulls`, one for tearing reactors down mid-RPC, and one for each use of
`ReadPacing::kContinuous`, using `std::promise`/`std::future` for synchronization instead of an EventLoop:

- [active_unary_reactor_pull_test.cpp][unary-pull-test], [active_unary_reactor_push_test.cpp][unary-push-test]:
  `ActiveUnaryReactor` via `GetFeature`
- [active_read_reactor_pull_test.cpp][read-pull-test], [active_read_reactor_push_test.cpp][read-push-test]:
  `ActiveReadReactor` via `ListFeatures`
- [active_write_reactor_pull_test.cpp][write-pull-test], [active_write_reactor_push_test.cpp][write-push-test]:
  `ActiveWriteReactor` via `RecordRoute`
- [active_bidi_reactor_pull_test.cpp][bidi-pull-test], [active_bidi_reactor_push_test.cpp][bidi-push-test]:
  `ActiveBidiReactor` via `RouteChat`
- [active_write_reactor_producer_test.cpp][write-producer-test],
  [active_bidi_reactor_producer_test.cpp][bidi-producer-test]: both writing reactors under `Submission::kReactorPulls`,
  as described in [request submission coverage](#request-submission-coverage)
- [reactor_teardown_test.cpp][teardown-test]: `CancelAndWait()` on all four reactors, as described in
  [teardown coverage](#teardown-coverage)
- [latest_slot_test.cpp][latest-test]: `LatestSlot`, and the latest-only pattern, as the `live_vehicle_position` use
  case runs it, on `ActiveReadReactor` under `kPull` and `kContinuous`, as described in
  [latest-only coverage](#latest-only-coverage)
- [continuous_journal_test.cpp][journal-test]: the journal pattern, as the `trip_recorder` use case runs it, on
  `ActiveReadReactor` under `kPush` and `kContinuous`, as described in [journal coverage](#journal-coverage)

Each file pins its approach in a single `kDelivery` constant and instantiates every reactor with it, so a pull file
and its push counterpart read side by side as the same scenarios written against each approach. The writing reactors'
files also pin their `Submission` in a `kSubmission` constant: `kApplicationPushes` in the pull and push files,
`kReactorPulls` in the producer files.
The four files of the reading reactors also cover every `ReadPacing` of their approach, as described in
[read pacing mode coverage](#read-pacing-mode-coverage).

Each test file runs an in-process gRPC server with a fake `TestRouteGuideService` implementation
configured per scenario. It completes a promise directly from the reactor's callbacks, so there
is no EventLoop lifecycle to manage. This makes the approach faster to write. It still exercises
reactor callback logic, error paths, cancellation, and deadline handling, without the added
complexity of a background dispatch thread.

### EventLoop integration test

[client_reactor_integration_test.cpp][integration-test] validates the full production dispatch
path: a gRPC reactor callback triggers `EventLoop::TriggerEvent()`, and the application thread
executes the deferred handler. It runs a real EventLoop in `NON_BLOCK` mode on a background
thread. It asserts that reactor callbacks execute off the main thread, while `EventLoop` handlers
execute the deferred processing. Each case runs once per `Delivery` approach, as `RpcMethod_Pull_...` and
`RpcMethod_Push_...`. Under `Delivery::kPull` the event carries the reactor, and the handler pulls the message out
through `GetResponse()`. Under `Delivery::kPush`, for `ActiveReadReactor`, this includes the ownership-transfer path:
the gRPC-thread `read_ok` callback releases the owned message into `EventLoop::TriggerEvent()`, and the
`EventLoop` handler reclaims that pointer. The same case asserts that messages arrive in network
order and that the `done` event is handled behind all of them, since both ride the same FIFO queue.

The `ListFeatures` cases name their `ReadPacing` as well:

| Case | Pacing | Handler |
| ------ | -------- | --------- |
| `ListFeatures_Pull_MultipleResponses_DispatchesToEventLoop` | `kPerMessage` | Calls `GetResponse()` |
| `ListFeatures_Push_MultipleResponses_DispatchesToEventLoop` | `kContinuous` | Reclaims the message |
| `ListFeatures_Push_HeldUntilHandled_DispatchesToEventLoop` | `kPerMessage` | Reclaims it, then calls `ResumeRead()` |

Under `kPerMessage` both cases return true from `read_ok`, so the stream stays held until the handler releases it.
The last case posts the reactor along with the message, so the handler can resume it. It asserts that no
message arrives while the previous one still waits for its handler.

Unlike the four unit test files, this single fixture accumulates one case per reactor type
instead of splitting into one file per type. Every case exercises the same dispatch path and
differs only in RPC shape.

### Coroutine core tests

Two test binaries test the core of the coroutine level, which the
[client coroutines design draft](/docs/client-coroutines-design.md) proposes. Neither needs a gRPC server:

- [coro_task_test.cpp][coro-task-test]: `Task<T>`, `RootTasks`, the `Executor` interface and `ResumeHandover` of
  [coro_task.h][coro-task], driven by hand or resumed on the `AppLoop` task queue of
  [continuous_use_support.h][continuous-support]
- [eventloop_executor_test.cpp][eventloop-executor-test]: `EventLoopExecutor`, of
  [reactor_eventloop.h][reactor-eventloop], against a real EventLoop in `NON_BLOCK` mode, started once for every test
  as in the integration test, since both EventLoop and the executor are one per process

They are described in [coroutine core coverage](#coroutine-core-coverage).

### Coroutine awaitable tests

[reactor_coro_unary_test.cpp][coro-unary-test] tests `coro::Unary`, the unary awaitable of
[reactor_coro.h][reactor-coro], against an in-process server, with each coroutine resumed on the `AppLoop` task queue.
Its executor counts the coroutines it is handed, so a test can tell whether `Finish()` suspended. The hand-over inside
the awaitable, between the done callback and `Finish()`, is tested on its own in coro_task_test.cpp, with no RPC and
no timing. Both are described in [unary awaitable coverage](#unary-awaitable-coverage).

### Callback application smoke test

[callback_smoke_test.sh][callback-smoke-test] runs the callback example applications themselves,
which have no unit tests: it starts `route_guide_callback_server`, runs `route_guide_callback_client`
against it twice, and checks how many `RouteChat` replies each run receives. The second run's
count depends on the notes the first run left on the server, so a server that resends earlier
replies fails it. Any sanitizer report from either process fails it too, which is how the
sanitizer jobs cover the server's reactors.

Both applications use the fixed port 50051, so the test holds a CTest `RESOURCE_LOCK` on it. The
server is killed at the end, so its exit-time checks (LeakSanitizer, coverage profiles) do not
run.

### Smoke matrix

The [smoke matrix][smoke-matrix-cmake] runs every client application against every server application:
`route_guide_sync_client`, `route_guide_callback_client`, the two reactor applications and their two coroutine twins,
against `route_guide_sync_server` and `route_guide_callback_server`, twelve CTest entries named
`smoke_matrix.<client>.<server>`, then one comparison, `smoke_matrix.compare`. Every job that runs `ctest` runs them,
the sanitizer jobs included.

Each cell, [smoke_matrix_cell.sh][smoke-matrix-cell], starts a fresh server, runs the client to its end, and fails on
a non-zero exit status, a sanitizer report from either process, or a failed RPC status in the client's log. It then
writes the client's digest: one line per logger, with the number of messages the scenario received, counted from its
`read_ok` and `skipped` lines when it logs any, else from its `RESPONSE` lines. The RouteChat loggers make one `chat`
line instead. Both servers answer each note with the notes already left at its location, by any RPC, but
`route_guide_callback_server` takes those notes and stores the new one under two separate locks, so concurrent chats
can miss each other's notes there, and what they receive depends on how they interleave. With one chat scenario, the
line counts its replies; with several, it counts only the chat scenarios that ran.

[smoke_matrix_compare.sh][smoke-matrix-compare] then checks that:

- each client's digest is the same against both servers;
- each logger of a coroutine twin application, named after its callback parent's logger with a `.coro` suffix, such
  as `GetFeature.pull.coro` for `GetFeature.pull`, counts what the parent's logger counts, against the same server.
  Every `.coro` logger must find its parent, and every twin application must have one.

A scenario added to an application, a coroutine twin included, needs nothing in the matrix. A new application is one
line in `SMOKE_SERVERS` or `SMOKE_CLIENTS`, and a new twin application one more line in `SMOKE_TWINS`. One limit: a
coroutine twin of a RouteChat scenario goes into its application's `chat` line, which is compared across servers
only, not with its parent.

The cells hold the same `RESOURCE_LOCK` on port 50051 as the callback smoke test, so they run one at a time. The
servers are killed at the end of each cell, so their exit-time checks do not run, while every client runs to its end,
LeakSanitizer and coverage profiles included.

### Reactor demo applications

The two reactor applications, `route_guide_active_reactor_client` for the variants and
`route_guide_reactor_usecases` for the use cases, and their coroutine twins, `route_guide_active_reactor_client_coro`
and `route_guide_reactor_usecases_coro`, have no unit tests of their own: the [smoke matrix](#smoke-matrix) runs them.
Each scenario holds an `rg_demo::Activity` from its `Start()` until its last handler has destroyed its reactor, or its
coroutine has ended, and `rg_demo::RunUntilDone()` halts the event loop once none is left, so each application exits
on its own, every reactor destroyed after its `OnDone()`. Every scenario asserts which thread runs
each callback and handler. Each use case also asserts what it claims, so a Debug run checks it every
time. For example, `turn_by_turn_guidance` asserts that no stop is read before the previous one was
confirmed, and `live_route_recording` that the summary counts every point the GPS produced. The
variants' ordering claims are checked in the transcript only: the header comment of each
application says which log lines to compare, such as a `resumed  | #k` line always coming before
`read_ok  | #k+1`. The patterns two use cases rely on are tested on their own, in the
[latest-only](#latest-only-coverage) and [journal](#journal-coverage) suites.

### When to use each approach

| Scenario | Approach |
| ---------- | ---------- |
| Reactor callback logic, error paths, cancellation, deadlines | Synchronous unit test |
| Thread-hopping and dispatch through `EventLoop` | Integration test |
| Message ownership crossing the thread boundary through `EventLoop` | Integration test |
| New reactor type | Both: one synchronous unit test file per `Delivery` plus one integration test per `Delivery` |
| Coroutine task and executor mechanics | Coroutine core tests |
| An awaitable over a reactor | Coroutine awaitable tests |

## Test coverage

### Coverage by RPC type

See the file list above for which file covers which RPC type.

| RPC Type | Reactor Class | Scenarios |
| ---------- | --------------- | ----------- |
| Unary (`GetFeature`) | `ActiveUnaryReactor` | Success, empty, error, failed response, cancel, deadline, concurrent |
| Server stream (`ListFeatures`) | `ActiveReadReactor` | Multiple/empty, error, cancel, concurrent, held stream |
| Client stream (`RecordRoute`) | `ActiveWriteReactor` | Points, overlapping writes, cancel, no done, write-flow hold |
| Bidirectional (`RouteChat`) | `ActiveBidiReactor` | Send/receive, interleaved, closes, cancel, held reads, write hold |
| EventLoop dispatch | N/A | `GetFeature`/`ListFeatures`/cancel dispatched through a real `EventLoop` |

The rows hold for both `Delivery` approaches, except where a scenario only exists under one of them: the deferred
consumer and the failed-response check are `kPush` scenarios, and `GetResponse()` before `OnDone` is a `kPull` one.
Every reading test file carries a regression test for the hold-before-hand-off ordering, run under both pacings
that hold a message and listed below.

The deferred-consumer scenario of the server-stream row is
`ListFeatures_DeferredConsumer_StreamCompletesWithoutConsumerAction` in
[active_read_reactor_push_test.cpp][read-push-test].
Its `read_ok` callback parks every received message and calls nothing back into the reactor, and the stream still runs
to completion, because `ReadPacing::kContinuous` re-arms each read itself. The same test checks that every parked
message kept its own value, so none of them alias a shared read buffer.

### Read pacing mode coverage

The two reading reactors take a `ReadPacing` as a template parameter next to `Delivery`. `ListFeatures` and
`RouteChat` therefore each have six cells: two approaches times three pacings. Each reading test file covers the
three pacings of its own approach, with GoogleTest typed tests over the pacing tags of
[read_pacing_test_support.h][pacing-support].

| Typed suite | Pacings | Runs |
| ------------- | --------- | ------ |
| `Active*ReactorPacingTest` | `kContinuous`, `kPerMessage`, `kTurnByTurn` | Behaviours independent of the pacing |
| `Active*ReactorHeldPacingTest` | `kPerMessage`, `kTurnByTurn` | Behaviours of a held message |

Each pacing is a named tag type, such as `pacing_test::TurnByTurn`. CTest names each case after it, as in
`ActiveReadReactorPacingTest.ListFeatures_EmptyStream_CompletesSuccessfully<pacing_test::TurnByTurn>`, so the ctest
output says which cell failed. GoogleTest's own output numbers the cases instead, and names the tag in its
`TypeParam` line.

The every-pacing suites run these scenarios:

- `ListFeatures`: multiple responses in order, empty stream, single feature, error mid-stream, immediate error,
  cancel, deadline, concurrent streams, `read_nok` on stream end, unbound `read_ok`.
- `RouteChat`: send/receive in order, interleaved locations, client closes first, server closes first, cancel,
  deadline, concurrent streams, unbound `read_ok`.

Their `read_ok` callbacks come from the fixture's `ConsumeEach()` helper. It hands each message to the test, then
consumes it the way the cell requires:

| Cell | What `ConsumeEach()` does |
| ------ | --------------------------- |
| `kPull`, `kContinuous` | Copies the lent message, since the next read overwrites it |
| `kPull`, `kPerMessage` or `kTurnByTurn` | Copies the message, then checks `GetResponse()` hands out the same one |
| `kPush`, `kContinuous` | Takes the owned message, and nothing more |
| `kPush`, `kPerMessage` or `kTurnByTurn` | Takes the owned message, then calls `ResumeRead()` |

Under `kPerMessage` the helper returns true, so these suites exercise the kept path. The message reaches the test
before the release, because the release arms the next read, whose callback could otherwise run first. The
`RouteChat` cases close the write side with `SendLastRequest()`. `RouteChat_ClientClosesFirst_ServerContinues`
is the exception: it calls `CloseRequestStream()` while echoes are still due, which checks that a message
released after the close still re-arms the read.

The declined path of `kPerMessage` is covered by the pull files' `ActiveReadReactorTest` and `ActiveBidiReactorTest`
cases written for the earlier contract. Under it every pull reactor had a `bool` `read_ok`, which `kPerMessage`
keeps unchanged, so those cases run on it as written, returning false.

The behaviours specific to some cells run as the held typed suites, or as `TEST_F`:

| Test | Cells | Asserts |
| ------ | ------- | --------- |
| `ListFeatures_Continuous_CompletesWithoutGetResponse` | Pull continuous | No `GetResponse()`, one lent storage |
| `ListFeatures_DeferredConsumer_StreamCompletesWithoutConsumerAction` | Push continuous | Parked messages intact |
| `RouteChat_DeferredConsumer_StreamCompletesWithoutConsumerAction` | Push continuous | The same, bidirectional |
| `ListFeatures_StreamStallsUntilGetResponse` | Pull held | One message per `GetResponse()`, `OnDone` pending |
| `ListFeatures_StreamStallsUntilResumed` | Push held | One message per `ResumeRead()`, `OnDone` pending |
| `ListFeatures_DuplicateGetResponseIsRejected` | Pull held | A second call returns false, swaps nothing |
| `ListFeatures_DuplicateResumeIsRejected` | Push held | A second call is rejected, not double-released |
| `ListFeatures_ResponseConsumedBeforeCallbackReturns_HoldsUntilReturn` | Pull held | Hold precedes hand-off |
| `ListFeatures_ResumedBeforeCallbackReturns_HoldsUntilReturn` | Push held | Hold precedes hand-off |
| `RouteChat_ResponseConsumedBeforeCallbackReturns_HoldsUntilReturn` | Pull held | Hold precedes hand-off |
| `RouteChat_ResumedBeforeCallbackReturns_HoldsUntilReturn` | Push held | Hold precedes hand-off |
| `RouteChat_ReadHoldDoesNotBlockWrites` | Push held | A write completes during a read hold |
| `RouteChat_ReleasedAfterCloseRequestStream_ReadsOn` | Both held | A release after a close re-arms the read |
| `ListFeatures_PerMessage_DeclinedMessagesReadOnWithoutResume` | Push per-message | False reads on unaided |
| `ListFeatures_PerMessage_MixedDecisions_HoldOnlyKeptMessages` | Push per-message | Only kept messages stall |
| `ListFeatures_PerMessage_DeclinedAfterResumeRead_ReleasesOnce` | Push per-message | No double release |
| `*_PerMessage_DeclinedAfter*_LeavesNextHoldAlone` | Both per-message | The next message's hold is left alone |

"Held" stands for both `kPerMessage` and `kTurnByTurn`, through the held typed suite. The last row is one test in
each reading file. It orders two `read_ok` callbacks explicitly, so a claim that takes the wrong hold fails on every
run. `RouteChat_ReleasedAfterCloseRequestStream_ReadsOn` guards the bidirectional reactor's close: a successful
`CloseRequestStream()` ends only the write side, so a message held across it must still re-arm the read.

Some cells must not offer a method at all, which no runtime test can show. Each reading test file opens with
`static_assert` checks instead, written with the `HasGetResponse` and `HasResumeRead` concepts of
[read_pacing_test_support.h][pacing-support]:

| Method | Exists only under |
| -------- | ------------------- |
| `GetResponse()` | `kPull`, with `kPerMessage` or `kTurnByTurn` |
| `ResumeRead()` | `kPush`, with `kPerMessage` or `kTurnByTurn` |

The same checks pin the `read_ok` signature: its parameter follows the approach, and it returns `bool` only under
`kPerMessage`. They also pin `kPerMessage` as the default pacing, in the generic reactors and in the RouteGuide
`ClientReactor` alike, so naming only the `Delivery` selects it.

### Write-side idle hold coverage

`ActiveBidiReactor` keeps a hold while its write stream is idle, and claiming that hold is the permission to write.
Four tests cover it, and each was mutation-verified except where noted.

| Test | Asserts |
| ------ | --------- |
| `RouteChat_OverlappingWrite_RejectedWhileHoldIsClaimed` | Accepted writes never exceed completed ones by one |
| `RouteChat_ConcurrentClaimants_KeepTheHoldAccountingIntact` | Two writers and the read stream's end share one claim |
| `RouteChat_WriteAfterReadStreamClosed_RejectedDeterministically` | Writes are refused once the read stream ends |
| `RouteChat_ReleaseWithNoHold_DoesNotLetOnDoneOverlapTheReaction` | An unclaimed release lets `OnDone` overlap |

The four live in [active_bidi_reactor_push_test.cpp][bidi-push-test], under `kContinuous`. The write side is identical
under both `Delivery` approaches, and the one place the two meet it, the `OnReadDone(false)` release, is exercised under
each: `RouteChat_DeadlineExceeded_PropagatesStatus` exists in both bidi files and doubles as the guard for that release,
since removing it hangs the test for its full timeout. `RouteChat_ConcurrentClaimants_KeepTheHoldAccountingIntact`
races two writer threads against the server ending the RPC, so `OnReadDone(false)` claims the hold to release it
while the writers claim it to write. Of three mutants run 200 times each, an unclaimed release and a non-atomic claim
in `SendRequest()` were caught in every batch of ten runs. A non-atomic claim in the release itself never was: its
window is too narrow for a test to hit, and no other test in the suite detects it. The non-re-arm after a terminal
write is not covered either, since `OnReadDone(false)` masks it.

### Write-flow hold coverage

`ActiveWriteReactor` holds the RPC for its whole write flow, and releases the hold when the write side ends. Five tests
cover it, in [active_write_reactor_push_test.cpp][write-push-test]. Two of them use the test service's early-ending
mode, `SetEndEarly()`, where the server ends the RPC as soon as it starts, so the client is idle when the status arrives.

| Test | Asserts |
| ------ | --------- |
| `RecordRoute_ServerEndsWhileIdle_OnDoneWaitsForWriteSide` | `OnDone` waits for the close, then has the status |
| `RecordRoute_ServerEndsWhileIdle_FailedWriteEndsWriteSide` | A failed write alone ends the write side |
| `RecordRoute_TryCancelWhileIdle_CompletesWithoutWriteSideAction` | `TryCancel()` alone completes an idle RPC |
| `RecordRoute_EndedWriteSide_RejectsEveryWriteCall` | After `SendLastRequest()`, every write and close is refused |
| `RecordRoute_TryCancelRacingWrites_CompletesOnce` | A cancel racing a writing thread ends the flow once |

The first one was mutation-verified: against the reactor without the write-flow hold, `OnDone` fired about 7 ms after
the server ended the RPC, while the application could still write. The write path is identical under both `Delivery`
approaches, so the tests run under `kPush` only. Since an unclosed write side keeps the RPC from completing, every
writer test now checks that its close is accepted, and `RecordRoute_OnWriteDone_FiresForEachWrite` waits for each
write's acknowledgement instead of sleeping, which a slow run under ThreadSanitizer outlasted.
`RecordRoute_NoDoneCallbackBound_RpcStillCompletes` binds no `done` slot, so it has no way to know when the reactor may
be destroyed, and ThreadSanitizer caught it destroying the reactor while a reaction still ran. It now runs a test-only
subclass of `ActiveWriteReactor` that signals once `OnDone()` has run, and destroys the reactor only then.

### Request submission coverage

Under `Submission::kReactorPulls` the writing reactors pull each request from the `next_request` callback and take no
write-side hold, so most tests check that the RPC ends on its own however it ends. The submission is independent of the
`Delivery` and of the `ReadPacing`, which the other files cover, so
[active_write_reactor_producer_test.cpp][write-producer-test] runs under `kPush` only, and
[active_bidi_reactor_producer_test.cpp][bidi-producer-test] under `kPull` with the default `kPerMessage` only. Both
start with compile-time checks: the write methods do not exist under `kReactorPulls`, and the `next_request` slot is a
`std::monostate` placeholder under `kApplicationPushes`.

| Test | Reactor | Asserts |
| ------ | --------- | --------- |
| `PullsEveryRequest_ThenClosesStream` | Both | One pull per request plus the `std::nullopt` one, all sent, OK status |
| `EmptySource_ClosesBeforeAnyWrite` | Both | A single pull, no write, OK status |
| `UnboundSlot_ClosesAtOnce` | Writer | An unbound slot closes the stream at once |
| `ServerEndsEarly_CompletesWithoutApplicationAction` | Writer | `OnDone` follows the server's error unprompted |
| `ServerFinishesFirst_CompletesWithoutApplicationAction` | Bidi | The same when the server finishes mid-stream |
| `TryCancelFromWriteDone_StopsPulling` | Both | No pull follows a `TryCancel()` from `write_done` |
| `TryCancelFromApplication_Completes` | Writer | `TryCancel()` ends an RPC whose source never runs dry |
| `HeldRead_DoesNotStallPulls` | Bidi | The source runs dry while the first answer is still held |

In `ServerEndsEarly_CompletesWithoutApplicationAction`, `OnDone` needs no application call, and no pull follows the
failed write.

The writer's first test also checks that each pull follows the `write_done` of the write before it, and that the first
pull runs on the thread that constructs the reactor.

### Teardown coverage

[reactor_teardown_test.cpp][teardown-test] tears reactors down while their RPC is still in flight, which is a
use-after-free unless `CancelAndWait()` has returned `true` first. Each test leaves outstanding a hold that an
application tearing down would never release, and checks that `CancelAndWait()` still sees `OnDone()` run.

| Test | Leaves outstanding |
| ------ | --------- |
| `Unary_MidRpc_CancelAndWaitThenDestroy` | An unanswered unary call, then destroys the reactor |
| `Unary_AfterOnDone_ReturnsAtOnce` | Nothing: the RPC has already ended |
| `Unary_FromDoneCallback_ReturnsAtOnce` | The `OnDone()` the call is made from |
| `Read_PullHeldMessageNeverPulled` | A message held under `kPull` and `kTurnByTurn` |
| `Read_PushKeptMessageNeverResumed` | A message kept under `kPush` and `kPerMessage` |
| `Write_IdleWriteSideNeverClosed` | The write-flow hold of an idle writer |
| `Write_ReactorPullsEndlessSource` | Nothing to release: a source that never runs dry |
| `Bidi_HeldEchoNeverResumed` | A held echo, and the idle-window hold with no read armed |
| `Bidi_KeptEchoNeverPulled` | The same under `kPull` and `kPerMessage` |
| `Bidi_ReactorPullsEndlessSource` | Nothing to release: a source that never runs dry |

Four mutants of `CancelAndWait()` were each caught. Without the held-message release, the four held-message tests time
out. With the writer cancelling only its context, the idle-writer test times out. Without the idle-window release, the
two bidi held-message tests time out. Without the wait, AddressSanitizer reports the use-after-free in the first test.

### Latest-only coverage

[latest_slot_test.cpp][latest-test] tests the realtime-reading pattern: `ActiveReadReactor` under `kPull` and
`kContinuous`, with a `LatestSlot` keeping only the newest message for a consumer slower than the stream. The slot's
own contract comes first, then a producer racing a consumer, then the reactor against an in-process server streaming
200 readings back to back. That server, and the single-threaded task queue standing in for the application's event
loop, are shared with the journal suite, in [continuous_use_support.h][continuous-support].

| Test | Checks |
| ------ | --------- |
| `FirstPut_AsksForANotification` | The first value asks for an event |
| `PutBeforeTake_ReplacesTheValue_AndAsksForNoNotification` | A value not yet taken is replaced, and counted |
| `Take_EmptiesTheSlot_SoTheNextPutAsksAgain` | `Take()` empties the slot, and the next value asks again |
| `MoveOnlyValue_IsMovedThrough` | An owned `kPush` message fits the slot |
| `ProducerAgainstNotifiedConsumer_KeepsEveryPromise` | 100000 values against a consumer on its own event loop |
| `BlockedConsumer_StreamRunsOn_AndOnlyTheLastReadingIsLeft` | The stream ends with nothing taken, one event asked for |
| `SlowConsumer_HandlesOnlyNewerReadings_AndEndsOnTheLast` | A slow consumer on its own event loop, as in the use case |

The racing tests check the same promises: at most one event waits in the queue, the consumer only moves forward, every
value is either taken or counted as replaced, and the last value is the last one taken. Five mutants of `LatestSlot`
were each caught by at least three tests: always asking for an event, never asking, not counting a replacement,
`Take()` leaving the value in place, and `Put()` keeping the oldest value.

### Journal coverage

[continuous_journal_test.cpp][journal-test] tests the ingestion pattern: `ActiveReadReactor` under `kPush` and
`kContinuous`, each owned message queued as its own task for a journal slower than the stream.

| Test | Checks |
| ------ | --------- |
| `BlockedJournal_StreamEndsWithEveryRecordQueued_ThenAllAreWrittenInOrder` | Ends with all queued, then all in order |
| `SlowJournal_WritesEveryRecordInOrder_AfterTheRpcEnded` | A slow journal on its own event loop, as in the use case |

Two mutants of `ActiveReadReactor` under `kContinuous` were each caught. Without the re-arm, both tests fail: the stream
stops after its first message. Taking a hold as `kTurnByTurn` does, the first test aborts, since the stream stalls and
not even a cancel can end it. Delivery in order, and of every message once, follows from the FIFO queue and from gRPC's
one read at a time, so the tests assert it rather than mutate it.

### Coroutine core coverage

[coro_task_test.cpp][coro-task-test] tests the task type and its owner. Most tests resume a suspended coroutine by
hand, and count the live instances of a `Tracked` parameter or local to show which frames still exist.

| Test | Checks |
| ------ | --------- |
| `Task_NotAwaited_BodyNeverStarts` | The start is lazy |
| `Task_Awaited_ReturnsTheValue` | The awaiter gets the value the body returned |
| `Task_NestedChain_ValuesFlowBackUp` | Each awaiter of a chain of three gets its child's value |
| `Task_MoveOnlyValue_IsMovedOut` | A `std::unique_ptr` is moved out to the awaiter |
| `Task_MillionSynchronousAwaits_StackDoesNotGrow` | A million children that never suspend run at one stack depth |
| `Task_ChildSuspends_ItsEndResumesTheAwaiter` | A child that suspends resumes its awaiter once it ends |
| `Task_BodyEndsOnAnotherThreadDuringStart_AwaiterResumedOnce` | 1000 bodies end on another thread, racing their start |
| `Task_DestroyedBeforeStart_DestroysItsFrame` | A task never awaited releases its frame and never runs |
| `Task_MovedFrom_OwnsNothing` | Only the task moved to destroys the frame |
| `Task_Finished_FrameLivesUntilTheTaskIsDestroyed` | The caller owns the finished frame until it destroys the task |
| `RootTasks_DestroyedWithSuspendedChain_DestroysEveryFrame` | The owner releases a suspended chain of two frames |
| `RootTasks_Spawn_RunsToTheFirstSuspension_ThenReleasesOnCompletion` | `Spawn()` starts at once, the end releases |
| `RootTasks_SeveralRoots_EachReleasedOnItsOwnCompletion` | Roots ending in any order, or at once, are each released |
| `Executor_ResumptionsFromOtherThreads_AllRunOnTheAppLoop` | 100 resumptions posted from other threads run on it |
| `Task_ExceptionEscapesTheBody_LogsAndTerminates` | An escaping exception is logged, then terminates |
| `CurrentTask_FollowsEachTaskAcrossItsSuspensions_AndIsNoneBetween` | Two tasks keep their own numbers |
| `CurrentTask_TaskSuspendedAgain_LeavesTheLoopUnderNoTask` | A task suspended again leaves the loop under no task |

`CurrentTask_FollowsEachTaskAcrossItsSuspensions_AndIsNoneBetween` checks that each of two tasks keeps its own number
across a resumption, and in a task it awaits too. Between them, the loop runs under no task.

The stack test measures the stack position of a plain function each child calls, since a coroutine's own locals live
in its heap frame. Under GCC below `-O2`, symmetric transfer alone grows the stack by about 128 bytes per await, and
that test overflows the stack. A child that ends without suspending therefore lets its awaiter go on without any
transfer, which the race test checks against a body ending on another thread at the same time.

[eventloop_executor_test.cpp][eventloop-executor-test] tests the executor of the applications against the real
EventLoop:

- `Executor_ResumptionsFromOtherThreads_AllRunOnTheApplicationThread` checks that 100 resumptions posted from other
  threads run on the EventLoop thread, the tests' application thread.
- `Executor_Post_ResumesInPostOrder_AfterEveryEventQueuedBefore` checks that resumptions follow the post order, and
  that an event queued between two posts runs between them.

Five mutants of `coro_task.h` were each caught:

| Mutant | Caught by |
| ------ | --------- |
| Transferring to the awaiter even when the body ended inside its start | The stack test, and five more tests |
| Not destroying a frame with its task | The frame tests |
| Not destroying the owner's suspended roots | The suspended chain test |
| Not removing an ended root from its owner | Every spawning test, on the double destruction |
| A plain `bool` for the start's hand-over flag | ThreadSanitizer, in the race test |

Under the first mutant, the stack test overflows under GCC, and the five other tests fail under AddressSanitizer.

`CurrentTask` gives the task number the coroutine applications log as `[task#N]`. Four mutants of it were each caught
by one of the two `CurrentTask` tests: not setting it in `Spawn()`, not restoring it in `ResumeHandover::Resumed()`, not
clearing it when a task suspends through the hand-over, and not clearing it when a root task ends.

### Unary awaitable coverage

[reactor_coro_unary_test.cpp][coro-unary-test] runs `GetFeature` through `coro::Unary`. Its service can hold an answer
until the test releases it, or until the client cancels, so a coroutine that awaits `Finish()` before the answer is
known to suspend.

| Test | Checks |
| ------ | --------- |
| `GetFeature_PullOk_ReturnsOwnedResponseOnTheLoop` | One resumption, on the loop, with the response swapped out |
| `GetFeature_PushOk_ReturnsOwnedResponseOnTheLoop` | The same under `kPush`, with the message handed over |
| `GetFeature_PullServerError_ReturnsTheStatusAndAnEmptyResponse` | A failed RPC is a status, with an empty response |
| `GetFeature_PushServerError_ReturnsTheStatusAndAnEmptyMessage` | Under `kPush`, the message is never null |
| `GetFeature_TryCancel_FinishReturnsCancelled` | A cancel ends a held RPC, and `Finish()` returns `CANCELLED` |
| `GetFeature_FrameDestroyedMidRpc_CancelsAndWaitsForOnDone` | Frees nothing under gRPC, and posts nothing |

Whether `Finish()` suspends depends on whether the RPC ended first, which a test can only arrange by waiting on
timing. So the hand-over that decides it, `ResumeHandover` of [coro_task.h][coro-task], is tested directly in
[coro_task_test.cpp][coro-task-test], with an executor that records the coroutines it is given:

| Test | Checks |
| ------ | --------- |
| `EndedFirst_DoesNotSuspend_AndPostsNothing` | An operation ended first lets the coroutine go on, and posts nothing |
| `AwaitedFirst_Suspends_AndTheEndPostsItOnce` | A coroutine awaiting first suspends, and the end posts it once |
| `Abandoned_TheEndPostsNothing` | Once abandoned, as by a destroyed `Unary`, the end posts nothing |

Six mutants of `coro::Unary` were each caught:

| Mutant | Caught by |
| ------ | --------- |
| The destructor not abandoning the waiter, so the done callback posts a destroyed frame | The destroyed frame test |
| `Abandon()` doing nothing | The abandoned hand-over test, and the destroyed frame test |
| The destructor not waiting for `OnDone()` before destroying the reactor | The destroyed frame test |
| Always suspending in `Finish()` | The hand-over test of an operation ended first |
| Never posting the waiter | The hand-over test of a coroutine awaiting first, and every RPC test that suspends |
| Not keeping the message the done callback hands over under `kPush` | Both `kPush` tests |

### Naming convention

Test names have three underscore-separated segments: `RpcMethod_Scenario_Result`. The first and
third segments identify something different in each suite, because each level exercises a
different class.

| Suite | Exercises | `RpcMethod` segment | `Result` segment |
| ------- | ----------- | ---------------------- | ------------------- |
| Unit (`Active*ReactorTest`) | The generic, shape-templated reactors in [reactor_client.h][reactor-client] | A vehicle RPC method. The file and fixture name already identify the shape under test. | The behavior asserted (`ReturnsFeature`, `TerminatesStream`, ...). Varies freely per test. |
| Integration (`ClientReactorIntegrationTest`) | The concrete per-RPC specializations in [reactor_client_routeguide.h][reactor-client-routeguide] | The real identifier. One fixture mixes several RPC methods. | Always `DispatchesToEventLoop`. |

The integration suite only verifies that dispatch happened. The RPC's business outcome is
already covered by the unit suites.

Example: the unit suite uses `GetFeature_ValidPoint_ReturnsFeature`. The integration suite uses
`GetFeature_Pull_ValidPoint_DispatchesToEventLoop` and `GetFeature_Push_ValidPoint_DispatchesToEventLoop`: one
fixture holds both `Delivery` approaches there, so their names carry it as an extra segment after the RPC method.
The unit suites need no such segment, since each file is dedicated to one approach. Their typed suites carry the
pacing as a suffix instead, as `ActiveReadReactorPacingTest.ListFeatures_EmptyStream_...<pacing_test::PerMessage>`.

## Test infrastructure

### Shared fixture

[route_guide_test_fixture.h][test-fixture] provides `RouteGuideTestFixtureBase<ServiceT>`, a
template test fixture. It starts an in-process gRPC server on a dynamic port, and creates a
channel and stub connected to it. `ServiceT` is the fake `routeguide::RouteGuide::CallbackService`
implementation each test suite supplies, since each exercises a different RPC method.

### Read pacing support

[read_pacing_test_support.h][pacing-support] holds what the reading reactors' tests share:

| Item | Use |
| ------ | ----- |
| `Continuous`, `TurnByTurn`, `PerMessage` | One named tag type per pacing, each a `PacingTag` |
| `AllPacings`, `HeldPacings` | Type lists for `TYPED_TEST_SUITE` |
| `KeepHeld<kPacing>()` | What `read_ok` returns to keep a message held, for both held pacings |
| `HasGetResponse`, `HasResumeRead` | Concepts for the compile-time API checks |

### In-process server

Each test file defines its own `TestRouteGuideService`, a fake implementation of the RPC method
under test with configurable responses and error injection, so scenarios are deterministic and do
not depend on the real RouteGuide feature database. The server binds to `localhost:0` (dynamic
port) to avoid conflicts between parallel test runs.

## Continuous integration

[ci.yml][ci-workflow] runs the test suite as a `build-test-alpine` job with three matrix variants, each an
Alpine container with a different compiler and sanitizer combination.
`fail-fast` is disabled, so a failure in one variant does not block the others from reporting their
own result. Each variant installs its own apk packages and rebuilds EventLoop from source
independently rather than sharing a container image, since a prebuilt-image approach was tried and
measured slower: the image big enough to cover both toolchains costs more to pull per job (no
persistent Docker layer cache between GitHub-hosted runner jobs) than the apk install and
EventLoop's own build cost in the first place, and it added a build-the-image job as a hard
serialization step in front of all three variants.

`ci.yml`, `coverage.yml` and `static-analysis.yml` also run every Monday on `master`, without any
commit. Their inputs change on their own: openSUSE Tumbleweed's rolling packages, the vcpkg
revision cloned for EventLoop, and the runner image. A weekly run reports that breakage before the
next pull request runs into it.

| Matrix variant | Compiler | Sanitizers | Publishes JUnit report |
| ----- | ---------- | ------------ | ------------------------- |
| `gcc-debug` | GCC | None | Yes |
| `clang-debug` | Clang | None | No |
| `clang-asan-ubsan` | Clang | AddressSanitizer, UndefinedBehaviorSanitizer | No |

`gcc-debug` is the only variant that publishes a per-suite JUnit report to its job summary, since
it is the baseline configuration. The other variants are pass/fail gates: a failure needs ctest's
raw output and stack trace, not a results table.

### Why Clang for the sanitizer job, not GCC

Alpine's GCC package builds with `--enable-libsanitizer`, but its libsanitizer interceptors (for
example `sendmsg`) are written and tested against glibc's struct layouts. They crash against musl
before this project's own code runs. Clang's `compiler-rt` is the sanitizer runtime Alpine's own
ecosystem maintains musl support for, so the sanitizer job uses Clang exclusively.

### Why ThreadSanitizer runs only in sanitizers.yml

ThreadSanitizer was first tried in `ci.yml`, against the distributions' uninstrumented gRPC, Abseil
and c-ares, and dropped. TSan only sees the synchronization of code it instruments, so every
suppression strategy attempted (`race:` entries, `called_from_lib:`,
`ignore_noninstrumented_modules=1`) either never converged, or hid enough synchronization to
report races that do not exist. [sanitizers.yml][sanitizers-workflow] instruments that whole stack
instead, as described below.

### Sanitizers with an instrumented dependency stack

[sanitizers.yml][sanitizers-workflow] builds every vcpkg dependency with the sanitizer under test:
the gRPC bundle (gRPC, Protobuf, Abseil, c-ares, RE2), OpenSSL, zlib, spdlog, gflags, GoogleTest
and EventLoop. It has one matrix entry per sanitizer:

| Entry | Triplet | Preset | Sanitizers |
| ----- | ------- | ------ | ---------- |
| `asan` | `x64-linux-clang-asan` | `vcpkg-clang-asan` | ASan on the whole stack, UBSan on this project |
| `tsan` | `x64-linux-clang-tsan` | `vcpkg-clang-tsan` | ThreadSanitizer |

Unlike the `ci.yml` sanitizer variants, headers and libraries agree on every sanitizer-dependent
layout, and a report can point inside gRPC or EventLoop, not only at this project's code. A job
step checks that each installed library references the sanitizer runtime, since an
uninstrumented dependency still links and passes under ASan.

A cold build of one stack takes 35 to 50 minutes. The vcpkg binary cache keeps it to a few minutes
afterwards. It uses vcpkg's `files` provider, one zip per package and ABI hash, kept in the GitHub
Actions cache: no extra tool is needed, and it is the same mechanism vcpkg uses for its local cache
(`~/.cache/vcpkg/archives`). Each run restores its branch's latest archive, or `master`'s, so a
changed port rebuilds only itself and the ports that depend on it, and saves a new archive only when
a package was built or pruned. Every run marks the packages it installed as used, and a package no
run saved as used for 30 days is deleted. Pruning by age, not by one run's use, keeps the packages of
branches still on older ports. GitHub itself deletes an archive left unused for 7 days, and the
repository's caches are capped at 10 GB. vcpkg is checked out at the registry baseline of
`vcpkg-configuration.json`, since its helper scripts are part of every ABI hash. The workflow also
runs every Monday on `master`, to catch runner image and toolchain updates, which also keeps
`master`'s archive from expiring.

This workflow's TSan entry found that EventLoop pushed onto its event queue without the mutex its
loop reads that queue under. Upstream fixed it, along with a race on its callback map, and the
overlay port pins a commit that carries both fixes.

### ASan runtime options

The sanitizer job sets `ASAN_OPTIONS` on the `Test` step (see [ci.yml][ci-workflow] for the
per-option rationale), enabling checks that are off by default upstream: invalid pointer pair
comparisons, a larger use-after-free quarantine, strict C-string null-termination checks,
stack-use-after-return detection, leak detection, and static initialization order checking.

Stack traces from this job are symbolized via `llvm22-symbolizer`, installed alongside Clang and
symlinked to the unversioned `llvm-symbolizer` name the sanitizer runtime expects. Neither the
`clang` nor the `compiler-rt` Alpine package ships a symbolizer binary.

### Static analysis

[static-analysis.yml][static-analysis-workflow] runs `lint` (pre-commit), `clang-tidy`, and
`cppcheck` as their own workflow, separate from the build/test matrix above, on every push. A
`build` job compiles once and shares `compile_commands.json` and the generated protobuf/gRPC
headers with both analysis tools, which run as one matrix job's two entries rather than as
separate jobs. Each entry has two modes: a `pull_request`, `push` or weekly `schedule` run
annotates diagnostics without failing the job, and a `workflow_dispatch` run fails on any
diagnostic for a deliberate, whole-repo pass. clang-tidy only analyses `applications/`; the
supporting `common/`, `protobuf_utils/` and `rg_service/` directories are left out. See
`static-analysis.yml` for why it is a separate workflow, and `.clang-tidy` for clang-tidy's check
selection.

## Adding new tests

### 1. Choose the test file

Add the test to the file matching the reactor type under test. For a new reactor type, add both a
synchronous unit test file and a case in the EventLoop integration test.

### 2. Follow existing patterns

Name the test per the [naming convention](#naming-convention) above.

```cpp
TEST_F(ActiveUnaryReactorTest, NewTest_Scenario_ExpectedBehavior) {
  // Configure test service
  test_service_.SetGetFeatureResponse(expected);

  // Set up synchronization
  std::promise<grpc::Status> done_promise;
  std::future<grpc::Status> done_future = done_promise.get_future();

  // Create callbacks
  routeguide::GetFeature::Callbacks<kDelivery> cbs;
  cbs.done = [&done_promise](...) { done_promise.set_value(status); };

  // Create reactor
  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor<kDelivery>>(
      *stub_, CreateClientContext(), request, std::move(cbs));

  // Wait and verify
  auto result = done_future.wait_for(std::chrono::seconds(5));
  ASSERT_EQ(result, std::future_status::ready);
  EXPECT_TRUE(done_future.get().ok());
}
```

Always set a timeout on futures (`wait_for(std::chrono::seconds(5))`). A reactor bug that never
completes the promise would otherwise hang the test indefinitely.

### 3. Extend the test service if needed

Add new configuration methods to the file's `TestRouteGuideService` for new scenarios.

### 4. Run the new test

```bash
cmake --build cmake-build-vcpkg-debug-gcc --target active_unary_reactor_pull_test
ctest --test-dir cmake-build-vcpkg-debug-gcc -R "NewTest"
```

## Troubleshooting

### Test timeout

If tests hang, check:

1. Server started correctly (dynamic port assigned)
2. EventLoop running, for `client_reactor_integration_test`
3. Promise set in all callback paths

### Flaky tests

`TryCancel` tests may return either `CANCELLED` or `OK` depending on timing. This is expected: the
test verifies `OnDone()` was called, not the specific resulting status.

### CTest shows "No tests found"

Ensure `enable_testing()` is in the root `CMakeLists.txt` and reconfigure:

```bash
cmake --preset vcpkg-gcc-debug
cmake --build cmake-build-vcpkg-debug-gcc
ctest --test-dir cmake-build-vcpkg-debug-gcc
```

<!-- Reference links -->
[ci-workflow]: /.github/workflows/ci.yml
[static-analysis-workflow]: /.github/workflows/static-analysis.yml
[sanitizers-workflow]: /.github/workflows/sanitizers.yml
[reactor-client]: /applications/reactor/reactor_client.h
[reactor-client-routeguide]: /applications/reactor/reactor_client_routeguide.h
[unary-pull-test]: /applications/reactor/tests/active_unary_reactor_pull_test.cpp
[unary-push-test]: /applications/reactor/tests/active_unary_reactor_push_test.cpp
[read-pull-test]: /applications/reactor/tests/active_read_reactor_pull_test.cpp
[read-push-test]: /applications/reactor/tests/active_read_reactor_push_test.cpp
[write-pull-test]: /applications/reactor/tests/active_write_reactor_pull_test.cpp
[write-push-test]: /applications/reactor/tests/active_write_reactor_push_test.cpp
[bidi-pull-test]: /applications/reactor/tests/active_bidi_reactor_pull_test.cpp
[bidi-push-test]: /applications/reactor/tests/active_bidi_reactor_push_test.cpp
[write-producer-test]: /applications/reactor/tests/active_write_reactor_producer_test.cpp
[bidi-producer-test]: /applications/reactor/tests/active_bidi_reactor_producer_test.cpp
[teardown-test]: /applications/reactor/tests/reactor_teardown_test.cpp
[latest-test]: /applications/reactor/tests/latest_slot_test.cpp
[journal-test]: /applications/reactor/tests/continuous_journal_test.cpp
[continuous-support]: /applications/reactor/tests/continuous_use_support.h
[coro-task-test]: /applications/reactor/tests/coro_task_test.cpp
[eventloop-executor-test]: /applications/reactor/tests/eventloop_executor_test.cpp
[coro-task]: /applications/reactor/coro_task.h
[reactor-eventloop]: /applications/reactor/reactor_eventloop.h
[coro-unary-test]: /applications/reactor/tests/reactor_coro_unary_test.cpp
[reactor-coro]: /applications/reactor/reactor_coro.h
[integration-test]: /applications/reactor/tests/client_reactor_integration_test.cpp
[callback-smoke-test]: /applications/callback/tests/callback_smoke_test.sh
[smoke-matrix-cmake]: /applications/tests/CMakeLists.txt
[smoke-matrix-cell]: /applications/tests/smoke_matrix_cell.sh
[smoke-matrix-compare]: /applications/tests/smoke_matrix_compare.sh
[test-fixture]: /applications/reactor/tests/route_guide_test_fixture.h
[pacing-support]: /applications/reactor/tests/read_pacing_test_support.h
