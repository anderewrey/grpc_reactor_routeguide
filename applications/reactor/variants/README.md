# Demo variants

Each file pair in this directory is one variant scenario: a class that owns one RPC and runs one variant. There are two
kinds of scenarios, one directory and one application each:

| Kind | Directory | Application | Named after |
|------|-----------|-------------|-------------|
| Variant | `variants/`, this one | `route_guide_active_reactor_client` | Its RPC and variant |
| Use case | [usecases/](/applications/reactor/usecases/) | `route_guide_reactor_usecases` | A real-life situation |

A variant's name is `<rpc>_<delivery>_<pacing>`, with `_producer` added under `kReactorPulls`. A use case's name is
its situation, such as `trip_recorder`.

A variant shows what one choice of parameters changes, against the neighbouring file that differs along that one
choice. A use case shows why a variant fits a real need, and is a clone of the variant it uses, along the use only. The
parameters themselves are described in
[Reactors and their variants](/applications/reactor/reactor_client.md#reactors-and-their-variants).

## Every scenario at a glance

The RPC sets the reactor: `GetFeature` runs `ActiveUnaryReactor`, `ListFeatures` runs `ActiveReadReactor`,
`RecordRoute` runs `ActiveWriteReactor`, and `RouteChat` runs `ActiveBidiReactor`. A dash marks a parameter the
reactor does not take.

| Scenario                               | Kind     | RPC          | Delivery | Submission           | ReadPacing    |
|----------------------------------------|----------|--------------|----------|----------------------|---------------|
| `get_feature_pull`                     | variant  | GetFeature   | `kPull`  | —                    | —             |
| `get_feature_push`                     | variant  | GetFeature   | `kPush`  | —                    | —             |
| `list_features_pull_continuous`        | variant  | ListFeatures | `kPull`  | —                    | `kContinuous` |
| `list_features_pull_per_message`       | variant  | ListFeatures | `kPull`  | —                    | `kPerMessage` |
| `list_features_pull_turn_by_turn`      | variant  | ListFeatures | `kPull`  | —                    | `kTurnByTurn` |
| `list_features_push_continuous`        | variant  | ListFeatures | `kPush`  | —                    | `kContinuous` |
| `list_features_push_per_message`       | variant  | ListFeatures | `kPush`  | —                    | `kPerMessage` |
| `list_features_push_turn_by_turn`      | variant  | ListFeatures | `kPush`  | —                    | `kTurnByTurn` |
| `record_route_pull`                    | variant  | RecordRoute  | `kPull`  | `kApplicationPushes` | —             |
| `record_route_push`                    | variant  | RecordRoute  | `kPush`  | `kApplicationPushes` | —             |
| `record_route_pull_producer`           | variant  | RecordRoute  | `kPull`  | `kReactorPulls`      | —             |
| `route_chat_pull_continuous`           | variant  | RouteChat    | `kPull`  | `kApplicationPushes` | `kContinuous` |
| `route_chat_pull_per_message`          | variant  | RouteChat    | `kPull`  | `kApplicationPushes` | `kPerMessage` |
| `route_chat_pull_turn_by_turn`         | variant  | RouteChat    | `kPull`  | `kApplicationPushes` | `kTurnByTurn` |
| `route_chat_push_continuous`           | variant  | RouteChat    | `kPush`  | `kApplicationPushes` | `kContinuous` |
| `route_chat_push_per_message`          | variant  | RouteChat    | `kPush`  | `kApplicationPushes` | `kPerMessage` |
| `route_chat_push_turn_by_turn`         | variant  | RouteChat    | `kPush`  | `kApplicationPushes` | `kTurnByTurn` |
| `route_chat_pull_per_message_producer` | variant  | RouteChat    | `kPull`  | `kReactorPulls`      | `kPerMessage` |
| `live_vehicle_position`                | use case | ListFeatures | `kPull`  | —                    | `kContinuous` |
| `trip_recorder`                        | use case | ListFeatures | `kPush`  | —                    | `kContinuous` |
| `unnamed_places_audit`                 | use case | ListFeatures | `kPull`  | —                    | `kContinuous` |
| `place_search`                         | use case | ListFeatures | `kPull`  | —                    | `kPerMessage` |
| `offline_map_download`                 | use case | ListFeatures | `kPush`  | —                    | `kPerMessage` |
| `turn_by_turn_guidance`                | use case | ListFeatures | `kPush`  | —                    | `kTurnByTurn` |
| `gpx_track_upload`                     | use case | RecordRoute  | `kPull`  | `kReactorPulls`      | —             |
| `live_route_recording`                 | use case | RecordRoute  | `kPush`  | `kApplicationPushes` | —             |
| `convoy_chat`                          | use case | RouteChat    | `kPush`  | `kApplicationPushes` | `kContinuous` |
| `guided_tour_briefing`                 | use case | RouteChat    | `kPull`  | `kReactorPulls`      | `kPerMessage` |
| `whats_here`                           | use case | GetFeature   | `kPull`  | —                    | —             |

The demo client runs the 18 variants at once, so 18 of the 24, and `route_guide_reactor_usecases` runs the use cases.
The six variants left out all use `kReactorPulls`, which combines with every `Delivery` and `ReadPacing` exactly as
`kApplicationPushes` does.

## What each variant does

Each one shows its variant and nothing else. Under `kPerMessage`, the per-message decision follows one arbitrary rule,
keeping every second message, the same under both deliveries; the use cases decide it for a reason. A file differs from
the one in its last column along one parameter only, so `diff` between the two shows exactly what that parameter
changes. A name starting with `...` abbreviates the same RPC's prefix.

| Scenario                               | Diff against                  |
|----------------------------------------|-------------------------------|
| `get_feature_pull`                     | `get_feature_push`            |
| `get_feature_push`                     | `get_feature_pull`            |
| `list_features_pull_continuous`        | `..._pull_turn_by_turn`       |
| `list_features_pull_per_message`       | `..._pull_turn_by_turn`       |
| `list_features_pull_turn_by_turn`      | `..._push_turn_by_turn`       |
| `list_features_push_continuous`        | `..._push_turn_by_turn`       |
| `list_features_push_per_message`       | `..._push_turn_by_turn`       |
| `list_features_push_turn_by_turn`      | `..._pull_turn_by_turn`       |
| `record_route_pull`                    | `record_route_push`           |
| `record_route_push`                    | `record_route_pull`           |
| `record_route_pull_producer`           | `record_route_pull`           |
| `route_chat_<delivery>_<pacing>`       | its `list_features_` namesake |
| `route_chat_pull_per_message_producer` | `..._pull_per_message`        |

What each one does with each message:

- `get_feature_pull`: the done handler pulls the answer out of the reactor with `GetResponse()`.
- `get_feature_push`: the done callback hands the answer out owned, and the handler reclaims it.
- `list_features_pull_continuous`: `read_ok` logs and counts each message inside the reaction. The done handler
  reports the count.
- `list_features_pull_per_message`: `read_ok` keeps every second message, for the handler to pull, and skips the
  others.
- `list_features_pull_turn_by_turn`: every feature is held until the handler pulls it, so one is in flight at a time.
- `list_features_push_continuous`: every feature is queued owned, and the reactor reads on at once.
- `list_features_push_per_message`: every message is queued owned, and `read_ok` holds every second one until
  `ResumeRead()`.
- `list_features_push_turn_by_turn`: every feature is queued owned, and held until the handler calls `ResumeRead()`.
- `record_route_pull`: the application sends each point, the last one with `SendLastRequest()`, then pulls the
  summary.
- `record_route_push`: the same, with the summary handed out owned.
- `record_route_pull_producer`: the reactor pulls each point from a prepared list, and closes the request stream
  itself.
- `route_chat_<delivery>_<pacing>`: six files, the same note pump in each. Each read side behaves as its
  `ListFeatures` namesake, with the same rule under `kPerMessage`.
- `route_chat_pull_per_message_producer`: the same read side, with the reactor pulling each note.

The situation behind each use case, and why its variant fits, are in the
[use cases' README](/applications/reactor/usecases/README.md).
