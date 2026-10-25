# Demo scenarios

Each file pair in this directory is one scenario: a class that owns one RPC, run by the demo client,
`route_guide_active_reactor_client`. A scenario is one of two kinds:

| Kind    | Named after                                       | Shows                                                |
|---------|---------------------------------------------------|------------------------------------------------------|
| Variant | Its RPC and variant: `<rpc>_<delivery>_<pacing>`, | What one choice of parameters changes, against the   |
|         | and `_producer` under `Submission::kReactorPulls` | neighbouring file that differs along that one choice |
| Usage   | A real-life situation, such as `trip_recorder`    | Why a variant fits a real need                       |

The variants are the reference for each parameter, and the usages put them in context. Each usage scenario is a clone
of the variant it uses, along the use only, so a `diff` against that variant shows what the application adds. The
parameters themselves are described in
[Reactors and their variants](/applications/reactor/reactor_client.md#reactors-and-their-variants).

## Every scenario at a glance

The RPC sets the reactor: `GetFeature` runs `ActiveUnaryReactor`, `ListFeatures` runs `ActiveReadReactor`,
`RecordRoute` runs `ActiveWriteReactor`, and `RouteChat` runs `ActiveBidiReactor`. A dash marks a parameter the
reactor does not take. A planned scenario does not exist yet: a follow-up adds it, and decides what "either" and
"to choose" leave open, since that use does not depend on them.

| Scenario                               | Kind    | RPC          | Delivery | Submission           | ReadPacing    |
|----------------------------------------|---------|--------------|----------|----------------------|---------------|
| `get_feature_pull`                     | variant | GetFeature   | `kPull`  | —                    | —             |
| `get_feature_push`                     | variant | GetFeature   | `kPush`  | —                    | —             |
| `list_features_pull_continuous`        | variant | ListFeatures | `kPull`  | —                    | `kContinuous` |
| `list_features_pull_per_message`       | variant | ListFeatures | `kPull`  | —                    | `kPerMessage` |
| `list_features_pull_turn_by_turn`      | variant | ListFeatures | `kPull`  | —                    | `kTurnByTurn` |
| `list_features_push_continuous`        | variant | ListFeatures | `kPush`  | —                    | `kContinuous` |
| `list_features_push_per_message`       | variant | ListFeatures | `kPush`  | —                    | `kPerMessage` |
| `list_features_push_turn_by_turn`      | variant | ListFeatures | `kPush`  | —                    | `kTurnByTurn` |
| `record_route_pull`                    | variant | RecordRoute  | `kPull`  | `kApplicationPushes` | —             |
| `record_route_push`                    | variant | RecordRoute  | `kPush`  | `kApplicationPushes` | —             |
| `record_route_pull_producer`           | variant | RecordRoute  | `kPull`  | `kReactorPulls`      | —             |
| `route_chat_pull_continuous`           | variant | RouteChat    | `kPull`  | `kApplicationPushes` | `kContinuous` |
| `route_chat_pull_per_message`          | variant | RouteChat    | `kPull`  | `kApplicationPushes` | `kPerMessage` |
| `route_chat_pull_turn_by_turn`         | variant | RouteChat    | `kPull`  | `kApplicationPushes` | `kTurnByTurn` |
| `route_chat_push_continuous`           | variant | RouteChat    | `kPush`  | `kApplicationPushes` | `kContinuous` |
| `route_chat_push_per_message`          | variant | RouteChat    | `kPush`  | `kApplicationPushes` | `kPerMessage` |
| `route_chat_push_turn_by_turn`         | variant | RouteChat    | `kPush`  | `kApplicationPushes` | `kTurnByTurn` |
| `route_chat_pull_per_message_producer` | variant | RouteChat    | `kPull`  | `kReactorPulls`      | `kPerMessage` |
| `live_vehicle_position`                | usage   | ListFeatures | `kPull`  | —                    | `kContinuous` |
| `trip_recorder`                        | usage   | ListFeatures | `kPush`  | —                    | `kContinuous` |
| `unnamed_places_audit`                 | planned | ListFeatures | `kPull`  | —                    | `kContinuous` |
| `place_search`                         | planned | ListFeatures | `kPull`  | —                    | `kPerMessage` |
| `offline_map_download`                 | planned | ListFeatures | `kPush`  | —                    | `kPerMessage` |
| `turn_by_turn_guidance`                | planned | ListFeatures | either   | —                    | `kTurnByTurn` |
| `gpx_track_upload`                     | planned | RecordRoute  | either   | `kReactorPulls`      | —             |
| `live_route_recording`                 | planned | RecordRoute  | either   | `kApplicationPushes` | —             |
| `convoy_chat`                          | planned | RouteChat    | either   | `kApplicationPushes` | to choose     |
| `guided_tour_briefing`                 | planned | RouteChat    | either   | `kReactorPulls`      | to choose     |
| `whats_here`                           | planned | GetFeature   | either   | —                    | —             |

The demo runs the 18 variant scenarios and the 2 usage scenarios at once, so 18 of the 24 variants. The six variants
left out all use `kReactorPulls`, which combines with every `Delivery` and `ReadPacing` exactly as
`kApplicationPushes` does.

## Variant scenarios

Each one shows its variant, and a few apply it to a small use so that a per-message decision has something to decide:
a filter under `kPull` with `kPerMessage`, backpressure under `kPush` with `kPerMessage`. A file differs from the one
in its last column along one parameter only, so `diff` between the two shows exactly what that parameter changes. A
name starting with `...` abbreviates the same RPC's prefix.

| Scenario                               | What it does with each message                 | Diff against            |
|----------------------------------------|------------------------------------------------|-------------------------|
| `get_feature_pull`                     | The done handler pulls the answer out of the   | `get_feature_push`      |
|                                        | reactor with `GetResponse()`                   |                         |
| `get_feature_push`                     | The done callback hands the answer out owned,  | `get_feature_pull`      |
|                                        | and the handler reclaims it                    |                         |
| `list_features_pull_continuous`        | `read_ok` counts named and unnamed features    | `..._pull_turn_by_turn` |
|                                        | inside the reaction; the done handler reports  |                         |
|                                        | the totals                                     |                         |
| `list_features_pull_per_message`       | A filter: `read_ok` keeps a named feature, for | `..._pull_turn_by_turn` |
|                                        | the handler to pull, and skips an unnamed one  |                         |
| `list_features_pull_turn_by_turn`      | Every feature is held until the handler pulls  | `..._push_turn_by_turn` |
|                                        | it, so one is in flight at a time              |                         |
| `list_features_push_continuous`        | Every feature is queued owned, and the reactor | `..._push_turn_by_turn` |
|                                        | reads on at once                               |                         |
| `list_features_push_per_message`       | Backpressure: `read_ok` holds the stream once  | `..._push_turn_by_turn` |
|                                        | 3 features wait for the handler                |                         |
| `list_features_push_turn_by_turn`      | Every feature is queued owned, and held until  | `..._pull_turn_by_turn` |
|                                        | the handler calls `ResumeRead()`               |                         |
| `record_route_pull`                    | The application sends each point, the last one | `record_route_push`     |
|                                        | with `SendLastRequest()`, then pulls the       |                         |
|                                        | summary                                        |                         |
| `record_route_push`                    | The same, with the summary handed out owned    | `record_route_pull`     |
| `record_route_pull_producer`           | The reactor pulls each point from a prepared   | `record_route_pull`     |
|                                        | list, and closes the request stream itself     |                         |
| `route_chat_<delivery>_<pacing>`       | Six files, the same note pump in each. Each    | its `list_features_`    |
|                                        | read side behaves as its `ListFeatures`        | namesake                |
|                                        | namesake, except the `kPull` filter skips the  |                         |
|                                        | server's repeated notes, and the `kPush` bound |                         |
|                                        | is 2 notes                                     |                         |
| `route_chat_pull_per_message_producer` | The same filter, with the reactor pulling each | `..._pull_per_message`  |
|                                        | note                                           |                         |

## Usage scenarios

Each one starts from a real-life situation, and uses the variant that situation calls for.

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

Each existing one is a clone of its variant scenario along the use only, so a `diff` shows what the application adds:

```bash
diff list_features_pull_continuous.cpp live_vehicle_position.cpp  # keeps only the newest fix
diff list_features_push_continuous.cpp trip_recorder.cpp          # reports the backlog it accepts
```

Both are described in the reference, under
[Keeping only the newest message](/applications/reactor/reactor_client.md#keeping-only-the-newest-message) and
[Handling every message without slowing the stream](/applications/reactor/reactor_client.md#handling-every-message-without-slowing-the-stream),
and the walkthrough runs both in its part for the stream reader.
