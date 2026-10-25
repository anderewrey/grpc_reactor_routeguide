# Use cases

Each file pair in this directory is one use case: a class that owns one RPC to serve a real-life situation, with the
variant that situation calls for, started by its own application, `route_guide_reactor_usecases`. The variants
themselves, one file each, are in [variants/](/applications/reactor/variants/), run by the demo client,
`route_guide_active_reactor_client`, and their [README](/applications/reactor/variants/README.md) also lists every
scenario, variants and use cases, with its parameters.

## Situations and the variant each calls for

| Scenario                | Real-life situation                        | Why this variant                           |
|-------------------------|--------------------------------------------|--------------------------------------------|
| `live_vehicle_position` | A map draws a vehicle's position from a    | Only the newest fix matters: a             |
|                         | live feed of fixes                         | `LatestSlot` keeps it, and the feed never  |
|                         |                                            | waits for the map                          |
| `trip_recorder`         | A vehicle's black box stores every         | Every report matters, and the feed never   |
|                         | position report of a trip                  | waits for storage; the backlog has no      |
|                         |                                            | bound                                      |
| `unnamed_places_audit`  | A map-quality tool counts the places with  | Each place is counted and forgotten:       |
|                         | no name in a region                        | nothing is kept                            |
| `place_search`          | A search shows only the named places of    | An unwanted place is skipped at no cost,   |
|                         | the visible area                           | and a kept one is taken without a copy     |
| `offline_map_download`  | Saving a region's places for offline use,  | Full speed until the backlog passes a      |
|                         | pausing when the disk writer falls behind  | threshold, then the download waits         |
| `turn_by_turn_guidance` | A guidance screen shows one stop at a      | Exactly one message in flight, released by |
|                         | time, the next once the driver confirms    | the application. `kPush` is required: the  |
|                         | the current one                            | screen owns the stop until the driver      |
|                         |                                            | confirms, while `kPull`'s `GetResponse()`  |
|                         |                                            | would arm the next read as it takes it     |
| `gpx_track_upload`      | Uploading a route recorded earlier, from a | The whole source is ready: the reactor     |
|                         | GPX file, to get its summary               | pulls each point and closes on its own.    |
|                         |                                            | `kPull`: the screen reads the summary once |
| `live_route_recording`  | Recording a route while driving, each      | Points appear over time, on the            |
|                         | point sent as the GPS produces it          | application's thread. `kPush`: the summary |
|                         |                                            | is handed to the trip history, which keeps |
|                         |                                            | it                                         |
| `convoy_chat`           | Drivers in a convoy exchange notes tied to | Notes are written whenever people type     |
|                         | their positions                            | them. `kPush` with `kContinuous`: every    |
|                         |                                            | note is shown, and the chat never stalls   |
| `guided_tour_briefing`  | A prepared tour sends its scripted notes   | The script is ready, so the reactor paces  |
|                         | stop by stop, and reads the replies        | it to the network. `kPull` with            |
|                         |                                            | `kPerMessage`: a repeated reply is skipped |
|                         |                                            | in place, a new one pulled without a copy  |
| `whats_here`            | Tapping the map to ask what is at that     | One question, one answer: either delivery  |
|                         | point                                      | fits. `kPull`: the card reads the answer   |
|                         |                                            | once, where it arrives                     |

The source of each use case gives the full reason for its parameters. Each one that waits on time, for a GPS fix, a
driver's confirmation or a typed note, gets it from a timed `EventLoop` event on the application thread: nothing
sleeps to wait. The reference server keeps one `RouteChat` note history for the whole service, so `convoy_chat` and
`guided_tour_briefing` also receive the notes other RPCs left at their locations.

## What each use case adds

Each use case is a clone of its variant along the use only, so a `diff` shows what the application adds:

```bash
cd applications/reactor
diff variants/list_features_pull_continuous.cpp usecases/live_vehicle_position.cpp  # keeps only the newest fix
diff variants/list_features_push_continuous.cpp usecases/trip_recorder.cpp          # reports the backlog it accepts
diff variants/list_features_pull_continuous.cpp usecases/unnamed_places_audit.cpp   # counts the unnamed places
diff variants/list_features_pull_per_message.cpp usecases/place_search.cpp          # keeps the named places
diff variants/list_features_push_per_message.cpp usecases/offline_map_download.cpp  # holds once 3 places wait
diff variants/list_features_push_turn_by_turn.cpp usecases/turn_by_turn_guidance.cpp  # resumes on confirmation
diff variants/record_route_pull_producer.cpp usecases/gpx_track_upload.cpp          # checks the summary
diff variants/record_route_push.cpp usecases/live_route_recording.cpp               # sends what the GPS produces
diff variants/route_chat_push_continuous.cpp usecases/convoy_chat.cpp               # sends what people type
diff variants/route_chat_pull_per_message_producer.cpp usecases/guided_tour_briefing.cpp  # skips repeated replies
diff variants/get_feature_pull.cpp usecases/whats_here.cpp                          # fills the card
```

The first two are described in the reference, under
[Keeping only the newest message](/applications/reactor/reactor_client.md#keeping-only-the-newest-message) and
[Handling every message without slowing the stream](/applications/reactor/reactor_client.md#handling-every-message-without-slowing-the-stream),
and the walkthrough runs both in its part for the stream reader.
