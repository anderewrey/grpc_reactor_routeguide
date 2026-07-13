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

Four test binaries validate reactor callback logic directly, one per reactor type, using
`std::promise`/`std::future` for synchronization instead of an EventLoop:

- [active_unary_reactor_test.cpp][unary-test]: `ActiveUnaryReactor` via `GetFeature`
- [active_read_reactor_test.cpp][read-test]: `ActiveReadReactor` via `ListFeatures`
- [active_write_reactor_test.cpp][write-test]: `ActiveWriteReactor` via `RecordRoute`
- [active_bidi_reactor_test.cpp][bidi-test]: `ActiveBidiReactor` via `RouteChat`

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
execute the deferred processing. This includes the hold/resume pattern for streaming responses,
where `GetResponse()` is called from an `EventLoop` handler.

Unlike the four unit test files, this single fixture accumulates one case per reactor type
instead of splitting into one file per type. Every case exercises the same dispatch path and
differs only in RPC shape.

### When to use each approach

| Scenario | Approach |
| ---------- | ---------- |
| Reactor callback logic, error paths, cancellation, deadlines | Synchronous unit test |
| Thread-hopping and dispatch through `EventLoop` | Integration test |
| Hold/resume pattern for streaming responses | Integration test |
| New reactor type | Both: one synchronous unit test file plus one integration test |

## Test coverage

### Coverage by RPC type

See the file list above for which file covers which RPC type.

| RPC Type | Reactor Class | Scenarios |
| ---------- | --------------- | ----------- |
| Unary (`GetFeature`) | `ActiveUnaryReactor` | Success, empty/server/not-found response, cancel, deadline, concurrent |
| Server stream (`ListFeatures`) | `ActiveReadReactor` | Multiple/empty response, mid-stream error, cancel, concurrent |
| Client stream (`RecordRoute`) | `ActiveWriteReactor` | Multiple/empty point, overlapping writes, cancel, error |
| Bidirectional (`RouteChat`) | `ActiveBidiReactor` | Send/receive, interleaved, either side closes first, cancel |
| EventLoop dispatch | N/A | `GetFeature`/`ListFeatures`/cancel dispatched through a real `EventLoop` |

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
`GetFeature_ValidPoint_DispatchesToEventLoop`.

## Test infrastructure

### Shared fixture

[route_guide_test_fixture.h][test-fixture] provides `RouteGuideTestFixtureBase<ServiceT>`, a
template test fixture. It starts an in-process gRPC server on a dynamic port, and creates a
channel and stub connected to it. `ServiceT` is the fake `routeguide::RouteGuide::CallbackService`
implementation each test suite supplies, since each exercises a different RPC method.

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

| Matrix variant | Compiler | Sanitizers | Publishes JUnit report |
| ----- | ---------- | ------------ | ------------------------- |
| `gcc-debug` | GCC | None | Yes |
| `clang-debug` | Clang | None | No |
| `clang-asan-ubsan` | Clang | AddressSanitizer, UndefinedBehaviorSanitizer | No |

`gcc-debug` is the only variant that publishes a per-suite JUnit check-run report, since it is the
baseline configuration. The other variants are pass/fail gates: a failure needs ctest's raw output
and stack trace, not a results table.

### Why Clang for the sanitizer job, not GCC

Alpine's GCC package builds with `--enable-libsanitizer`, but its libsanitizer interceptors (for
example `sendmsg`) are written and tested against glibc's struct layouts. They crash against musl
before this project's own code runs. Clang's `compiler-rt` is the sanitizer runtime Alpine's own
ecosystem maintains musl support for, so the sanitizer job uses Clang exclusively.

### Why no ThreadSanitizer job

ThreadSanitizer was tried and dropped. Every suppression strategy attempted (`race:` entries,
`called_from_lib:`, `ignore_noninstrumented_modules=1`) either failed to converge on a stable
suppression list, since each run surfaced new gRPC/Abseil/c-ares-internal signatures with no
repeats, or blinded TSan's synchronization tracking enough to produce a worse class of false
positive: races reported between this project's own fixture and worker threads that do not
actually race. Running gRPC, Abseil, and c-ares themselves built with TSan instrumentation would
be required to get a clean signal, which is out of scope for this project's CI.

### ASan runtime options

The sanitizer job sets `ASAN_OPTIONS` on the `Test` step (see [ci.yml][ci-workflow] for the
per-option rationale), enabling checks that are off by default upstream: invalid pointer pair
comparisons, a larger use-after-free quarantine, strict C-string null-termination checks,
stack-use-after-return detection, leak detection, and static initialization order checking.

Stack traces from this job are symbolized via `llvm22-symbolizer`, installed alongside Clang and
symlinked to the unversioned `llvm-symbolizer` name the sanitizer runtime expects. Neither the
`clang` nor the `compiler-rt` Alpine package ships a symbolizer binary.

### Static analysis

[static-analysis.yml][static-analysis-workflow] runs `lint` (pre-commit) and `clang-tidy` as their
own workflow, separate from the build/test matrix above, on every push. clang-tidy has two entry
points sharing one build: a `pull_request` or `push` run that annotates diagnostics without
failing the job, and a `workflow_dispatch` run that fails on any diagnostic for a deliberate,
whole-repo pass. See `static-analysis.yml` for why it is a separate workflow, and `.clang-tidy`
for the check selection.

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
  routeguide::GetFeature::Callbacks cbs;
  cbs.done = [&done_promise](...) { done_promise.set_value(status); };

  // Create reactor
  auto reactor = std::make_unique<routeguide::GetFeature::ClientReactor>(
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
cmake --build cmake-build-vcpkg-debug-gcc --target active_unary_reactor_test
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
[reactor-client]: /applications/reactor/reactor_client.h
[reactor-client-routeguide]: /applications/reactor/reactor_client_routeguide.h
[unary-test]: /applications/reactor/tests/active_unary_reactor_test.cpp
[read-test]: /applications/reactor/tests/active_read_reactor_test.cpp
[write-test]: /applications/reactor/tests/active_write_reactor_test.cpp
[bidi-test]: /applications/reactor/tests/active_bidi_reactor_test.cpp
[integration-test]: /applications/reactor/tests/client_reactor_integration_test.cpp
[test-fixture]: /applications/reactor/tests/route_guide_test_fixture.h
