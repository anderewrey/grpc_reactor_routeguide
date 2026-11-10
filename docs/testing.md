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

[reactor_coro_reader_test.cpp][coro-reader-test] tests `coro::Reader`, the reader without a buffer, the same way. The
re-armed hand-over inside it is tested on its own in coro_task_test.cpp. Both are described in
[reader coverage](#reader-coverage).

[reactor_coro_writer_test.cpp][coro-writer-test] tests `coro::Writer`, the awaited writes, and `coro::Source`, the range
the reactor pulls, the same way. Both are described in [writer coverage](#writer-coverage).

[reactor_coro_bidi_test.cpp][coro-bidi-test] tests `coro::Bidi`, the bidirectional stream, read by one coroutine and
written by another, joined by `coro::WhenAll()`, the same way. `WhenAll()` itself is tested on its own in
coro_task_test.cpp. Both are described in [bidirectional join coverage](#bidirectional-join-coverage).

[reactor_coro_halt_test.cpp][coro-halt-test] halts the real EventLoop with every awaitable kind in flight, then
destroys their frames, as described in [coroutine teardown coverage](#coroutine-teardown-coverage).

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
a non-zero exit status, a sanitizer report from either process, or a failed RPC status in the client's log. It runs
the client with `--point_delay_ms=5`, so the sync and callback clients' RecordRoute waits 5 ms between points rather
than gRPC's example's random 0.5 to 1.5 s. It then
writes the client's digest: one line per logger, with the number of messages the scenario received, counted from its
`read_ok` and `skipped` lines when it logs any, else from its `RESPONSE` lines. The RouteChat loggers make one `chat`
line instead, the replies of all of them together. Both servers answer each note with every note already left at its
location, by any RPC, and store the new one, so each pair of notes at one location makes exactly one reply. What one
chat receives depends on how the chats interleave; their total does not. The cell checks that total against the
replies the server owed: it counts the notes the server's log shows it read, per location, and fails unless the chats
received n(n-1)/2 replies for each location's n notes. A chat whose logger logs one line per note it sends,
`typed    |` or `sent     |`, also makes a `notes:<logger>` line, the notes it sent.

[smoke_matrix_compare.sh][smoke-matrix-compare] then checks that:

- each client's digest is the same against both servers;
- each logger of a coroutine twin application, named after its callback parent's logger with a `.coro` suffix, such
  as `GetFeature.pull.coro` for `GetFeature.pull`, counts what the parent's logger counts, against the same server.
  Every `.coro` logger must find its parent, and every twin application must have one. A `notes:` line of a `.coro`
  chat, such as `notes:ConvoyChat.coro`, is compared the same way, with `notes:ConvoyChat`.

A scenario added to an application, a coroutine twin included, needs nothing in the matrix. A new application is one
line in `SMOKE_SERVERS` or `SMOKE_CLIENTS`, and a new twin application one more line in `SMOKE_TWINS`. A coroutine twin
of a RouteChat scenario goes into its application's `chat` line, which depends on every chat the application runs, so
it is compared across servers and with the replies owed, not with the parent's: the twin is compared with its parent
on the notes it sends instead, and its parent must log a line per note for that.

The cells hold the same `RESOURCE_LOCK` on port 50051 as the callback smoke test, so they run one at a time. The
servers are killed at the end of each cell, so their exit-time checks do not run, while every client runs to its end,
LeakSanitizer and coverage profiles included.

### Reactor demo applications

The two reactor applications, `route_guide_active_reactor_client` for the variants and
`route_guide_reactor_usecases` for the use cases, and their coroutine twins, `route_guide_active_reactor_client_coro`
and `route_guide_reactor_usecases_coro`, have no unit tests of their own: the [smoke matrix](#smoke-matrix) runs them.
Each application exits on its own: `rg_demo::RunUntilDone()` halts the event loop once no `rg_demo::Activity` is
left, every reactor destroyed after its `OnDone()`. An `Activity` is a move-only token, and the scenario hands the one
`Activity::Begin()` returns in `Start()` to whatever owns its end, so it ends on every path out:

- a callback scenario keeps it in a member, and its last handler moves it into a local, `const Activity
  activity{std::move(activity_)};`, which ends the scenario when the handler returns, after the reactor is destroyed;
- a coroutine scenario passes it to its coroutine as a by-value parameter, `Run(point, Activity::Begin())`, which the
  frame holds until the coroutine ends, so it needs no line at the end at all.

A scenario that never takes its `Activity` back keeps the application running until CTest's 300 s timeout, and one
that never begins it lets the application halt before it ends, which `smoke_matrix.compare` reports as short counts.
Every scenario asserts which thread runs
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
| `Read_MessageHeldAfterTheCancel` | A message kept after the cancel looked for holds, see below |
| `Write_IdleWriteSideNeverClosed` | The write-flow hold of an idle writer |
| `Write_ReactorPullsEndlessSource` | Nothing to release: a source that never runs dry |
| `Bidi_HeldEchoNeverResumed` | A held echo, and the idle-window hold with no read armed |
| `Bidi_KeptEchoNeverPulled` | The same under `kPull` and `kPerMessage` |
| `Bidi_EchoHeldAfterTheCancel` | An echo held after the cancel looked for holds, see below |
| `Bidi_ReactorPullsEndlessSource` | Nothing to release: a source that never runs dry |

Four mutants of `CancelAndWait()` were each caught. Without the held-message release, the four held-message tests time
out. With the writer cancelling only its context, the idle-writer test times out. Without the idle-window release, the
two bidi held-message tests time out. Without the wait, AddressSanitizer reports the use-after-free in the first test.

The two `...AfterTheCancel` tests land `CancelAndWait()` on a message whose `OnReadDone()` has begun but not yet
published its hold. A test-local reactor, built as the RouteGuide one is, stops its first `OnReadDone(true)` at a gate
before the base class sees the message. The test calls `CancelAndWait()` once, without waiting, so it finds nothing to
release, then opens the gate: `read_ok` keeps the message, and the test waits for `OnDone()` through the done callback.
A second `CancelAndWait()` would release the late hold itself and hide the bug.

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
- `After_ResumesOnTheApplicationThread_OnceTheDelayIsOver` checks that `After()` resumes a coroutine no sooner than
  its delay, on the EventLoop thread, and under its own task number, which the loop does not run meanwhile.

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

`WhenAll()`'s tests are in the same file, under `CoroWhenAllTest`, and described in
[bidirectional join coverage](#bidirectional-join-coverage).

### Unary awaitable coverage

[reactor_coro_unary_test.cpp][coro-unary-test] runs `GetFeature` through `coro::Unary`. Its service can hold an answer
until the test releases it, or until the client cancels, so a coroutine that awaits `Finish()` before the answer is
known to suspend.

| Test | Checks |
| ------ | --------- |
| `GetFeature_PullOk_ReturnsOwnedResponseOnTheLoop` | One resumption, on the loop, with the response swapped out |
| `GetFeature_PushOk_ReturnsOwnedResponseOnTheLoop` | The same under `kPush`, moved out of the message handed over |
| `GetFeature_PullServerError_ReturnsTheStatusAndAnEmptyResponse` | A failed RPC is a status, with an empty response |
| `GetFeature_PushServerError_ReturnsTheStatusAndAnEmptyMessage` | The same under `kPush` |
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

### Reader coverage

[reactor_coro_reader_test.cpp][coro-reader-test] runs `ListFeatures` through `coro::Reader`, under both deliveries.
Its service streams numbered features, then finishes OK, fails after a given number, or holds the stream until the
client cancels it, so a coroutine waiting in `Read()` is known to wait.

| Test | Checks |
| ------ | --------- |
| `ListFeatures_PullReadsEveryMessage_ThenNothing_ThenFinishOk` | Each message owned, in order, on the loop, then none |
| `ListFeatures_PushReadsEveryMessage_ThenNothing_ThenFinishOk` | The same under `kPush`, each moved out |
| `ListFeatures_EmptyStream_FirstReadGivesNothing` | An empty stream ends at the first `Read()` |
| `ListFeatures_PullServerErrorMidStream_ReadsWhatCameThenFinishGivesTheError` | An error is a status |
| `ListFeatures_PushServerErrorMidStream_ReadsWhatCameThenFinishGivesTheError` | The same, `kPush` |
| `ListFeatures_PullReadsAhead_TheNextMessageArrivesDuringAPause` | Taking a message arms the next read at once |
| `ListFeatures_PushReadsOnlyWhenAsked_NothingArrivesDuringAPause` | Nothing is read until the next `Read()` |
| `ListFeatures_PullTryCancel_WaitingReadGivesNothing_FinishGivesCancelled` | A cancel ends a waiting `Read()` |
| `ListFeatures_PushTryCancel_WaitingReadGivesNothing_FinishGivesCancelled` | The same under `kPush` |
| `ListFeatures_FrameDestroyedWhileReadWaits_CancelsAndWaitsForOnDone` | Frees nothing under gRPC, and posts nothing |
| `ListFeatures_FrameDestroyedWithAMessageHeld_ReleasesTheHold` | A held message no longer stalls the teardown |
| `ListFeatures_PullOnDoneBeforeFinish_FinishGivesTheStatus` | `OnDone()` first: `Finish()` goes on with the status |
| `ListFeatures_PushOnDoneBeforeFinish_FinishGivesTheStatus` | The same under `kPush` |
| `ListFeatures_PullFinishBeforeOnDone_FinishWaitsForTheStatus` | `Finish()` first: it waits for `OnDone()` |
| `ListFeatures_PushFinishBeforeOnDone_FinishWaitsForTheStatus` | The same under `kPush` |
| `ListFeatures_PullFinishMidStream_EndsTheRpcCancelled` | An early `Finish()` cancels, releasing the read-ahead |
| `ListFeatures_PushFinishMidStream_EndsTheRpcCancelled` | The same under `kPush`, releasing the message given |
| `ListFeatures_PullFinishAfterTheLastMessage_EndsTheRpcCancelled` | The same once every message is given |
| `ListFeatures_PushFinishAfterTheLastMessage_EndsTheRpcCancelled` | The same under `kPush` |
| `ListFeatures_PullFinishAfterTheLastMessageOfAFinishedStream_GivesOk` | The server's status, all messages read |
| `ListFeatures_PushFinishAfterTheLastMessageOfAFinishedStream_GivesOk` | The same under `kPush` |

The two read-ahead tests tell the deliveries apart. The coroutine reads one message, pauses the loop thread for 200
milliseconds, cancels, then reads what is left. Under `kPull`, the second message arrived during the pause, since
taking the first one armed its read, so it is still given after the cancel. Under `kPush`, nothing was read during
the pause, so the read the next `Read()` arms fails, and gives nothing. Of these two, only `kPull`'s outcome depends
on the pause.

The last test destroys a frame suspended outside `Read()`, while the reactor holds the stream for the message it gave
under `kPush`. No read is armed then, and the hold suppresses `OnDone()`, so the destruction must release it. Otherwise
it waits the 5 seconds of `kOnDoneTimeout`, then leaks the reactor, which the test's time limit and LeakSanitizer
report.

The four ordering tests order the end of the last read and `OnDone()` against `Finish()`, the two events the reader hands
over separately. A pause of the loop thread before `Finish()` lets `OnDone()` run first. A test reactor whose
`OnDone()` sleeps 300 milliseconds before it runs makes `Finish()` come first. Should the timing not hold, both orders
still give the status, so these tests can lose their power, never fail on correct code.

The first four early `Finish()` tests stop reading before the end, on a stream the server holds open after three
messages, so the cancel `Finish()` sends is the only way it can end. Before `Finish()` ended the RPC itself, each one
tripped its assert in a debug build, and waited forever with asserts compiled out. The last two read every message of
a stream the server finishes OK, and pause 200 milliseconds before `Finish()`, so the server's status has arrived:
the cancel then changes nothing, and `Finish()` gives OK. Stopping after one message of that finished stream gives
`CANCELLED` instead, since the client has not received the status behind the unread messages yet. Both held in 100
local runs each, and the OK tests in 50 more with every core busy.

Whether a `Read()` suspends depends on whether its read ended first, which a test can only arrange by waiting on
timing. So the re-armed hand-over is tested directly in [coro_task_test.cpp][coro-task-test]:

| Test | Checks |
| ------ | --------- |
| `Rearmed_EachAwaitSuspends_AndEachEndPostsItOnce` | Three waits in a row each suspend, and each end posts once |
| `Rearmed_EndedFirstThenAwaitedFirst_EachDecidedOnItsOwn` | Each wait decides whether to suspend on its own |
| `CurrentTask_RearmedHandover_RestoresTheTaskAtEachResumption` | Each resumption runs the task, none between |

A `Rearm()` that does nothing, leaving the hand-over ended, was caught by both re-armed hand-over tests: the second wait
goes on without suspending, and its end posts nothing.

Seven mutants of `coro::Reader` and `EventLoopExecutor::After()` were each caught, run in CI against the in-process
server:

| Mutant | Caught by |
| ------ | --------- |
| No `Rearm()` before `ResumeRead()`, under `kPush` | Both `kPush` tests that read the whole stream |
| `Rearm()` after `GetResponse()` instead of before, under `kPull` | `PullReadsEveryMessage`, waiting for a lost end |
| Arming the next read as the message is taken, under `kPush` | `PushReadsOnlyWhenAsked` |
| The destructor not abandoning the read hand-over | `FrameDestroyedWhileReadWaits`, on the extra post and ASan |
| `OnDone()` ending the read hand-over instead of its own | Every test that awaits `Finish()` |
| The destructor destroying the reactor without waiting for `OnDone()` | `FrameDestroyedWithAMessageHeld`, under ASan |
| `After()` not restoring the task number | `After_ResumesOnTheApplicationThread_OnceTheDelayIsOver` |

The second mutant opens a window, between `GetResponse()` arming the next read and the late `Rearm()`, in which an end
of read is lost. A test catches it only when a message arrives in that window, which no test can arrange without
timing: it failed in the run above, and may pass in another. Re-arming first is what closes the window, and the
re-armed hand-over tests show that an end which comes before the await is kept.

Two more mutants share one hand-over between the reads and `OnDone()`, as the reader would without `done_`. Each was
run 50 times against each ordering test, locally:

| Mutant | Caught by |
| ------ | --------- |
| `Finish()` re-arms the read hand-over, then waits on it | Always: both `OnDoneBeforeFinish`, `PullFinishBeforeOnDone` |
| `Finish()` waits on the read hand-over as it is | Always: `PushFinishBeforeOnDone`. 33/50: `PullOnDoneBeforeFinish` |

The first loses an end that came before `Finish()`, and waits forever, or trips `Rearm()`'s assert. The second
returns at once, since the last read left the hand-over ended, with a status `OnDone()` has not set yet.

Three more mutants of the early `Finish()`, each run 5 times against each of its tests, locally:

| Mutant | Caught by |
| ------ | --------- |
| `Finish()` only calls `TryCancel()` | Every time: both `MidStream` tests, `PushFinishAfterTheLastMessage` |
| `CancelAndRelease()` releasing no held message | The same three, `PullFinishMidStream` 4 times of 5 |
| `CancelAndRelease()` not raising `tearing_down_` | `ReactorTeardownTest.Read_MessageHeldAfterTheCancel` |

`PullFinishAfterTheLastMessage` holds no message, only a read armed for the next one, which the cancel ends, so it
catches none of them, and shows that a cancel alone is enough there. The third mutant opens the race of a message
arriving as the cancel is sent, which [the teardown tests](#teardown-coverage) land deterministically.

### Writer coverage

[reactor_coro_writer_test.cpp][coro-writer-test] runs `RecordRoute` through `coro::Writer` under
`Submission::kApplicationPushes`, and through `coro::Source` under `Submission::kReactorPulls`, under both deliveries.
Its service records the latitude of every point it reads, and finishes once the client closes its stream, with a
summary counting them. It can instead fail once it has read a given number, or hold the RPC after the close until the
client cancels it.

| Test | Checks |
| ------ | --------- |
| `RecordRoute_PullWritesEveryPoint_ThenFinishGivesTheSummary` | Each write resumes on the loop, then the summary |
| `RecordRoute_PushWritesEveryPoint_ThenFinishGivesTheSummary` | The same under `kPush`, the summary moved out |
| `RecordRoute_NoWrite_CloseThenFinishGivesAnEmptySummary` | A stream closed with no write |
| `RecordRoute_WriteCompletedBeforeTheAwait_GoesOnWithoutSuspending` | No post, and the next write still works |
| `RecordRoute_PullServerError_WriteGivesFalse_FinishGivesTheError` | A `Write()` gives false, then the next at once |
| `RecordRoute_PushServerError_WriteGivesFalse_FinishGivesTheError` | The same under `kPush` |
| `RecordRoute_WriteAfterTheServerErrorArrived_ThatWriteGivesFalse` | The failed write itself gives false |
| `RecordRoute_TryCancel_WriteGivesFalse_FinishGivesCancelled` | A cancel refuses the next write |
| `RecordRoute_PullFinishBeforeClose_EndsTheRpcCancelled` | An early `Finish()` cancels the RPC the close holds open |
| `RecordRoute_PushFinishBeforeAnyWrite_EndsTheRpcCancelled` | The same with no write, under `kPush` |
| `RecordRoute_FinishBeforeCloseAfterTheServerError_GivesTheError` | The server's status, once it has arrived |
| `RecordRoute_FrameDestroyedMidWrite_CancelsAndWaitsForOnDone` | Frees nothing under gRPC, and posts nothing |
| `RecordRoute_FrameDestroyedBetweenWrites_ReleasesTheWriteFlow` | The open write flow no longer stalls the teardown |
| `RecordRoute_PullSourceSendsEveryElement_ThenFinishGivesTheSummary` | Every element, in order, then the summary |
| `RecordRoute_PushSourceSendsEveryElement_ThenFinishGivesTheSummary` | The same under `kPush` |
| `RecordRoute_SourceOfAnEmptyRange_FinishGivesAnEmptySummary` | An empty range closes the stream at once |
| `RecordRoute_SourceOfADeque_SendsEveryElement` | A range other than a vector is copied, in order |
| `RecordRoute_SourceOfATransformView_SendsEveryElement` | A view is copied too |
| `RecordRoute_SourceTryCancel_FinishGivesCancelled` | A cancel ends a held upload |
| `RecordRoute_SourceFrameDestroyedWhileFinishWaits_CancelsAndWaitsForOnDone` | Frees nothing, and posts nothing |

A write started before the client has the server's status can still succeed after the server has ended the RPC,
since gRPC buffers it, so the server error tests write until a `Write()` gives false, pausing 5 milliseconds between
writes, and check only that a false comes, that every later call gives false at once, and the status. Which write
fails is set by `WriteAfterTheServerErrorArrived`: it writes once, pauses the loop thread 1 second while the
server fails, then writes again. That write, started once the status has arrived, fails, held in 10 runs of 10. The
same pause lets `FinishBeforeCloseAfterTheServerError` show that a cancel sent once the status has arrived changes
nothing, as for the reader.

`WriteCompletedBeforeTheAwait` takes the awaitable `Write()` returns, pauses the loop thread 1 second before
awaiting it, so the write has completed, and checks the executor was given no coroutine meanwhile.
`FrameDestroyedMidWrite` keeps a write in flight with a test reactor whose `OnWriteDone()` sleeps 1 second,
and destroys the frame suspended in that `Write()`. Both destruction tests also check the reactor was released well
under the 5 seconds of `kOnDoneTimeout`, and that nothing was posted after the destruction.

The whole file held in 30 local runs, and in 5 under GCC's AddressSanitizer and UndefinedBehaviorSanitizer.

Nine mutants of `coro::Writer` and `coro::Source`, each run once against the whole file, locally:

| Mutant | Caught by |
| ------ | --------- |
| No `Rearm()` after a completed write | Both `WritesEveryPoint`, and every test that writes after a pause |
| `Rearm()` before `SendRequest()`, as the reader re-arms before `ResumeRead()` | The first write: `Rearm()`'s assert |
| The write callback ignoring `ok` | `WriteAfterTheServerErrorArrived` |
| An early `Finish()` not cancelling | All three early `Finish()` tests, waiting for the close |
| `Close()` not closing the request stream | Every test that closes, waiting for the close |
| The `Writer` destructor not abandoning the write hand-over | `FrameDestroyedMidWrite`: the destroyed frame resumed |
| `Source` dropping the last element | Every `Source` test with elements |
| `Source::Finish()` cancelling, as the `Writer`'s early `Finish()` does | Every `Source` test that ends on its own |
| The `Source` destructor not abandoning the done hand-over | `SourceFrameDestroyedWhileFinishWaits`: resumed |

The write callback ignoring `ok` was first missed: the next `Write()` is refused anyway, since the failed write ended
the write flow, and the server error tests could not tell which write failed. `WriteAfterTheServerErrorArrived` was
added for it. The second mutant is the order the reader uses, and the reason the writer differs: a fresh hand-over is
not ended, so nothing can be re-armed before the first write, and the writer re-arms after each one instead.

### Bidirectional join coverage

[coro_task_test.cpp][coro-task-test] tests `coro::WhenAll()` with no RPC: its tasks suspend on a hand-out handle the
test resumes, or on a `ResumeHandover` ended from the test's thread and resumed on the `AppLoop`.

| Test | Checks |
| ------ | --------- |
| `BothFinishWithoutSuspending_GivesBothValuesAtOnce` | Both values, a move-only one too, and no suspension |
| `FirstFinishesFirst_TheSecondsEndResumesTheAwaiter` | Nothing before the second ends, then both values |
| `SecondFinishesFirst_TheFirstsEndResumesTheAwaiter` | The same, the values still in the order of the tasks |
| `OneFinishesWithoutSuspending_TheJoinWaitsForTheOther` | The join waits for the task that suspended |
| `VoidTasks_JoinOnceBothHaveRun` | Two `Task<>` join, each giving a `std::monostate` |
| `FrameDestroyedWhileBothWait_DestroysBothTasks` | Every argument and local of both suspended tasks released |
| `FrameDestroyedWhileOneWaits_DestroysTheFinishedTaskToo` | The finished task's frame released too |
| `CurrentTask_BothTasksRunInTheAwaitersTask` | Both tasks, and the awaiter after, under the root's number |
| `CurrentTask_LastBranchResumedOutsideTheTask_AwaiterGoesOnInItsTask` | The join, not the branch, restores it |

[reactor_coro_bidi_test.cpp][coro-bidi-test] runs `RouteChat` through `coro::Bidi`, under `Delivery::kPush`,
`Submission::kApplicationPushes` and `ReadPacing::kContinuous`, the one configuration it takes. Most tests run a
reading coroutine and a writing one on the same `Bidi`, joined by `WhenAll()` before `Finish()`. Its service records
every note it reads and echoes it back, and finishes once the client closes its stream. It can instead echo nothing,
answer the close with a burst of replies before finishing, or finish with a given status once it has read a given
number of notes.

| Test | Checks |
| ------ | --------- |
| `RouteChat_EveryNoteSent_EveryReplyReceived` | 20 notes read in order, 20 echoes, every resumption on the loop |
| `RouteChat_NoNote_CloseEndsBothSides` | A close with no write ends the read side too |
| `RouteChat_ReadSideEndsWhileWritesGoOn_WriteGivesFalse` | The reader ends on the server's end, unclosed writes fail |
| `RouteChat_ServerError_ReadEnds_FinishGivesTheError` | The same with an error, which `Finish()` gives |
| `RouteChat_WritesEndWhileReadsGoOn_EveryLaterReplyReceived` | 50 replies after the close, all read |
| `RouteChat_ReplyQueuedBeforeTheAwait_GoesOnWithoutSuspending` | No post for a queued reply, then the end still waits |
| `RouteChat_TryCancel_EndsBothSides_FinishGivesCancelled` | A cancel from the writer ends the reader too |
| `RouteChat_FinishBeforeClose_EndsTheRpcCancelled` | An early `Finish()` cancels, on the loop |
| `RouteChat_FinishBeforeReadingTheQueuedReplies_GivesTheServersStatus` | The queue dropped, the status arrived kept |
| `RouteChat_FinishBeforeCloseAfterTheServerError_GivesTheError` | The server's status, once it has arrived |
| `RouteChat_FinishWhileTheReaderWaits_TheReaderGetsTheEnd` | The waiting reader ends, a later `Read()` too |
| `RouteChat_FrameDestroyedWithBothSidesInFlight_CancelsAndWaitsForOnDone` | A read waiting, a write in flight |

`ReadSideEndsWhileWritesGoOn` writes 5 milliseconds apart, and checks that the reader gets every echo then the end,
while the writer was never closed, and that a later write gives false. Which coroutine the loop runs first once the
server has ended is not fixed, so the test does not check whether the reader ended before the refused write: a first
version did, and failed on its first run. `FinishBeforeReadingTheQueuedReplies` closes, pauses the loop thread 1
second so the server's burst and status arrive, then awaits `Finish()` without reading: the status is the server's
OK, not a `CANCELLED` nobody sent. `FrameDestroyedWithBothSidesInFlight` keeps a write in flight with a test reactor
whose `OnWriteDone()` sleeps 1 second, against a service that echoes nothing, so the reader waits too, and checks the
reactor was released well under the 5 seconds of `kOnDoneTimeout`, and that nothing was posted after the
destruction.

`FinishWhileTheReaderWaits` came from the pre-PR review: `Finish()` awaited by the writing coroutine while the reading
one waits. Its first run aborted on `Read()`'s assert, since the reader, ended by the cancel, read once more after
`Finish()` had destroyed the reactor; `Read()`, `Write()` and `Close()` now assert only what they need, and give
nothing or false at once.

The same review pointed at a window no test provokes: when a frame holding a `Bidi` is destroyed while both flows
wait, the flows' frames go first, and a read or write callback that comes before the `Bidi`'s destructor abandons its
hand-overs could hand the executor a destroyed flow. The window is the few instructions between the two, on the
application thread, which a test cannot stretch without changing the code under test, so it is not provoked. Every
awaitable has the same rule, which their scenarios state: a frame is destroyed while suspended only once the loop no
longer runs. The `Bidi`'s comment and [reactor_coro.md](/applications/reactor/reactor_coro.md) say it too.

The whole file held in 10 local runs, and in 5 under GCC's AddressSanitizer and UndefinedBehaviorSanitizer.

Mutants, each run once against its test file, locally:

| Mutant | Caught by |
| ------ | --------- |
| A branch that ends first not clearing the task number | `CurrentTask_BothTasksRunInTheAwaitersTask` |
| The second branch started under the first one's task number | `CurrentTask_BothTasksRunInTheAwaitersTask` |
| The awaiter suspending even when both tasks have ended | `BothFinishWithoutSuspending`: the root never ends |
| The `Join` not destroying its branch frames | Both frame destruction tests |
| The awaiter's task number not restored by the join | `LastBranchResumedOutsideTheTask` |
| A queued message not waking a waiting `Read()` | `FinishBeforeClose`: the reader waits for the end |
| The end of the stream not waking a waiting `Read()` | Seven tests: the reader never ends |
| The read hand-over not re-armed after a wait | Four tests: `Read()` ends the stream at the next wait |
| An early `Finish()` not cancelling | `FinishBeforeClose` and `FinishWhileTheReaderWaits` |
| The `Bidi` destructor not abandoning the read hand-over | `FrameDestroyedWithBothSidesInFlight`: a crash |
| An early `Finish()` not dropping the queue | None: the `Bidi` frees it moments later anyway |
| `convoy_chat_coro` not showing its first reply | The smoke matrix: 2 replies received, 3 owed |

The awaiter's task number was first restored only by the hand-over that resumed the last branch, and its mutant
survived every test: `LastBranchResumedOutsideTheTask` was added for it. The queue the
early `Finish()` drops is freed with the `Bidi` once the frame ends, right after, so dropping it earlier shows in no
test; it stays for the rule that an early `Finish()` releases what is queued.

### Coroutine teardown coverage

[reactor_coro_halt_test.cpp][coro-halt-test] runs the coroutine level's shutdown against the real EventLoop, in a
process of its own, since EventLoop cannot restart after `Halt()`. Its service holds every RPC open, answering nothing,
until the client cancels it.

| Test | Checks |
| ------ | --------- |
| `EveryAwaitableInFlight_HaltThenDestroy_ReleasesEveryReactorAfterOnDone` | All cancelled, each freed after `OnDone()` |

One coroutine of each awaitable kind is suspended mid-RPC:

- a `Unary` in `Finish()`;
- a `Reader` in `Read()`;
- a `Writer` between writes, its write flow open, in `EventLoopExecutor::After()`;
- a `Source` in `Finish()`;
- a `Bidi`, joined, with one flow in `Read()` and the other between writes.

The test then halts the loop, which discards the timers and any resumption still to come. It destroys the root tasks
on its own thread, as `main()` does once `EventLoop::Run()` has returned. It checks that the destruction took well
under the 5 seconds after which a reactor would be leaked, that the server saw all five RPCs cancelled, and that no
coroutine resumed. It held in 5 runs under GCC's AddressSanitizer and UndefinedBehaviorSanitizer.

| Mutant | Caught by |
| ------ | --------- |
| `DestroyAfterOnDone()` freeing the reactor after `TryCancel()`, without waiting for `OnDone()` | ASan: use after free |

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
selection; `applications/reactor/tests/.clang-tidy` adjusts it for the gtest bodies there.

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
[coro-reader-test]: /applications/reactor/tests/reactor_coro_reader_test.cpp
[coro-writer-test]: /applications/reactor/tests/reactor_coro_writer_test.cpp
[coro-bidi-test]: /applications/reactor/tests/reactor_coro_bidi_test.cpp
[coro-halt-test]: /applications/reactor/tests/reactor_coro_halt_test.cpp
[reactor-coro]: /applications/reactor/reactor_coro.h
[integration-test]: /applications/reactor/tests/client_reactor_integration_test.cpp
[callback-smoke-test]: /applications/callback/tests/callback_smoke_test.sh
[smoke-matrix-cmake]: /applications/tests/CMakeLists.txt
[smoke-matrix-cell]: /applications/tests/smoke_matrix_cell.sh
[smoke-matrix-compare]: /applications/tests/smoke_matrix_compare.sh
[test-fixture]: /applications/reactor/tests/route_guide_test_fixture.h
[pacing-support]: /applications/reactor/tests/read_pacing_test_support.h
