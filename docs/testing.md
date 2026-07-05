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
configured per scenario, and completes a promise directly from the reactor's callbacks. This
approach has no EventLoop lifecycle to manage, so it is faster to write and exercises reactor
callback logic, error paths, cancellation, and deadline handling without the added complexity of
a background dispatch thread.

### EventLoop integration test

[client_reactor_integration_test.cpp][integration-test] validates the full production dispatch
path: a gRPC reactor callback triggers `EventLoop::TriggerEvent()`, and the application thread
executes the deferred handler. It runs a real EventLoop in `NON_BLOCK` mode on a background
thread and asserts that reactor callbacks execute off the main thread while `EventLoop` handlers
execute the deferred processing, including the hold/resume pattern for streaming responses
(`GetResponse()` called from an `EventLoop` handler).

Unlike the four unit test files, this single fixture accumulates one case per reactor type
instead of splitting into one file per type, since every case exercises the same dispatch path
and differs only in RPC shape.

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

Test names have three underscore-separated segments. The first segment differs by suite, since
the two suites verify different things:

- Unit suites (`Active*ReactorTest`): `RpcMethod_Scenario_Result`, e.g.
  `GetFeature_ValidPoint_ReturnsFeature`. `Result` is the actual outcome being asserted, so it
  varies freely per test.
- Integration suite (`ClientReactorIntegrationTest`): `Shape_Scenario_DispatchesToEventLoop`,
  e.g. `Unary_ValidPoint_DispatchesToEventLoop`. `Shape` is one of `Unary`/`Read`/`Write`/`Bidi`
  (the RPC method itself is redundant here, since this fixture mixes multiple RPC methods in one
  file). `Result` is always `DispatchesToEventLoop`: this suite only ever verifies that dispatch
  happened, never the RPC's business outcome, which the unit suites already cover.

## Test infrastructure

### Shared fixture

[route_guide_test_fixture.h][test-fixture] provides `RouteGuideTestFixtureBase<ServiceT>`, a
template test fixture that starts an in-process gRPC server on a dynamic port, and creates a
channel and stub connected to it. `ServiceT` is the fake `routeguide::RouteGuide::CallbackService`
implementation each test suite supplies, since each exercises a different RPC method.

### In-process server

Each test file defines its own `TestRouteGuideService`, a fake implementation of the RPC method
under test with configurable responses and error injection, so scenarios are deterministic and do
not depend on the real RouteGuide feature database. The server binds to `localhost:0` (dynamic
port) to avoid conflicts between parallel test runs.

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

Always set a timeout on futures (`wait_for(std::chrono::seconds(5))`); a reactor bug that never
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
[unary-test]: /applications/reactor/tests/active_unary_reactor_test.cpp
[read-test]: /applications/reactor/tests/active_read_reactor_test.cpp
[write-test]: /applications/reactor/tests/active_write_reactor_test.cpp
[bidi-test]: /applications/reactor/tests/active_bidi_reactor_test.cpp
[integration-test]: /applications/reactor/tests/client_reactor_integration_test.cpp
[test-fixture]: /applications/reactor/tests/route_guide_test_fixture.h
