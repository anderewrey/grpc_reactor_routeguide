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

/// A use case: a map search lists the places of the visible area by name, so a place without a name has
/// nothing to show and is left out of the results. Most places are named, but a region holds many that
/// are not, and the search must not pay for them.
///
/// The variant chosen is ActiveReadReactor under Delivery::kPull and ReadPacing::kPerMessage. read_ok
/// decides for each place: an unnamed one is declined inside the callback, at no cost, since nothing is
/// copied and no event is queued, and the reactor reads on at once. A named one is kept in the reactor,
/// which holds the stream until the results list pulls it with GetResponse(), without a copy. ListFeatures
/// stands in for the search: the rectangle is the visible area.
///
/// Clone of list_features_pull_per_message.h, along the use only: the reactor is the same, and the diff
/// is the decision, which here follows from the place rather than from an arbitrary rule.
class PlaceSearch {
  // The reactor flavour this scenario uses: its Delivery approach only. Its ReadPacing is the
  // default, ReadPacing::kPerMessage.
  using Delivery = routeguide::ListFeatures::Delivery;
  using Callbacks = routeguide::ListFeatures::Callbacks<Delivery::kPull>;
  using ClientReactor = routeguide::ListFeatures::ClientReactor<Delivery::kPull>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit PlaceSearch(routeguide::RouteGuide::Stub& stub);

  PlaceSearch(const PlaceSearch&) = delete;
  PlaceSearch& operator=(const PlaceSearch&) = delete;
  PlaceSearch(PlaceSearch&&) = delete;
  PlaceSearch& operator=(PlaceSearch&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~PlaceSearch();

  /// Proxy component: creates the Method Request and returns immediately.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param rect bounding the features to list
  void Start(routeguide::Rectangle rect);

 private:
  /// Application-thread handler for OnReadDoneOk.
  /// Same as the turn-by-turn counterpart: only kept messages reach it, each one still in the reactor,
  /// and it pulls them with GetResponse(), which also arms the next read. No read is armed between a
  /// keeping callback and that call, so read_ok #k+1 cannot appear before the pulled #k line.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the places shown and left out, and destroys the
  /// reactor.
  void OnDone(const EventLoop::Event* event);

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;
  Activity activity_;

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared. They stay in step because the event queue
  // is FIFO and at most one OnReadDone reaction per RPC is in flight. Both count kept messages only.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  // Declined messages: incremented by read_ok on the gRPC thread, read by the OnDone handler on the
  // application thread, so it is shared, and atomic rather than locked.
  std::atomic<unsigned> skipped_seq_{0};

  spdlog::logger& logger_{VariantLogger("PlaceSearch")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
