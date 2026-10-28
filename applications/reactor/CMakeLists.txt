# SPDX-License-Identifier: Apache-2.0
# Copyright 2025 anderewrey

# Reactor pattern (Active Object) RouteGuide applications

# Variants executable. One scenario per reactor variant, one source pair per scenario, so a variant
# added to one RPC method touches that RPC's files and this list, and nothing else.
add_executable(route_guide_active_reactor_client
    route_guide_active_reactor_client.cpp
    reactor_client.h
    reactor_client_routeguide.h
    reactor_eventloop.h
    demo_support.h
    demo_support.cpp
    variants/get_feature_pull.h
    variants/get_feature_pull.cpp
    variants/get_feature_push.h
    variants/get_feature_push.cpp
    variants/list_features_pull_continuous.h
    variants/list_features_pull_continuous.cpp
    variants/list_features_pull_turn_by_turn.h
    variants/list_features_pull_turn_by_turn.cpp
    variants/list_features_pull_per_message.h
    variants/list_features_pull_per_message.cpp
    variants/list_features_push_continuous.h
    variants/list_features_push_continuous.cpp
    variants/list_features_push_turn_by_turn.h
    variants/list_features_push_turn_by_turn.cpp
    variants/list_features_push_per_message.h
    variants/list_features_push_per_message.cpp
    variants/record_route_pull.h
    variants/record_route_pull.cpp
    variants/record_route_push.h
    variants/record_route_push.cpp
    variants/record_route_pull_producer.h
    variants/record_route_pull_producer.cpp
    variants/route_chat_pull_continuous.h
    variants/route_chat_pull_continuous.cpp
    variants/route_chat_pull_per_message.h
    variants/route_chat_pull_per_message.cpp
    variants/route_chat_pull_per_message_producer.h
    variants/route_chat_pull_per_message_producer.cpp
    variants/route_chat_pull_turn_by_turn.h
    variants/route_chat_pull_turn_by_turn.cpp
    variants/route_chat_push_continuous.h
    variants/route_chat_push_continuous.cpp
    variants/route_chat_push_per_message.h
    variants/route_chat_push_per_message.cpp
    variants/route_chat_push_turn_by_turn.h
    variants/route_chat_push_turn_by_turn.cpp
)

target_include_directories(route_guide_active_reactor_client
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_active_reactor_client
    PRIVATE
        rg_service
        EventLoop::EventLoop
)

# Use case executable. One use case per real-life situation, one source pair per use case, each a
# clone of the variant it uses, along the use only.
add_executable(route_guide_reactor_usecases
    route_guide_reactor_usecases.cpp
    reactor_client.h
    reactor_client_routeguide.h
    reactor_eventloop.h
    latest_slot.h
    demo_support.h
    demo_support.cpp
    usecases/convoy_chat.h
    usecases/convoy_chat.cpp
    usecases/gpx_track_upload.h
    usecases/gpx_track_upload.cpp
    usecases/guided_tour_briefing.h
    usecases/guided_tour_briefing.cpp
    usecases/live_route_recording.h
    usecases/live_route_recording.cpp
    usecases/live_vehicle_position.h
    usecases/live_vehicle_position.cpp
    usecases/offline_map_download.h
    usecases/offline_map_download.cpp
    usecases/place_search.h
    usecases/place_search.cpp
    usecases/trip_recorder.h
    usecases/trip_recorder.cpp
    usecases/turn_by_turn_guidance.h
    usecases/turn_by_turn_guidance.cpp
    usecases/unnamed_places_audit.h
    usecases/unnamed_places_audit.cpp
    usecases/whats_here.h
    usecases/whats_here.cpp
)

target_include_directories(route_guide_reactor_usecases
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_reactor_usecases
    PRIVATE
        rg_service
        EventLoop::EventLoop
)

# Coroutine twin of the variants executable: one scenario per awaitable of the coroutine level, each
# next to the callback variant it wraps, under variants/ with a _coro suffix.
add_executable(route_guide_active_reactor_client_coro
    route_guide_active_reactor_client_coro.cpp
    reactor_client.h
    reactor_client_routeguide.h
    coro_task.h
    reactor_coro.h
    coro_eventloop.h
    demo_support.h
    demo_support.cpp
    variants/get_feature_pull_coro.h
    variants/get_feature_pull_coro.cpp
)

target_include_directories(route_guide_active_reactor_client_coro
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_active_reactor_client_coro
    PRIVATE
        rg_service
        EventLoop::EventLoop
)

# Coroutine twin of the use case executable: one use case per callback use case that keeps its code on
# the application thread, under usecases/ with a _coro suffix.
add_executable(route_guide_reactor_usecases_coro
    route_guide_reactor_usecases_coro.cpp
    reactor_client.h
    reactor_client_routeguide.h
    coro_task.h
    reactor_coro.h
    coro_eventloop.h
    demo_support.h
    demo_support.cpp
    usecases/whats_here_coro.h
    usecases/whats_here_coro.cpp
)

target_include_directories(route_guide_reactor_usecases_coro
    PRIVATE
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(route_guide_reactor_usecases_coro
    PRIVATE
        rg_service
        EventLoop::EventLoop
)

# Tests subdirectory
add_subdirectory(tests)
