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
#include <set>
#include <string>
#include <optional>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: a prepared tour sends its scripted notes, stop by stop, and a briefing screen reads the
/// notes that come back from the stops. The script is ready before the tour starts. A stop answers a
/// new note with every note left there so far, so the same reply comes back again and again, and the
/// briefing shows each reply once.
///
/// The variant chosen is ActiveBidiReactor under Delivery::kPull, Submission::kReactorPulls and
/// ReadPacing::kPerMessage. kReactorPulls, since the script can always answer at once: the reactor pulls
/// each note as the previous write completes, and closes the request stream at the end of the script,
/// so the tour runs at the network's pace with no pump in the application. kPerMessage with kPull, since
/// the replies are consumed in place: read_ok declines a reply already seen at no cost, with no copy and
/// no event, and keeps a new one in the reactor, for the briefing to pull without a copy. kContinuous
/// would see every reply too, but could only copy the new ones out of the reaction.
///
/// The reference server keeps one note history for the whole service, not one per RPC, so the tour also
/// receives the notes other RPCs left at its stops, such as a convoy's at the stone bridge from an
/// earlier run against the same server, and its transcript varies with what ran before.
///
/// Clone of route_chat_pull_per_message_producer.h, along the use only: the reactor is the same, and the
/// diff is the decision, which here follows from the replies already seen rather than from an arbitrary
/// rule.
class GuidedTourBriefing {
  // The reactor flavour this scenario uses: its Delivery approach, then its Submission. Its ReadPacing
  // is the default, ReadPacing::kPerMessage.
  using Delivery = routeguide::RouteChat::Delivery;
  using Submission = routeguide::RouteChat::Submission;
  using Callbacks = routeguide::RouteChat::Callbacks<Delivery::kPull, Submission::kReactorPulls>;
  using ClientReactor = routeguide::RouteChat::ClientReactor<Delivery::kPull, Submission::kReactorPulls>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit GuidedTourBriefing(routeguide::RouteGuide::Stub& stub);

  GuidedTourBriefing(const GuidedTourBriefing&) = delete;
  GuidedTourBriefing& operator=(const GuidedTourBriefing&) = delete;
  GuidedTourBriefing(GuidedTourBriefing&&) = delete;
  GuidedTourBriefing& operator=(GuidedTourBriefing&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~GuidedTourBriefing();

  /// Proxy component: creates the Method Request, which pulls the first note before it returns.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param notes to stream to the server, pulled one at a time by the reactor
  void Start(std::vector<routeguide::RouteNote> notes);

 private:
  /// Application-thread handler for OnReadDoneOk.
  /// Runs for kept notes only. The message is still in the reactor, and this handler pulls it with
  /// GetResponse(), which also re-arms the read side. The write side is unaffected by that hold.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnWriteDone, which only logs: the reactor pulls the next note.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which reports the replies shown and declined, and destroys
  /// the reactor.
  void OnDone(const EventLoop::Event* event);

  /// The next_request callback's body: the next queued note, or std::nullopt once the queue is empty, which
  /// closes the client's write side; the server may keep sending responses afterward.
  std::optional<routeguide::RouteNote> NextNote();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // The queue belongs to this object, and to the reactor while the RPC runs: Start() fills it before
  // creating the reactor, and only next_request reads it afterward. The reactor pulls once at a time,
  // first on the application thread, then on gRPC threads, each pull after the write before it
  // completed, so the pulls never overlap.
  std::vector<routeguide::RouteNote> pending_;
  unsigned pulled_seq_{0};

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared. Both count kept notes only.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  // Declined notes: counted by read_ok on the gRPC thread, and read by OnDone on the application
  // thread, so it is shared, and atomic rather than locked.
  std::atomic<unsigned> skipped_seq_{0};

  // The replies seen so far, keyed by their text form. Only read_ok touches it, before its hand-off, and
  // the next read_ok can only start after a read armed later, so the reactions never share it at once.
  std::set<std::string> seen_;

  spdlog::logger& logger_{VariantLogger("GuidedTourBriefing")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
