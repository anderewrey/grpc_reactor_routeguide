#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>

#include <Event.h>
#include <EventLoop.h>

#include <functional>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#include "generated/route_guide.grpc.pb.h"

#include "common/db_utils.h"
#include "proto/proto_utils.h"

#include "client/reactor_client_routeguide.h"

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
 ************************/
class RouteGuideClient {
  static constexpr auto kGetFeatureOnDone{"GetFeatureOnDone"};
  static constexpr auto kListFeaturesOnReadDoneOk{"ListFeaturesOnReadDoneOk"};
  static constexpr auto kListFeaturesOnReadDoneNOk{"ListFeaturesOnReadDoneNOk"};
  static constexpr auto kListFeaturesOnDone{"ListFeaturesOnDone"};
 public:
  explicit RouteGuideClient(const std::shared_ptr<grpc::Channel>& channel)
      : stub_(routeguide::RouteGuide::NewStub(channel)) {
    EventLoop::RegisterEvent(kGetFeatureOnDone, [&reactor_ = reactor_map_[routeguide::GetFeature::RpcKey]](const Event* event) {
      // (Point 3.5) ProceedEvent: OnDone
      assert(main_thread == std::this_thread::get_id());  // application thread
      auto* reactor = static_cast<routeguide::GetFeature::ClientReactor*>(event->getData());
      assert(reactor == reactor_.get());
      const auto status = reactor->Status();
      if (status.ok()) {
        // (Point 3.6) extracts response
        routeguide::Feature response;
        reactor->GetResponse(response);
        // (Point 3.7) update application with response
        std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "] RESPONSE | " << response.GetTypeName() << ": " << response.ShortDebugString() << std::endl;
      } else {
        std::cerr << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "]          | " << event->getName() << " reactor: " << reactor << " Status: OK: " << status.ok() << " msg: " << status.error_message() << std::endl;
      }
      // (Point 3.8) Destroy reactor
      reactor_.reset();
      std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "]          | reactor[" << reactor << "] ended" << std::endl;
    });
    EventLoop::RegisterEvent(kListFeaturesOnReadDoneOk, [this, &reactor_ = reactor_map_[routeguide::ListFeatures::RpcKey]](const Event* event) {
      // (Point 2.7) ProceedEvent: OnReadDoneOk
      assert(main_thread == std::this_thread::get_id());  // application thread
      auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
      assert(reactor == reactor_.get());
      // (Point 2.8, 2.9, 2.10, 2.11) extracts response and restart RPC
      routeguide::Feature response;
      reactor->GetResponse(response);
      // (Point 2.12) update application with response
      std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "] RESPONSE | " << response.GetTypeName() << ": " << response.ShortDebugString() << std::endl;
#if 0
      // Triggering extra concurrency: Un-comment that #IF block to probe the refusal of concurrent RPC calls.
      // Each received result from stream is reused to trigger a concurrent unary RPC request. If the RPC already has
      // a pending operation, the new call is refused.
      GetFeature(response.location());
#endif
    });
    EventLoop::RegisterEvent(kListFeaturesOnReadDoneNOk, [&reactor_ = reactor_map_[routeguide::ListFeatures::RpcKey]](const Event* event) {
      // (Point 4.7) ProceedEvent: OnReadDoneNOk
      assert(main_thread == std::this_thread::get_id());  // application thread
      auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
      assert(reactor == reactor_.get());
      // (Point 4.8) update application
      std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "]          | " << event->getName() << " reactor: " << reactor << std::endl;
    });
    EventLoop::RegisterEvent(kListFeaturesOnDone, [&reactor_ = reactor_map_[routeguide::ListFeatures::RpcKey]](const Event* event) {
      // (Point 4.9) ProceedEvent: OnDone
      assert(main_thread == std::this_thread::get_id());  // application thread
      auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
      assert(reactor == reactor_.get());
      const auto status = reactor->Status();
      // (Point 4.8) update application with status
      std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "]          | " << event->getName() << " reactor: " << reactor /*<< " ok: " << ok*/ << std::endl;
      // (Point 4.11) Destroy reactor
      reactor_.reset();
      std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "]          | reactor[" << reactor << "] ended" << std::endl;
    });
  }

  void GetFeature(routeguide::Point point) {
    using routeguide::GetFeature::ClientReactor;
    using routeguide::GetFeature::RpcKey;
    if (reactor_map_[RpcKey]) {
      std::cerr << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "]          | reactor[" << reactor_map_[RpcKey] << "] already in execution, ignoring: " << point.ShortDebugString() << std::endl;
      return;
    }
    ClientReactor::callbacks cbs;
    // (Point 3.4) TriggerEvent: OnDone
    cbs.done = std::bind(static_cast<void(*)(const std::string&, void*)>(&EventLoop::TriggerEvent),
                         kGetFeatureOnDone, std::placeholders::_1);
    // (Point 1.1) Create reactor
    reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_,
                                                           std::move(CreateClientContext()),
                                                           std::move(point),
                                                           std::move(cbs));
    std::cout << "RouteGuide::GetFeature[" << std::this_thread::get_id() << "]          | reactor[" << reactor_map_[RpcKey] << "] created" << std::endl;
  }

  void ListFeatures(routeguide::Rectangle rect) {
    using routeguide::ListFeatures::ClientReactor;
    using routeguide::ListFeatures::RpcKey;
    if (reactor_map_[RpcKey]) {
      std::cerr << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "]          | reactor[" << reactor_map_[RpcKey] << "] already in execution, ignoring: " << rect.ShortDebugString() << std::endl;
      return;
    }

    ClientReactor::callbacks cbs;
    // (Point 2.4) TriggerEvent: OnReadDoneOk
    cbs.ok = [](auto* reactor, const routeguide::Feature&) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kListFeaturesOnReadDoneOk, reactor);
      return true;  // true: hold the RPC until the application proceeded the response
    };
    // (Point 4.3) TriggerEvent: OnReadDoneNOk
    cbs.nok = std::bind(static_cast<void(*)(const std::string&, void*)>(&EventLoop::TriggerEvent),
                        kListFeaturesOnReadDoneNOk, std::placeholders::_1);
    // (Point 4.6) TriggerEvent: OnDone
    cbs.done = std::bind(static_cast<void(*)(const std::string&, void*)>(&EventLoop::TriggerEvent),
                         kListFeaturesOnDone, std::placeholders::_1);

    // (Point 1.1) Create reactor
    reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_,
                                                           std::move(CreateClientContext()),
                                                           std::move(rect),
                                                           std::move(cbs));
    std::cout << "RouteGuide::ListFeatures[" << std::this_thread::get_id() << "]          | reactor[" << reactor_map_[RpcKey] << "] created" << std::endl;
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
  // Expect only arg: --db_path=path/to/route_guide_db.json.
  db_utils::ParseDb(db_utils::GetDbFileContent(argc, argv), feature_list_);
  RouteGuideClient guide(grpc::CreateChannel("localhost:50051", grpc::InsecureChannelCredentials()));

  std::cout << "-------------- ListFeatures --------------" << std::endl;
  guide.ListFeatures(proto_utils::MakeRectangle(400000000, -750000000, 420000000, -730000000));
  std::cout << "-------------- GetFeature --------------" << std::endl;
  guide.GetFeature(proto_utils::GetRandomPoint(feature_list_));
  EventLoop::Run();
  std::cout << "-------------- LEAVING APPLICATION --------------" << std::endl;
  return 0;
}
