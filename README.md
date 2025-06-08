# gRPC Reactor Callbacks, client-side implementation

Complete redesign of the [gRPC route_guide](https://github.com/grpc/grpc/tree/master/examples/cpp/route_guide) example.

To learn more about:

- [Asynchronous callback API](https://grpc.io/docs/languages/cpp/callback/) and the
  [best practices](https://grpc.io/docs/languages/cpp/best_practices/) about gRPC services.
- [Proto3 syntax](https://protobuf.dev/programming-guides/proto3/) and the
  [best practices](https://protobuf.dev/programming-guides/best-practices/) about RPC messages.

This project shows the design and usages of the client-side implementation for applications using gRPC.
This project is based on gRPC library and provides an implementation to:

- send request and receive response messages ([unary reactor](https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_unary_reactor.html))
- to connect to a stream and receives streamed response messages ([read reactor](https://grpc.github.io/grpc/cpp/classgrpc_1_1_client_read_reactor.html))

## Requirements

### C++ standard

This code requires C++20, so be sure the available C++ compiler has a good support of the C++20 features and libraries.
For details about the compilers: [C++20 compiler support](https://en.cppreference.com/w/cpp/compiler_support/20)

### Build tools

- C++ compiler (GCC 11 or above, Clang, ...)
- CMake

### Dependencies automatically solved by CMake

The following third-parties are fetched by CMake and compiled for the need of the project:

- [EventLoop](https://github.com/amoldhamale1105/EventLoop) library which follows the state-of-the-art of eventloop
  mechanisms and its replacement with similar library or implementation should be easy.
- [gflags](https://github.com/gflags/gflags) library for command-line arguments processing.
- [glaze](https://github.com/stephenberry/glaze) library to handle the JSON database in a better way than the
  original code from the gRPC example.
- [spdlog](https://github.com/gabime/spdlog) library for logging using the {fmt} style.

### External dependencies

- gRPC
- Protobuf, which is usually included through gRPC

Because downloading and compiling gRPC on project build is a long and annoying process, it is better instead to have
them already available through the OS system:

- with Ubuntu 24.04: installing it through the package manager is good enough.
- Homemade compilation and installation: it works 100% of time when cmake is used to build it, otherwise
  there's missing hints for cmake to include it into this project.
- TODO: another way would be through [conan package](https://conan.io/center/recipes/grpc), but not achieved yet: got
        many compilation errors while executing/building the package from conan.

## Implementation details about the client-side reactors

Refer to [reactor_client.md](client/reactor_client.md)

## Thanks to

(in alphabetical order)

- Alexander Shvets for his book [Dive Into Design Patterns](https://refactoring.guru/design-patterns/book) and
  [website](https://refactoring.guru/design-patterns).
- Falko Axmann for his structured example about
  [CMake project with gRPC and protobuf](https://www.f-ax.de/dev/2020/11/08/grpc-plugin-cmake-support.html).
- [Menno van der Graaf](https://github.com/Mercotui) for being my rubberduck while learning about and developing the
  initial client-side implementation of the gRPC Callback API and struggling with the nonsense examples from gRPC.
- Rainer Grimm for all his [books](https://leanpub.com/u/RainerGrimm) and his [blog](https://www.modernescpp.com/index.php/blog/)
  about modern C++.
