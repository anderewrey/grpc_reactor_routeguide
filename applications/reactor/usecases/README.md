# Use cases

Each file pair in this directory is one use case: a class that owns one RPC to serve a real-life situation, with the
variant that situation calls for, started by the demo client, `route_guide_active_reactor_client`. The variants
themselves, one file each, are in [variants/](/applications/reactor/variants/), whose
[README](/applications/reactor/variants/README.md) also lists every scenario, variants and use cases, with its
parameters.

## Situations and the variant each calls for

A planned use case does not exist yet: a follow-up adds it.

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
|                         | time, the next once the driver confirms    | the application                            |
|                         | the current one                            |                                            |
| `gpx_track_upload`      | Uploading a route recorded earlier, from a | The whole source is ready: the reactor     |
|                         | GPX file, to get its summary               | pulls each point and closes on its own     |
| `live_route_recording`  | Recording a route while driving, each      | Points appear over time, on the            |
|                         | point sent as the GPS produces it          | application's thread                       |
| `convoy_chat`           | Drivers in a convoy exchange notes tied to | Notes are written whenever people type     |
|                         | their positions                            | them                                       |
| `guided_tour_briefing`  | A prepared tour sends its scripted notes   | The script is ready, so the reactor paces  |
|                         | stop by stop, and reads the replies        | it to the network                          |
| `whats_here`            | Tapping the map to ask what is at that     | One question, one answer: either delivery  |
|                         | point                                      | fits                                       |

## What the existing use cases add

Each existing use case is a clone of its variant along the use only, so a `diff` shows what the application adds:

```bash
cd applications/reactor
diff variants/list_features_pull_continuous.cpp usecases/live_vehicle_position.cpp  # keeps only the newest fix
diff variants/list_features_push_continuous.cpp usecases/trip_recorder.cpp          # reports the backlog it accepts
```

Both are described in the reference, under
[Keeping only the newest message](/applications/reactor/reactor_client.md#keeping-only-the-newest-message) and
[Handling every message without slowing the stream](/applications/reactor/reactor_client.md#handling-every-message-without-slowing-the-stream),
and the walkthrough runs both in its part for the stream reader.
