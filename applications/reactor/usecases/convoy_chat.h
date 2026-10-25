///
/// SPDX-License-Identifier: Apache-2.0
/// Copyright 2026 anderewrey
///

#pragma once

#include <Event.h>
#include <EventLoop.h>
#include <spdlog/logger.h>

#include <cstddef>
#include <memory>
#include <vector>

#include "applications/reactor/reactor_client_routeguide.h"
#include "applications/reactor/reactor_eventloop.h"
#include "applications/reactor/demo_support.h"

namespace rg_demo::usecase {

/// A use case: the drivers of a convoy exchange notes tied to their positions, such as a hazard on the
/// road. A note is sent when someone types it, so notes appear over time, and two drivers can type at the
/// same moment. Every note received must be shown, and the chat must never stall, whether or not anyone
/// is typing.
///
/// The variant chosen is ActiveBidiReactor under Delivery::kPush, Submission::kApplicationPushes and
/// ReadPacing::kContinuous. kApplicationPushes, since a note exists only once typed: the application
/// sends it then, and keeps a note typed while a write is in flight until its OnWriteDone. kContinuous,
/// since the read side must never wait for the screen, and kPush with it, since each note is then handed
/// out owned and queued whole, so every one is shown however busy the screen is. EventLoop timers stand
/// in for the drivers typing.
///
/// The reference server keeps one note history for the whole service, not one per RPC, and echoes the
/// notes already left at a location whenever a new note arrives there. So the convoy also receives the
/// notes other RPCs left where it writes, such as the guided tour's note at the stone bridge, which the
/// demo places where the convoy reports debris, and its transcript varies with what ran before.
///
/// Clone of route_chat_push_continuous.h, along the use only: the reactor is the same, and the diff is
/// the typing and the pump that follows it.
class ConvoyChat {
  // The reactor flavour this scenario uses: its Delivery approach, then its ReadPacing mode.
  using Delivery = routeguide::RouteChat::Delivery;
  using Submission = routeguide::RouteChat::Submission;
  using ReadPacing = routeguide::RouteChat::ReadPacing;
  using Callbacks =
      routeguide::RouteChat::Callbacks<Delivery::kPush, Submission::kApplicationPushes, ReadPacing::kContinuous>;
  using ClientReactor =
      routeguide::RouteChat::ClientReactor<Delivery::kPush, Submission::kApplicationPushes, ReadPacing::kContinuous>;

 public:
  /// Registers the event handlers this scenario needs, for its own lifetime.
  /// @param stub of the RouteGuide API, which must outlive this object
  explicit ConvoyChat(routeguide::RouteGuide::Stub& stub);

  ConvoyChat(const ConvoyChat&) = delete;
  ConvoyChat& operator=(const ConvoyChat&) = delete;
  ConvoyChat(ConvoyChat&&) = delete;
  ConvoyChat& operator=(ConvoyChat&&) = delete;

  /// Cancels an RPC still in flight, and destroys the reactor once its OnDone() has run, see TearDownReactor().
  ~ConvoyChat();

  /// Proxy component: creates the Method Request, lets the drivers type, then returns. The first note is
  /// sent once typed.
  /// Runs on the application thread. A second call while one RPC is in flight is refused.
  /// @param notes the drivers will type, in order
  void Start(std::vector<routeguide::RouteNote> notes);

 private:
  /// Application-thread handler for OnReadDoneOk, which shows the note.
  void OnReadOk(const EventLoop::Event* event);
  /// Application-thread handler for a typed note: queues it, and sends it at once when no write is in
  /// flight.
  void OnNoteTyped(const EventLoop::Event* event);
  /// Application-thread handler for OnReadDoneNOk.
  void OnReadNOk(const EventLoop::Event* event);
  /// Application-thread handler for OnWriteDone, which sends the next queued note, if any.
  void OnWriteDone(const EventLoop::Event* event);
  /// Application-thread handler for OnDone, which destroys the reactor.
  void OnDone(const EventLoop::Event* event);

  /// Sends the next queued note, unless a write is in flight or no note waits. The last note goes through
  /// SendLastRequest(), which closes the client's write side; the server may keep sending responses
  /// afterward.
  void SendNextNote();

  routeguide::RouteGuide::Stub& stub_;
  std::unique_ptr<ClientReactor> reactor_;

  // All of these belong to the application thread. The drivers take their next note from the script,
  // and the queue keeps the typed notes waiting for a write.
  std::vector<routeguide::RouteNote> script_;
  std::vector<routeguide::RouteNote> pending_;
  std::size_t typed_{0};
  bool write_in_flight_{false};

  // The read counter is incremented only on the gRPC thread, in read_ok, and the handled counter
  // only on the application thread, so neither is shared.
  unsigned read_seq_{0};
  unsigned handled_seq_{0};

  spdlog::logger& logger_{VariantLogger("ConvoyChat")};

  // Declared last, so destruction deregisters the handlers before anything they reach is gone.
  RpcReactor::EventConnection on_read_ok_;
  RpcReactor::EventConnection on_note_typed_;
  RpcReactor::EventConnection on_read_nok_;
  RpcReactor::EventConnection on_write_done_;
  RpcReactor::EventConnection on_done_;
};

}  // namespace rg_demo::usecase
