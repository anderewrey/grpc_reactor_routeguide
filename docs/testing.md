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

Eight test binaries validate reactor callback logic directly, one per reactor type and `Delivery` approach, using
`std::promise`/`std::future` for synchronization instead of an EventLoop:

- [active_unary_reactor_pull_test.cpp][unary-pull-test], [active_unary_reactor_push_test.cpp][unary-push-test]:
  `ActiveUnaryReactor` via `GetFeature`
- [active_read_reactor_pull_test.cpp][read-pull-test], [active_read_reactor_push_test.cpp][read-push-test]:
  `ActiveReadReactor` via `ListFeatures`
- [active_write_reactor_pull_test.cpp][write-pull-test], [active_write_reactor_push_test.cpp][write-push-test]:
  `ActiveWriteReactor` via `RecordRoute`
- [active_bidi_reactor_pull_test.cpp][bidi-pull-test], [active_bidi_reactor_push_test.cpp][bidi-push-test]:
  `ActiveBidiReactor` via `RouteChat`

Each file pins its approach in a single `kDelivery` constant and instantiates every reactor with it, so a pull file
and its push counterpart read side by side as the same scenarios written against each approach.
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

### When to use each approach

| Scenario | Approach |
| ---------- | ---------- |
| Reactor callback logic, error paths, cancellation, deadlines | Synchronous unit test |
| Thread-hopping and dispatch through `EventLoop` | Integration test |
| Message ownership crossing the thread boundary through `EventLoop` | Integration test |
| New reactor type | Both: one synchronous unit test file per `Delivery` plus one integration test per `Delivery` |

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
| `RouteChat_ConcurrentClaimants_KeepTheHoldAccountingIntact` | Two writer threads cannot both win one claim |
| `RouteChat_WriteAfterReadStreamClosed_RejectedDeterministically` | Writes are refused once the read stream ends |
| `RouteChat_ReleaseWithNoHold_DoesNotLetOnDoneOverlapTheReaction` | An unclaimed release lets `OnDone` overlap |

The four live in [active_bidi_reactor_push_test.cpp][bidi-push-test], under `kContinuous`. The write side is identical
under both `Delivery` approaches, and the one place the two meet it, the `OnReadDone(false)` release, is exercised under
each: `RouteChat_DeadlineExceeded_PropagatesStatus` exists in both bidi files and doubles as the guard for that release,
since removing it hangs the test for its full timeout. Two properties are deliberately not covered by tests, and are
recorded in [deferred-work.md](/_bmad-output/implementation-artifacts/deferred-work.md): the atomicity of the claim,
whose race window is too narrow to hit from a test and which needs ThreadSanitizer, and the non-re-arm after a terminal
write, which `OnReadDone(false)` masks.

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
[integration-test]: /applications/reactor/tests/client_reactor_integration_test.cpp
[callback-smoke-test]: /applications/callback/tests/callback_smoke_test.sh
[test-fixture]: /applications/reactor/tests/route_guide_test_fixture.h
[pacing-support]: /applications/reactor/tests/read_pacing_test_support.h
