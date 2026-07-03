# Architecture Documentation

## Executive Summary

This project implements a gRPC client application using the **Active Object** design pattern
combined with the **Reactor pattern** for event-driven asynchronous RPC handling. The architecture
enables non-blocking RPC calls while maintaining thread safety and clean separation of concerns.

## Architecture Pattern

### Active Object Pattern

This implementation follows the Active Object pattern, combining it with the Reactor pattern
for event-driven processing. The reactor library provides the pattern infrastructure; applications
provide business logic in the Servant handlers.

**Library vs Application Responsibilities:**

| Aspect | Definition | This Implementation |
| -------- | ------------------ | --------------------- |
| Guards | Method Requests have `guard()` for synchronization | `AddHold()`/`RemoveHold()` mechanism |
| Servant | Business logic component with state | Application-provided (demo uses logging placeholders) |
| Scope | Full client-server encapsulation | Client-side library (server has separate Servant) |

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

### Component Roles

| Component | Implementation | Role |
| ----------- | ---------------- | ------ |
| **Proxy** | `RouteGuideClient::GetFeature()`, `ListFeatures()` | Client-facing API, creates reactors |
| **Method Request** | `ActiveUnaryReactor`, `ActiveReadReactor` | Encapsulates RPC state with guards |
| **Scheduler** | `EventLoop::Run()` | Dispatches events to handlers |
| **Activation Queue** | EventLoop internal queue | Holds pending events |
| **Servant** | `EventLoop::RegisterEvent()` handlers | Processes RPC responses (application logic) |
| **Future** | `GetResponse()`, `Status()` | Deferred result access |

> **Note:** The "Proxy" terminology describes the component's role in the Active Object pattern, not
> an application of the GoF Proxy design pattern. The demo application uses simple logging as Servant
> logic; production applications implement actual business logic in the event handlers.

## Technology Stack

| Layer | Technology | Purpose |
| ------- | ------------ | --------- |
| Language | C++20 | Modern features (concepts, constexpr) |
| RPC Framework | gRPC | Remote procedure calls |
| Serialization | Protobuf | Binary message format |
| Event System | EventLoop | Cross-thread event dispatching |
| Logging | spdlog | Structured logging |
| JSON | glaze | Database loading |
| Build | CMake + Ninja | Build orchestration |
| Dependencies | vcpkg | Multi-compiler package management |

## Data Architecture

### Protobuf Messages

```protobuf
message Point {
  int32 latitude = 1;   // E7 format (degrees * 10^7)
  int32 longitude = 2;
}

message Rectangle {
  Point lo = 1;  // Lower-left corner
  Point hi = 2;  // Upper-right corner
}

message Feature {
  string name = 1;
  Point location = 2;
}

message RouteNote {
  Point location = 1;
  string message = 2;
}

message RouteSummary {
  int32 point_count = 1;
  int32 feature_count = 2;
  int32 distance = 3;      // meters
  int32 elapsed_time = 4;  // seconds
}
```

### Data Flow

1. **Request**: Application creates `Point` or `Rectangle` message
2. **Serialization**: Protobuf serializes to binary
3. **Transport**: gRPC sends over HTTP/2
4. **Server Processing**: Server processes request
5. **Response**: Server sends `Feature` or streaming responses
6. **Callback**: gRPC thread calls `OnDone`/`OnReadDone`
7. **Event**: Callback triggers EventLoop event
8. **Processing**: Response handler processes response on application thread

## API Design

### RPC Methods

| Method | Pattern | Description |
| -------- | --------- | ------------- |
| `GetFeature` | Unary | Get feature at a point |
| `ListFeatures` | Server streaming | Get features within rectangle |
| `RecordRoute` | Client streaming | Send points, receive summary |
| `RouteChat` | Bidirectional | Exchange route notes |

### Reactor Class Hierarchy

```text
grpc::ClientUnaryReactor
    └── RpcReactor::Client::ActiveUnaryReactor<ResponseT>
            └── routeguide::GetFeature::ClientReactor

grpc::ClientReadReactor<ResponseT>
    └── RpcReactor::Client::ActiveReadReactor<ResponseT>
            └── routeguide::ListFeatures::ClientReactor
```

## Thread Safety Model

### Thread Boundaries

| Thread | Operations |
| ------------------- | ------------------------------------------------------- |
| Application (main) | Create reactors, process responses, application logic |
| gRPC Thread Pool | Execute callbacks (OnDone, OnReadDone, OnWriteDone) |

### Synchronization Mechanisms

1. **atomic flags**: `response_ready_`, `read_no_more_` for cross-thread state
2. **AddHold/RemoveHold**: Prevents race between StartRead and OnDone
3. **EventLoop**: Safe cross-thread event dispatching

### Critical Pattern: Hold Mechanism

For streaming RPCs, the reactor uses AddHold/RemoveHold to prevent race conditions:

```cpp
void OnReadDone(bool ok) override {
  if (ok && cbs_.ok(this, response_)) {
    this->AddHold();  // Prevent OnDone while processing
    return;
  }
  // ...
}

bool GetResponse(ResponseT& response) {
  // ... swap response ...
  this->StartRead(&response_);
  this->RemoveHold();  // Resume RPC
  return true;
}
```

## Component Structure

### Libraries

| Library | Type | Purpose |
| --------- | ------ | --------- |
| `common` | Header-only | C++23 backports (std::to_underlying) |
| `protobuf_utils` | Static | Protobuf message utilities |
| `rg_proto` | Object | Generated protobuf/gRPC code |
| `rg_service` | Static | RouteGuide business logic |

### Dependency Graph

```text
route_guide_active_reactor_client
    ├── rg_service
    │   ├── rg_proto
    │   │   ├── protobuf::libprotobuf
    │   │   └── gRPC::grpc++
    │   ├── protobuf_utils
    │   ├── common
    │   ├── spdlog::spdlog
    │   ├── glaze::glaze
    │   └── gflags::gflags
    └── EventLoop::EventLoop
```

## Testing Strategy

Currently, the project includes demonstration executables that validate functionality:

- Run server: `./route_guide_sync_server`
- Run client: `./route_guide_active_reactor_client`

Expected behavior:

- Client connects to localhost:50051
- Executes ListFeatures (streaming) and GetFeature (unary) RPCs
- Logs responses with per-RPC loggers

## Deployment Architecture

### Build Configurations

| Preset | Compiler | Build Type | Use Case |
| -------- | ---------- | ------------ | ---------- |
| `vcpkg-clang19-debug` | Clang 19 | Debug | Development |
| `vcpkg-clang19-release` | Clang 19 | Release | Production |
| `vcpkg-gcc-debug` | GCC 11 | Debug | Development |
| `vcpkg-gcc-release` | GCC 11 | Release | Production |
| `vcpkg-gcc13-debug` | GCC 13 | Debug | Development |
| `vcpkg-gcc13-release` | GCC 13 | Release | Production |

### Binary Optimization

Release builds use:

- LTO (Link-Time Optimization)
- Symbol stripping
- Dead code elimination
- `-O3 -march=native -mtune=native`

Result: 3.1 MB debug → 500-600 KB release (80%+ reduction)

## Design Decisions

### Why Active Object Pattern?

The Active Object pattern provides the architecture for this implementation:

1. **Thread Safety**: Responses processed on application thread, not gRPC thread
2. **Non-blocking**: Client calls return immediately
3. **Separation of Concerns**: Clean boundary between RPC and application logic
4. **Testability**: EventLoop can be mocked for testing

The reactor library provides the Active Object infrastructure; applications provide their own Servant
logic. The `AddHold()`/`RemoveHold()` mechanism serves as the guard, preventing concurrent access
during response processing. This separation enables reusable async RPC handling across different
applications.

### Why vcpkg with Custom Triplets?

1. **Multi-compiler Support**: Same project builds with GCC 11, GCC 13, Clang 19
2. **ABI Compatibility**: Dependencies built with matching compiler
3. **Reproducibility**: Consistent builds across environments
4. **Debug + Release Mix**: Debug app can link Release libraries on Linux

### Why EventLoop Library?

1. **Simple API**: `TriggerEvent()` / `RegisterEvent()` / `Run()`
2. **Thread-safe**: Safe cross-thread event dispatching
3. **Replaceable**: Interface allows swapping with other event systems

## Implementation Patterns & AI Agent Consistency Rules

This section defines patterns that ensure consistent implementation across AI agents and contributors.

### Style Guide Reference

All code follows the [Google C++ Style Guide](https://google.github.io/styleguide/cppguide.html) with these
project-specific settings:

- Line limit: 120 characters (configured in `.clang-format`)
- C++ standard: C++20
- Pre-commit hooks enforce cpplint and clang-format

### Reactor Class Hierarchy

**Generic Base Classes** (`applications/reactor/reactor_client.h`):

| Class | RPC Pattern | Template Parameters |
| ------- | ------------- | --------------------- |
| `ActiveUnaryReactor<ResponseT>` | Unary | Response type |
| `ActiveReadReactor<ResponseT>` | Server-streaming | Response type |
| `ActiveWriteReactor<RequestT>` | Client-streaming | Request type |
| `ActiveBidiReactor<RequestT, ResponseT>` | Bidirectional | Request and response types |

**Service-Specific Adapters** (`applications/reactor/reactor_client_routeguide.h`):

| Generic Class | RouteGuide Adapter | RPC Method |
| --------------- | ------------------- | ------------ |
| `ActiveUnaryReactor<Feature>` | `routeguide::GetFeature::ClientReactor` | `GetFeature` |
| `ActiveReadReactor<Feature>` | `routeguide::ListFeatures::ClientReactor` | `ListFeatures` |
| `ActiveWriteReactor<Point>` | `routeguide::RecordRoute::ClientReactor` | `RecordRoute` |
| `ActiveBidiReactor<RouteNote, RouteNote>` | `routeguide::RouteChat::ClientReactor` | `RouteChat` |

### Callback Struct Patterns

Each reactor type has a corresponding callback struct:

**Unary** (`ActiveUnaryCallbacks<ResponseT>`):

- `done` - Called on RPC completion with status and response

**Server-streaming** (`ActiveReadCallbacks<ResponseT>`):

- `ok` - Called on successful read, returns bool to hold/continue
- `nok` - Called when stream ends (no more reads)
- `done` - Called on RPC completion

**Client-streaming** (`ActiveWriteCallbacks<RequestT>`):

- `write_done` - Called after each write completes
- `done` - Called on RPC completion with response

**Bidirectional** (`ActiveBidiCallbacks<RequestT, ResponseT>`):

- `read_ok` - Called on successful read
- `read_nok` - Called when read stream ends
- `write_done` - Called after each write completes
- `done` - Called on RPC completion

### File Organization

**New reactor implementations:**

| File Type | Location | Naming |
| ----------- | ---------- | -------- |
| Generic base class | `applications/reactor/reactor_client.h` | Add to existing file |
| Service adapter | `applications/reactor/reactor_client_routeguide.h` | Add to existing file |
| Unit tests | `applications/reactor/tests/` | `{reactor_name}_test.cpp` |

**Test file naming:**

- `active_write_reactor_test.cpp` - Client-streaming reactor tests
- `active_bidi_reactor_test.cpp` - Bidirectional reactor tests

### Error Handling Pattern

Use `grpc::Status` passthrough - no custom exceptions:

```cpp
// Callback receives status
cbs.done = [](auto* reactor, const grpc::Status& status, const ResponseT& response) {
  if (status.ok()) {
    // Process response
  } else {
    // Handle error via status.error_code(), status.error_message()
    logger.error("RPC failed: {} - {}",
                 status.error_code(), status.error_message());
  }
};

// Application checks status after OnDone
if (reactor->Status().ok()) {
  ResponseT response;
  reactor->GetResponse(response);
}
```

**Rationale:** Status-based error handling aligns with Google C++ Style Guide (which discourages exceptions),
matches gRPC's native pattern, and works cleanly with the async callback model where exceptions cannot cross
thread boundaries.

### Thread Safety Patterns

**Atomic flags for cross-thread state:**

```cpp
std::atomic_bool response_ready_{false};  // Set by gRPC thread, read by app thread
std::atomic_bool read_no_more_{false};    // Set by gRPC thread, read by app thread
```

**Hold mechanism for streaming:**

```cpp
// In OnReadDone - hold before returning to application
this->AddHold();

// In GetResponse - resume after application processes
this->StartRead(&response_);
this->RemoveHold();
```

### Requirements to Component Mapping

| Requirement | Component | File |
| ------------- | ----------- | ------ |
| FR1 (Unary) | `ActiveUnaryReactor` | `reactor_client.h` |
| FR2 (Server-streaming) | `ActiveReadReactor` | `reactor_client.h` |
| FR3 (Client-streaming) | `ActiveWriteReactor` | `reactor_client.h` |
| FR4 (Bidirectional) | `ActiveBidiReactor` | `reactor_client.h` |
| FR5 (EventLoop integration) | Callback → `TriggerEvent()` | Application code |
| FR6 (Cancellation) | `TryCancel()` method | All reactor classes |
| FR7 (Status check) | `Status()` method | All reactor classes |
| FR12 (Service adapters) | `routeguide::*::ClientReactor` | `reactor_client_routeguide.h` |
| FR19-21 (Testing) | googletest suite | `applications/reactor/tests/` |

### AI Agent Implementation Checklist

When implementing a new reactor pattern, AI agents MUST:

1. **Follow the generic → specialized pattern**
   - Add generic class to `reactor_client.h`
   - Add RouteGuide adapter to `reactor_client_routeguide.h`

2. **Include all standard methods**
   - Constructor taking `context` and `callbacks`
   - Deleted copy/move constructors and assignment operators
   - `TryCancel()` for cancellation
   - `GetResponse()` for response extraction (with swap)
   - `Status()` for completion status

3. **Implement thread safety**
   - Atomic flags for cross-thread state
   - Hold mechanism for streaming patterns
   - No shared mutable state without synchronization

4. **Create per-reactor test file**
   - File: `applications/reactor/tests/{pattern}_test.cpp`
   - Cover: success, error, cancel, timeout scenarios
   - Use in-process server (no external dependencies)

5. **Update documentation**
   - Add PlantUML sequence diagram to `reactor_client.md`
   - Document the *why* behind design decisions
