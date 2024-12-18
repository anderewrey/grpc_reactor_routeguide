FROM ubuntu:24.04

# With Ubuntu 24.04, the packages are:
# - clang 18.1.3
# - cmake 3.28.3
# - gcc 13.3.0
# - gdb 15.0.50
# - git 2.43.0
# - make 4.3
# - ninja 1.11.1
RUN apt-get update -y && \
    apt-get install -y cmake ninja-build clang build-essential g++ gdb git iproute2 pkg-config net-tools && \
    rm -rf /var/lib/apt/lists/*

ENV GRPC_SOURCE_PATH="/grpc"
ENV GRPC_BUILD_PATH="$GRPC_SOURCE_PATH/cmake/build"

RUN export MY_INSTALL_DIR="$HOME/.local" && \
    mkdir -p $MY_INSTALL_DIR && \
    export PATH="$MY_INSTALL_DIR/bin:$PATH" && \
    echo "MY_INSTALL_DIR: $MY_INSTALL_DIR" && \
    echo "GRPC_SOURCE_PATH: $GRPC_SOURCE_PATH" && \
    echo "GRPC_BUILD_PATH: $GRPC_BUILD_PATH"

RUN git clone --recurse-submodules -b v1.68.2 --depth 1 \
        --shallow-submodules https://github.com/grpc/grpc $GRPC_SOURCE_PATH && \
    mkdir -p $GRPC_BUILD_PATH && \
    cmake -G Ninja -B $GRPC_BUILD_PATH -S $GRPC_SOURCE_PATH \
          -DCMAKE_CXX_COMPILER=g++ -DCMAKE_C_COMPILER=gcc \
          -DgRPC_INSTALL=ON \
          -DgRPC_BUILD_TESTS=OFF \
          -DgRPC_BUILD_GRPCPP_OTEL_PLUGIN=OFF \
          -DgRPC_BUILD_GRPC_CPP_PLUGIN=ON \
          -DgRPC_BUILD_GRPC_CSHARP_PLUGIN=OFF \
          -DgRPC_BUILD_GRPC_NODE_PLUGIN=OFF \
          -DgRPC_BUILD_GRPC_OBJECTIVE_C_PLUGIN=OFF \
          -DgRPC_BUILD_GRPC_PHP_PLUGIN=OFF \
          -DgRPC_BUILD_GRPC_PYTHON_PLUGIN=OFF \
          -DgRPC_BUILD_GRPC_RUBY_PLUGIN=OFF \
          -DCMAKE_INSTALL_PREFIX=$MY_INSTALL_DIR && \
    ninja -C $GRPC_BUILD_PATH -j 7 install && \
    rm -Rf $GRPC_SOURCE_PATH
