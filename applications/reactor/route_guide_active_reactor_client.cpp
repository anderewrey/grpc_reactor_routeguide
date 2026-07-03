///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2024-2025 anderewrey
///

#include <Event.h>
#include <EventLoop.h>
#include <gflags/gflags.h>
#include <grpc/grpc.h>
#include <grpcpp/channel.h>
#include <grpcpp/client_context.h>
#include <grpcpp/create_channel.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <spdlog/spdlog.h>

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "protobuf_utils/protobuf_utils.h"
#include "rg_service/rg_db.h"
#include "rg_service/rg_utils.h"
#include "rg_service/route_guide_service.h"

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
 * Active Object Pattern Implementation
 *
 * This application demonstrates the Active Object pattern combined with the
 * Reactor pattern for event-driven asynchronous RPC handling.
 *
 * Component mapping:
 * - RouteGuideClient methods (GetFeature, ListFeatures, RecordRoute, RouteChat) = Proxy
 * - ClientReactor classes (ActiveUnaryReactor, ActiveReadReactor, ActiveWriteReactor,
 *   ActiveBidiReactor) = Method Request
 * - EventLoop = Scheduler + Activation Queue
 * - EventLoop::RegisterEvent handlers = Servant (business logic placeholder)
 * - GetResponse() / Status() = Future
 * - AddHold/RemoveHold = Guards (prevent concurrent access)
 *
 * Note: This demo uses simple logging as Servant logic. Production applications
 * would implement actual business logic in the event handlers.
 *
 * See reactor_client.md for detailed pattern documentation.
 ************************/
class RouteGuideClient {
  static constexpr auto kGetFeatureOnDone{"GetFeatureOnDone"};
  static constexpr auto kListFeaturesOnReadDoneOk{"ListFeaturesOnReadDoneOk"};
  static constexpr auto kListFeaturesOnReadDoneNOk{"ListFeaturesOnReadDoneNOk"};
  static constexpr auto kListFeaturesOnDone{"ListFeaturesOnDone"};
  static constexpr auto kRecordRouteOnWriteDone{"RecordRouteOnWriteDone"};
  static constexpr auto kRecordRouteOnDone{"RecordRouteOnDone"};
  static constexpr auto kRouteChatOnReadDoneOk{"RouteChatOnReadDoneOk"};
  static constexpr auto kRouteChatOnReadDoneNOk{"RouteChatOnReadDoneNOk"};
  static constexpr auto kRouteChatOnWriteDone{"RouteChatOnWriteDone"};
  static constexpr auto kRouteChatOnDone{"RouteChatOnDone"};

 public:
  /// Constructor registers response handlers with EventLoop (Scheduler).
  /// Response handlers process RPC responses on the application thread (adapted Servant role).
  /// Each handler is owned by an EventConnection member, so it is deregistered automatically
  /// when this object is destroyed.
  explicit RouteGuideClient(const std::shared_ptr<grpc::Channel>& channel)
      : stub_(routeguide::RouteGuide::NewStub(channel)),
        get_feature_on_done_(
            kGetFeatureOnDone,
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
                logger.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor),
                            status.ok(), status.error_message());
              }
              // (Point 3.8) Destroy reactor
              reactor_.reset();
              logger.info("         | reactor[{}] ended", fmt::ptr(reactor));
            }),
        list_features_on_read_done_ok_(
            kListFeaturesOnReadDoneOk,
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
          // Each received result from stream is reused to trigger a concurrent unary RPC request. If the RPC already
          // has a pending operation, the new call is refused.
          GetFeature(response.location());
#endif
            }),
        list_features_on_read_done_nok_(
            kListFeaturesOnReadDoneNOk,
            [&reactor_ = reactor_map_[routeguide::ListFeatures::RpcKey],
             &logger = routeguide::logger::Get(routeguide::RpcMethods::kListFeatures)](const EventLoop::Event* event) {
              // (Point 4.7) ProceedEvent: OnReadDoneNOk
              assert(main_thread == std::this_thread::get_id());  // application thread
              auto* reactor = static_cast<routeguide::ListFeatures::ClientReactor*>(event->getData());
              assert(reactor == reactor_.get());
              // (Point 4.8) update application
              logger.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
            }),
        list_features_on_done_(
            kListFeaturesOnDone,
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
            }),
        record_route_on_write_done_(
            kRecordRouteOnWriteDone,
            [this, &reactor_ = reactor_map_[routeguide::RecordRoute::RpcKey],
             &logger = routeguide::logger::Get(routeguide::RpcMethods::kRecordRoute)](const EventLoop::Event* event) {
              // ProceedEvent: OnWriteDone
              assert(main_thread == std::this_thread::get_id());  // application thread
              auto* reactor = static_cast<routeguide::RecordRoute::ClientReactor*>(event->getData());
              assert(reactor == reactor_.get());
              logger.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
              // The point already sent to SendLastRequest() also triggers OnWriteDone, so check the
              // pending list rather than unconditionally sending: it is empty once the last point
              // sent was the final one.
              if (!record_route_pending_.empty()) {
                SendNextRecordRoutePoint();
              }
            }),
        record_route_on_done_(
            kRecordRouteOnDone,
            [&reactor_ = reactor_map_[routeguide::RecordRoute::RpcKey],
             &logger = routeguide::logger::Get(routeguide::RpcMethods::kRecordRoute)](const EventLoop::Event* event) {
              // ProceedEvent: OnDone
              assert(main_thread == std::this_thread::get_id());  // application thread
              auto* reactor = static_cast<routeguide::RecordRoute::ClientReactor*>(event->getData());
              assert(reactor == reactor_.get());
              if (const auto status = reactor->Status(); status.ok()) {
                routeguide::RecordRoute::ResponseT response;
                reactor->GetResponse(response);
                logger.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
              } else {
                logger.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor),
                            status.ok(), status.error_message());
              }
              reactor_.reset();
              logger.info("         | reactor[{}] ended", fmt::ptr(reactor));
            }),
        route_chat_on_read_done_ok_(
            kRouteChatOnReadDoneOk,
            [&reactor_ = reactor_map_[routeguide::RouteChat::RpcKey],
             &logger = routeguide::logger::Get(routeguide::RpcMethods::kRouteChat)](const EventLoop::Event* event) {
              // ProceedEvent: OnReadDoneOk
              assert(main_thread == std::this_thread::get_id());  // application thread
              auto* reactor = static_cast<routeguide::RouteChat::ClientReactor*>(event->getData());
              assert(reactor == reactor_.get());
              routeguide::RouteChat::ResponseT response;
              reactor->GetResponse(response);
              logger.info("RESPONSE | {}: {}", response.GetTypeName(), protobuf_utils::ToString(response));
            }),
        route_chat_on_read_done_nok_(
            kRouteChatOnReadDoneNOk,
            [&reactor_ = reactor_map_[routeguide::RouteChat::RpcKey],
             &logger = routeguide::logger::Get(routeguide::RpcMethods::kRouteChat)](const EventLoop::Event* event) {
              // ProceedEvent: OnReadDoneNOk
              assert(main_thread == std::this_thread::get_id());  // application thread
              auto* reactor = static_cast<routeguide::RouteChat::ClientReactor*>(event->getData());
              assert(reactor == reactor_.get());
              logger.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
            }),
        route_chat_on_write_done_(
            kRouteChatOnWriteDone,
            [this, &reactor_ = reactor_map_[routeguide::RouteChat::RpcKey],
             &logger = routeguide::logger::Get(routeguide::RpcMethods::kRouteChat)](const EventLoop::Event* event) {
              // ProceedEvent: OnWriteDone
              assert(main_thread == std::this_thread::get_id());  // application thread
              auto* reactor = static_cast<routeguide::RouteChat::ClientReactor*>(event->getData());
              assert(reactor == reactor_.get());
              logger.info("         | {} reactor: {}", event->getName(), fmt::ptr(reactor));
              if (!route_chat_pending_.empty()) {
                SendNextRouteChatNote();
              }
            }),
        route_chat_on_done_(kRouteChatOnDone, [&reactor_ = reactor_map_[routeguide::RouteChat::RpcKey],
                                               &logger = routeguide::logger::Get(routeguide::RpcMethods::kRouteChat)](
                                                  const EventLoop::Event* event) {
          // ProceedEvent: OnDone
          assert(main_thread == std::this_thread::get_id());  // application thread
          auto* reactor = static_cast<routeguide::RouteChat::ClientReactor*>(event->getData());
          assert(reactor == reactor_.get());
          const auto status = reactor->Status();
          logger.info("         | {} reactor: {} Status: OK: {} msg: {}", event->getName(), fmt::ptr(reactor),
                      status.ok(), status.error_message());
          reactor_.reset();
          logger.info("         | reactor[{}] ended", fmt::ptr(reactor));
        }) {
  }

  /// Proxy component: Client-facing method that creates Method Request and returns immediately.
  /// Runs on client thread (main application thread).
  void GetFeature(routeguide::Point point) {
    using routeguide::GetFeature::Callbacks;
    using routeguide::GetFeature::ClientReactor;
    using routeguide::GetFeature::ResponseT;
    using routeguide::GetFeature::RpcKey;
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kGetFeature);
    if (reactor_map_[RpcKey]) {
      logger.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_map_[RpcKey].get()),
                  protobuf_utils::ToString(point));
      return;
    }
    Callbacks cbs;
    // (Point 3.4) TriggerEvent: OnDone
    cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kGetFeatureOnDone, reactor);
    };

    // (Point 1.1) Create reactor
    reactor_map_[RpcKey] =
        std::make_unique<ClientReactor>(*stub_, std::move(CreateClientContext()), std::move(point), std::move(cbs));
    logger.info("         | reactor[{}] created", fmt::ptr(reactor_map_[RpcKey].get()));
  }

  /// Proxy component: Client-facing method that creates Method Request and returns immediately.
  /// Runs on client thread (main application thread).
  void ListFeatures(routeguide::Rectangle rect) {
    using routeguide::ListFeatures::Callbacks;
    using routeguide::ListFeatures::ClientReactor;
    using routeguide::ListFeatures::ResponseT;
    using routeguide::ListFeatures::RpcKey;
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kListFeatures);

    if (reactor_map_[RpcKey]) {
      logger.info("         | reactor[{}] already in execution, ignoring: {}", fmt::ptr(reactor_map_[RpcKey].get()),
                  protobuf_utils::ToString(rect));
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
    reactor_map_[RpcKey] =
        std::make_unique<ClientReactor>(*stub_, std::move(CreateClientContext()), std::move(rect), std::move(cbs));
    logger.info("         | reactor[{}] created", fmt::ptr(reactor_map_[RpcKey].get()));
  }

  /// Proxy component: Client-facing method that creates Method Request and returns immediately.
  /// Runs on client thread (main application thread). The points are sent to the server one at a
  /// time, waiting for each OnWriteDone before sending the next, since gRPC allows only one write
  /// in flight per stream.
  void RecordRoute(std::vector<routeguide::Point> points) {
    using routeguide::RecordRoute::Callbacks;
    using routeguide::RecordRoute::ClientReactor;
    using routeguide::RecordRoute::ResponseT;
    using routeguide::RecordRoute::RpcKey;
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kRecordRoute);
    if (reactor_map_[RpcKey]) {
      logger.info("         | reactor[{}] already in execution, ignoring {} points",
                  fmt::ptr(reactor_map_[RpcKey].get()), points.size());
      return;
    }
    if (points.empty()) {
      logger.info("         | no points to send, ignoring");
      return;
    }
    record_route_pending_ = std::move(points);

    Callbacks cbs;
    // TriggerEvent: OnWriteDone
    cbs.write_done = [](auto* reactor, bool) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kRecordRouteOnWriteDone, reactor);
    };
    // TriggerEvent: OnDone
    cbs.done = [](auto* reactor, const grpc::Status&, const ResponseT&) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kRecordRouteOnDone, reactor);
    };

    // (Point 1.1) Create reactor
    reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), std::move(cbs));
    logger.info("         | reactor[{}] created", fmt::ptr(reactor_map_[RpcKey].get()));
    SendNextRecordRoutePoint();
  }

  /// Proxy component: Client-facing method that creates Method Request and returns immediately.
  /// Runs on client thread (main application thread). The notes are sent to the server one at a
  /// time (same one-write-in-flight constraint as RecordRoute), while responses can arrive on the
  /// independent read side at any time.
  void RouteChat(std::vector<routeguide::RouteNote> notes) {
    using routeguide::RouteChat::Callbacks;
    using routeguide::RouteChat::ClientReactor;
    using routeguide::RouteChat::ResponseT;
    using routeguide::RouteChat::RpcKey;
    auto& logger = routeguide::logger::Get(routeguide::RpcMethods::kRouteChat);
    if (reactor_map_[RpcKey]) {
      logger.info("         | reactor[{}] already in execution, ignoring {} notes",
                  fmt::ptr(reactor_map_[RpcKey].get()), notes.size());
      return;
    }
    if (notes.empty()) {
      logger.info("         | no notes to send, ignoring");
      return;
    }
    route_chat_pending_ = std::move(notes);

    Callbacks cbs;
    // TriggerEvent: OnReadDoneOk
    cbs.read_ok = [](auto* reactor, const ResponseT&) -> bool {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kRouteChatOnReadDoneOk, reactor);
      return true;  // true: hold the RPC until the application proceeded the response
    };
    // TriggerEvent: OnReadDoneNOk
    cbs.read_nok = [](auto* reactor) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kRouteChatOnReadDoneNOk, reactor);
    };
    // TriggerEvent: OnWriteDone
    cbs.write_done = [](auto* reactor, bool) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kRouteChatOnWriteDone, reactor);
    };
    // TriggerEvent: OnDone
    cbs.done = [](auto* reactor, const grpc::Status&) {
      assert(main_thread != std::this_thread::get_id());  // gRPC thread
      EventLoop::TriggerEvent(kRouteChatOnDone, reactor);
    };

    // (Point 1.1) Create reactor
    reactor_map_[RpcKey] = std::make_unique<ClientReactor>(*stub_, CreateClientContext(), std::move(cbs));
    logger.info("         | reactor[{}] created", fmt::ptr(reactor_map_[RpcKey].get()));
    SendNextRouteChatNote();
  }

 private:
  /// Sends the next point queued for RecordRoute. Called once to fire the first write after
  /// creating the reactor, and again from the OnWriteDone handler until the list is exhausted.
  /// The last point is sent via SendLastRequest() to close the stream in the same operation.
  void SendNextRecordRoutePoint() {
    auto* reactor =
        static_cast<routeguide::RecordRoute::ClientReactor*>(reactor_map_[routeguide::RecordRoute::RpcKey].get());
    auto point = std::move(record_route_pending_.front());
    record_route_pending_.erase(record_route_pending_.begin());
    if (record_route_pending_.empty()) {
      reactor->SendLastRequest(std::move(point));
    } else {
      reactor->SendRequest(std::move(point));
    }
  }

  /// Sends the next note queued for RouteChat. Called once to fire the first write after creating
  /// the reactor, and again from the OnWriteDone handler until the list is exhausted.
  /// The last note is sent via SendLastRequest() to close the client's write side; the server may
  /// still send further responses afterward.
  void SendNextRouteChatNote() {
    auto* reactor =
        static_cast<routeguide::RouteChat::ClientReactor*>(reactor_map_[routeguide::RouteChat::RpcKey].get());
    auto note = std::move(route_chat_pending_.front());
    route_chat_pending_.erase(route_chat_pending_.begin());
    if (route_chat_pending_.empty()) {
      reactor->SendLastRequest(std::move(note));
    } else {
      reactor->SendRequest(std::move(note));
    }
  }

  // Access interface to the API RPCs
  std::unique_ptr<routeguide::RouteGuide::Stub> stub_;
  // Container of all RPC reactor instances. A new dedicated instance must be created for each RPC call and be destroyed
  // once the RPC is done (i.e. 'OnDone' event)
  std::map<routeguide::RpcMethods, std::unique_ptr<grpc::internal::ClientReactor>> reactor_map_;
  // Points/notes still to be sent for the in-flight RecordRoute/RouteChat call, one at a time.
  std::vector<routeguide::Point> record_route_pending_;
  std::vector<routeguide::RouteNote> route_chat_pending_;
  // EventLoop handler registrations, one per event name used above. Declared after reactor_map_
  // so it already exists when these are constructed, since their callbacks capture entries of it.
  RpcReactor::EventConnection get_feature_on_done_;
  RpcReactor::EventConnection list_features_on_read_done_ok_;
  RpcReactor::EventConnection list_features_on_read_done_nok_;
  RpcReactor::EventConnection list_features_on_done_;
  RpcReactor::EventConnection record_route_on_write_done_;
  RpcReactor::EventConnection record_route_on_done_;
  RpcReactor::EventConnection route_chat_on_read_done_ok_;
  RpcReactor::EventConnection route_chat_on_read_done_nok_;
  RpcReactor::EventConnection route_chat_on_write_done_;
  RpcReactor::EventConnection route_chat_on_done_;
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
  spdlog::info("-------------- RecordRoute --------------");
  guide.RecordRoute({rg_utils::GetRandomPoint(feature_list_), rg_utils::GetRandomPoint(feature_list_),
                     rg_utils::GetRandomPoint(feature_list_)});
  spdlog::info("-------------- RouteChat --------------");
  // The second note reuses the first note's location so the server echoes it back.
  guide.RouteChat({rg_utils::MakeRouteNote("First message", 0, 0),
                   rg_utils::MakeRouteNote("Second message", 0, 0),
                   rg_utils::MakeRouteNote("Third message", 10000000, 0)});
  EventLoop::Run();  // Scheduler component: Continuously processes queued events on main application thread
  spdlog::info("-------------- LEAVING APPLICATION --------------");
  return 0;
}
