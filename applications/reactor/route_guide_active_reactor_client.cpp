///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>

#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <gflags/gflags.h>

#include <Event.h>
#include <EventLoop.h>

#include <functional>
#include <memory>
#include <map>
#include <string>
#include <thread>
#include <utility>

#include "rg_service/route_guide_service.h"

#include "rg_service/rg_db.h"
#include "rg_service/rg_utils.h"
#include "protobuf_utils/protobuf_utils.h"

#include "applications/reactor/reactor_client_routeguide.h"

namespace {
std::thread::id main_thread = std::this_thread::get_id();
FeatureList feature_list_;

std::unique_ptr<grpc::ClientContext> CreateClientContext() {
  auto context = std::make_unique<grpc::ClientContext>();
  // context->set_wait_for_ready(true);
  return context;
}
}  // anonymous namespace

/************************
 * RPC handling: Following code belongs to the application.
 * That code is hosted on the application project, not auto-generated.
 * It also does the making and reading of proto messages. A good practice
 * would be to not expose the message types nor the `Status` outside of here.
 *
 * Active Object Pattern implementation:
 * - RouteGuideClient methods (GetFeature, ListFeatures) = Proxy components
 * - EventLoop::RegisterEvent handlers = Servant components
 * - EventLoop::Run() in main = Scheduler component
 * - Main thread serves as the Active Object's thread
 * See reactor_client.md for detailed Active Object pattern documentation.
 ************************/
class RouteGuideClient {
  static constexpr auto kGetFeatureOnDone{"GetFeatureOnDone"};
  static constexpr auto kListFeaturesOnReadDoneOk{"ListFeaturesOnReadDoneOk"};
  static constexpr auto kListFeaturesOnReadDoneNOk{"ListFeaturesOnReadDoneNOk"};
  static constexpr auto kListFeaturesOnDone{"ListFeaturesOnDone"};

 public:
  /// Constructor registers Servant handlers with EventLoop (Scheduler).
  /// Servant components: Event handlers that process RPC responses on application thread.
  explicit RouteGuideClient(const std::shared_ptr<grpc::Channel>& channel)
      : stub_(routeguide::RouteGuide::NewStub(channel)) {
    EventLoop::RegisterEvent(kGetFeatureOnDone,
                             [&reactor_ = reactor_map_[routeguide::GetFeature::RpcKey],
                              &logger = routeguide::logger::Get(routeguide::RpcMethods::kGetFeature)](const EventLoop::Event* event) {
      // (Point 3.5) ProceedEvent: OnDone
      assert(main_thread == std::this_thread::get_id());  // application thread
      auto* reactor = static_cast<routeguide::GetFeature::ClientReactor*>(event->getData());
      assert(reactor == reactor_.get());
      if (const auto status = reactor->Status(); status.ok()) {
        // (Point 3.6) extracts response
        routeguide::GetFeature::ResponseT response;
        reactor->GetResponse(response);
        // (Point 3.7) update application with response
        logger.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
      } else {
        logger.info("         | {} reactor: {} Status: OK: {} msg: {}",
                    event->getName(), fmt::ptr(reactor), status.ok(), status.error_message());
      }
      // (Point 3.8) Destroy reactor
      reactor_.reset();
      logger.info("         | reactor[{}] ended", fmt::ptr(reactor));
    });
    EventLoop::RegisterEvent(kListFeaturesOnReadDoneOk,
                             [this, &reactor_ = reactor_map_[routeguide::ListFeatures::RpcKey],
                              &logger = routeguide::logger::Get(routeguide::RpcMethods::kListFeatures)](const EventLoop::Event* event) {
      // (Point 2.7) ProceedEvent: OnReadDoneOk
      assert(main_thread == std::this_thread::get_id());  // application thread
      auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
      assert(reactor == reactor_.get());
      // (Point 2.8, 2.9, 2.10, 2.11) extracts response and restart RPC
      routeguide::ListFeatures::ResponseT response;
      reactor->GetResponse(response);
      // (Point 2.12) update application with response
      logger.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
#if 0
      // Triggering extra concurrency: Un-comment that #IF block to probe the refusal of concurrent RPC calls.
      // Each received result from stream is reused to trigger a concurrent unary RPC request. If the RPC already has
      // a pending operation, the new call is refused.
      GetFeature(response.location());
#endif
    });
    EventLoop::RegisterEvent(kListFeaturesOnReadDoneNOk,
                             [&reactor_ = reactor_map_[routeguide::ListFeatures::RpcKey],
                              &logger = routeguide::logger::Get(routeguide::RpcMethods::kListFeatures)](const EventLoop::Event* event) {
      // (Point 4.7) ProceedEvent: OnReadDoneNOk
      assert(main_thread == std::this_thread::get_id());  // application thread
      auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
      assert(reactor == reactor_.get());
      // (Point 4.8) update application
      logger.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
    });
    EventLoop::RegisterEvent(kListFeaturesOnDone,
                             [&reactor_ = reactor_map_[routeguide::ListFeatures::RpcKey],
                              &logger = routeguide::logger::Get(routeguide::RpcMethods::kListFeatures)](const EventLoop::Event* event) {
      // (Point 4.9) ProceedEvent: OnDone
      assert(main_thread == std::this_thread::get_id());  // application thread
      auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
      assert(reactor == reactor_.get());
      const auto status = reactor->Status();
      // (Point 4.8) update application with status
      logger.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
      // (Point 4.11) Destroy reactor
      reactor_.reset();
      logger.info("         | reactor[{}] ended", fmt::ptr(reactor));
    });
  }

  /// Proxy component: Client-facing method that creates Method Request and returns immediately.
  /// Runs on client thread (main application thread).
  void GetFeature(routeguide::Point point) {
    using routeguide::GetFeature::ClientReactor;
    using routeguide::GetFeature::Callbacks;
    using routeguide::GetFeature::ResponseT;
    using routeguide::GetFeature::RpcKey;
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kGetFeature);
    if (reactor_map_[RpcKey]) {
      logger.info("         | reactor[{}] already in execution, ignoring: {}",
                  fmt::ptr(reactor_map_[RpcKey].get()), protobuf_utils::ToString(point));
      return;
    }
    Callbacks cbs;
    // (Point 3.4) TriggerEvent: OnDone
    cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kGetFeatureOnDone, reactor);
    };

    // (Point 1.1) Create reactor
    reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_,
                                                           std::move(CreateClientContext()),
                                                           std::move(point),
                                                           std::move(cbs));
    logger.info("         | reactor[{}] created", fmt::ptr(reactor_map_[RpcKey].get()));
  }

  /// Proxy component: Client-facing method that creates Method Request and returns immediately.
  /// Runs on client thread (main application thread).
  void ListFeatures(routeguide::Rectangle rect) {
    using routeguide::ListFeatures::ClientReactor;
    using routeguide::ListFeatures::Callbacks;
    using routeguide::ListFeatures::ResponseT;
    using routeguide::ListFeatures::RpcKey;
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kListFeatures);

    if (reactor_map_[RpcKey]) {
      logger.info("         | reactor[{}] already in execution, ignoring: {}",
                  fmt::ptr(reactor_map_[RpcKey].get()), protobuf_utils::ToString(rect));
      return;
    }

    Callbacks cbs;
    // (Point 2.4) TriggerEvent: OnReadDoneOk
    cbs.ok = [](auto* reactor, const ResponseT&) -> bool {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kListFeaturesOnReadDoneOk, reactor);
      return true;  // true: hold the RPC until the application proceeded the response
    };
    // (Point 4.3) TriggerEvent: OnReadDoneNOk
    cbs.nok = [](auto* reactor) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kListFeaturesOnReadDoneNOk, reactor);
    };
    // (Point 4.6) TriggerEvent: OnDone
    cbs.done = [](auto* reactor, const grpc::Status&) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kListFeaturesOnDone, reactor);
    };

    // (Point 1.1) Create reactor
    reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_,
                                                           std::move(CreateClientContext()),
                                                           std::move(rect),
                                                           std::move(cbs));
    logger.info("         | reactor[{}] created", fmt::ptr(reactor_map_[RpcKey].get()));
  }

 private:
  // Access interface to the API RPCs
  std::unique_ptr<routeguide::RouteGuide::Stub> stub_;
  // Container of all RPC reactor instances. A new dedicated instance must be created for each RPC call and be destroyed
  // once the RPC is done (i.e. 'OnDone' event)
  std::map<routeguide::RpcMethods, std::unique_ptr<grpc::internal::ClientReactor>> reactor_map_;
};

int main(int argc, char** argv) {
  assert(main_thread == std::this_thread::get_id());
  spdlog::set_pattern("[%H:%M:%S.%f][%n][%t][%^%L%$] %v");
  auto logger_Main = spdlog::stdout_color_mt("Main");
  spdlog::set_default_logger(logger_Main);

  gflags::ParseCommandLineFlags(&argc, &argv, true);

  feature_list_ = rg_db::GetDbFileContent();
  RouteGuideClient guide(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));

  spdlog::info("-------------- ListFeatures --------------");
  guide.ListFeatures(rg_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000));
  spdlog::info("-------------- GetFeature --------------");
  guide.GetFeature(rg_utils::GetRandomPoint(feature_list_));
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
