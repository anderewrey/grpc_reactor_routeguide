///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <atomic>
#include <memory>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: a map-quality tool audits a region and reports how many of its places have no name, so
/// that editors know where to work. The report is a count: each place is looked at once, counted, and
/// forgotten, and none of them is needed after that.
///
/// The variant chosen is ActiveReadReactor under Delivery::kPull and ReadPacing::kContinuous. read_ok
/// counts each lent place inside the reaction, on the gRPC thread, and the reactor reads on at once, so
/// the audit runs at the server's pace, with no copy and no event per place. The application reads the
/// counts once the RPC is done. ListFeatures stands in for the audit: the rectangle is the region.
///
/// Clone of list_features_pull_continuous.h, along the use only: the reactor is the same, and the diff is
/// the aggregate the reaction builds.
class UnnamedPlacesAudit {
  // The reactor flavour this scenario uses: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::ListFeatures::Delivery;
  using ReadPacing = routeguide::ListFeatures::ReadPacing;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull, ReadPacing::kContinuous>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull, ReadPacing::kContinuous>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit UnnamedPlacesAudit(routeguide::RouteGuide::Stub& stub);

  UnnamedPlacesAudit(const UnnamedPlacesAudit&) = delete;
  UnnamedPlacesAudit& operator=(const UnnamedPlacesAudit&) = delete;
  UnnamedPlacesAudit(UnnamedPlacesAudit&&) = delete;
  UnnamedPlacesAudit& operator=(UnnamedPlacesAudit&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~UnnamedPlacesAudit();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(const routeguide::Rectangle& rect);

 private:
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the audit and destroys the reactor.
  /// Delta against the turn-by-turn counterpart: there is no handler for OnReadDoneOk, since no
  /// message is queued.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The aggregate. Incremented only on the gRPC thread, in read_ok, but read by the OnDone handler on
  // the application thread, so both are shared, and atomic rather than locked: the reaction must never
  // wait on the application.
  std::atomic<unsigned> read_seq_{0};
  std::atomic<unsigned> unnamed_count_{0};

  spdlog::logger& logger_{VariantLogger("UnnamedPlacesAudit")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
