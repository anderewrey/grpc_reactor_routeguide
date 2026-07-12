///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <gtest/gtest.h>

#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <grpcpp/security/server_credentials.h>
#include <grpcpp/server.h>
#include <grpcpp/server_builder.h>

#include <memory>
#include <string>

#include "rg_service/route_guide_service.h"

/// Base test fixture bringing up an in-process RouteGuide server on a dynamic port and a client
/// stub connected to it. ServiceT is the fake routeguide::RouteGuide::CallbackService
/// implementation the test registers; each RPC's test suite supplies its own, since each exercises
/// a different RPC method.
template <class ServiceT>
class RouteGuideTestFixtureBase : public ::testing::Test {
 protected:
  void SetUp() override {
    grpc::ServerBuilder builder;
    builder.RegisterService(&test_service_);
    int selected_port = 0;
    builder.AddListeningPort("localhost:0", grpc::InsecureServerCredentials(), &selected_port);
    server_ = builder.BuildAndStart();
    ASSERT_NE(server_, nullptr) << "Failed to start in-process server";
    ASSERT_GT(selected_port, 0) << "Failed to get dynamic port";

    std::string server_address = "localhost:" + std::to_string(selected_port);
    channel_ = grpc::CreateChannel(server_address, grpc::InsecureChannelCredentials());
    stub_ = routeguide::RouteGuide::NewStub(channel_);
  }

  void TearDown() override {
    if (server_) {
      server_->Shutdown();
    }
  }

  std::unique_ptr<grpc::ClientContext> CreateClientContext() {
    return std::make_unique<grpc::ClientContext>();
  }

  ServiceT test_service_;
  std::unique_ptr<grpc::Server> server_;
  std::shared_ptr<grpc::Channel> channel_;
  std::unique_ptr<routeguide::RouteGuide::Stub> stub_;
};
