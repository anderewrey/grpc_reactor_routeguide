# gRPC Reactor Callbacks, client-side implementation

Complete redesign of the [gRPC route_guide][grpc-route-guide] example.

To learn more about:

- [Asynchronous callback API][grpc-callback-api] and the [best practices][grpc-best-practices] about gRPC services.
- [Proto3 syntax][proto3-syntax] and the [best practices][protobuf-best-practices] about RPC messages.

This project shows the design and usages of the client-side implementation for applications using gRPC.
This project is based on gRPC library and provides an implementation to:

- send request and receive response messages ([unary reactor][client-unary-reactor])
- to connect to a stream and receives streamed response messages ([read reactor][client-read-reactor])

## Requirements

### C++ standard

This code requires C++20, so be sure the available C++ compiler has a good support of the C++20 features and libraries.
For details about the compilers: [C++20 compiler support][cpp20-support]

### Build tools

- C++ compiler (GCC 11 or above, Clang, ...)
- CMake

### Dependencies automatically solved by CMake

The following third-parties are fetched by CMake and compiled for the need of the project:

- [EventLoop][eventloop-lib] library which follows the state-of-the-art of eventloop mechanisms and its replacement with
  similar library or implementation should be easy.
- [gflags][gflags-lib] library for command-line arguments processing.
- [glaze][glaze-lib] library to handle the JSON database in a better way than the original code from the gRPC example.
- [spdlog][spdlog-lib] library for logging using the {fmt} style.

### External dependencies

- gRPC
- Protobuf, which is usually included through gRPC

Because downloading and compiling gRPC on project build is a long and annoying process, it is better instead to have
them already available through the OS system:

- **System installation**: Install via package manager or manual compilation (see
  [GRPC_BUILD_GUIDE.md](/GRPC_BUILD_GUIDE.md))

## Implementation details about the client-side reactors

Refer to [reactor_client.md](/applications/reactor/reactor_client.md)

## Thanks to

(in alphabetical order)

- Alexander Shvets for his book [Dive Into Design Patterns][design-patterns-book] and [website][refactoring-guru].
- Falko Axmann for his structured example about [CMake project with gRPC and protobuf][falko-grpc-cmake].
- [Menno van der Graaf][mercotui] for being my rubberduck while learning about and developing the initial client-side
  implementation of the gRPC Callback API and struggling with the nonsense examples from gRPC.
- Rainer Grimm for all his [books][rainer-books] and his [blog][modernescpp-blog] about modern C++.

<!-- Reference links -->
[grpc-route-guide]: https://github.com/grpc/grpc/tree/master/examples/cpp/route_guide
[grpc-callback-api]: https://grpc.io/docs/languages/cpp/callback/
[grpc-best-practices]: https://grpc.io/docs/languages/cpp/best_practices/
[proto3-syntax]: https://protobuf.dev/programming-guides/proto3/
[protobuf-best-practices]: https://protobuf.dev/best-practices/
[client-unary-reactor]: https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_unary_reactor.html
[client-read-reactor]: https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_read_reactor.html
[cpp20-support]: https://en.cppreference.com/w/cpp/compiler_support/20
[eventloop-lib]: https://github.com/amoldhamale1105/EventLoop
[gflags-lib]: https://github.com/gflags/gflags
[glaze-lib]: https://github.com/stephenberry/glaze
[spdlog-lib]: https://github.com/gabime/spdlog
[design-patterns-book]: https://refactoring.guru/design-patterns/book
[refactoring-guru]: https://refactoring.guru/design-patterns
[falko-grpc-cmake]: https://www.f-ax.de/dev/2020/11/08/grpc-plugin-cmake-support.html
[mercotui]: https://github.com/Mercotui
[rainer-books]: https://leanpub.com/u/RainerGrimm
[modernescpp-blog]: https://www.modernescpp.com/index.php/blog/
