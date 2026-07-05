#include <iostream>
#include <string>

#include <grpcpp/grpcpp.h>

#include "smoke.grpc.pb.h"
#include "smoke.pb.h"

int main() {
  // Proves protobuf codegen + libprotobuf link.
  smoke::PingRequest request;
  request.set_message("hello");
  std::string serialized;
  request.SerializeToString(&serialized);

  // Proves gRPC codegen + grpc++ link (stub construction only, no server needed).
  auto channel = grpc::CreateChannel("localhost:0", grpc::InsecureChannelCredentials());
  auto stub = smoke::Smoke::NewStub(channel);

  std::cout << "protobuf + gRPC codegen/link OK: serialized " << serialized.size()
            << " bytes, stub=" << (stub != nullptr) << "\n";
  return 0;
}
