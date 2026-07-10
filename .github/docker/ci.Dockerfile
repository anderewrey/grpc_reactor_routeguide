# CI build environment for grpc_reactor_routeguide: Alpine plus both toolchains (GCC, Clang) the
# CI matrix in .github/workflows/ci.yml needs, and EventLoop (not packaged upstream) built once
# per toolchain.
#
# Built and pushed by the prepare-image job in ci.yml, tagged by a hash of this file plus the
# EventLoop patch below, so an unchanged environment is reused across CI runs instead of rebuilt.
FROM alpine:3.24

# llvm22 provides llvm22-symbolizer, needed to turn ASan/UBSan stack traces into function names
# and line numbers; neither the clang nor the compiler-rt package ships a symbolizer. The
# sanitizer runtime only recognizes a fixed set of binary names (llvm-symbolizer, addr2line,
# atos, ...), so llvm22-symbolizer is symlinked to the unversioned name.
RUN apk add --no-cache \
      cmake ninja git nodejs \
      gcc g++ clang compiler-rt binutils llvm22 \
      protobuf-dev \
      grpc-dev \
      abseil-cpp-dev \
      c-ares-dev \
      re2-dev \
      openssl-dev \
      zlib-dev \
      spdlog-dev \
      gflags-dev \
      gtest-dev \
      glaze \
    && ln -sf /usr/bin/llvm22-symbolizer /usr/bin/llvm-symbolizer

# EventLoop isn't packaged and has no install rules upstream, so it is built from source here,
# applying the same patch the vcpkg overlay port uses (vcpkg/ports/eventloop/add-install-rules.patch)
# so the install rules exist in exactly one place, not duplicated between vcpkg and CI.
#
# Built once per compiler since the CI matrix exercises both GCC and Clang builds of this project,
# each EventLoop copy installed to its own prefix so the two don't collide.
ARG EVENTLOOP_REF=e16fa52aae1d58a994fd93808c62fde14adb62f1
COPY vcpkg/ports/eventloop/add-install-rules.patch /tmp/add-install-rules.patch

RUN git clone https://github.com/amoldhamale1105/EventLoop.git /tmp/EventLoop \
    && cd /tmp/EventLoop \
    && git checkout "${EVENTLOOP_REF}" \
    && git apply /tmp/add-install-rules.patch \
    && cmake -B build-gcc -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
         -DCMAKE_CXX_COMPILER=g++ -DCMAKE_INSTALL_PREFIX=/opt/eventloop-gcc \
    && cmake --build build-gcc \
    && cmake --install build-gcc \
    && cmake -B build-clang -GNinja -DCMAKE_BUILD_TYPE=Release -DCMAKE_POLICY_VERSION_MINIMUM=3.5 \
         -DCMAKE_CXX_COMPILER=clang++ -DCMAKE_INSTALL_PREFIX=/opt/eventloop-clang \
    && cmake --build build-clang \
    && cmake --install build-clang \
    && cd / && rm -rf /tmp/EventLoop /tmp/add-install-rules.patch
