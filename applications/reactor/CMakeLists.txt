# SPDX-License-Identifier: Apache-2.0
# Copyright 2025 anderewrey

# Reactor pattern (Active Object) RouteGuide applications

# Client executable. One scenario per reactor variant, one source pair per scenario, so a variant
# added to one RPC method touches that RPC's files and this list, and nothing else.
add_executable(route_guide_active_reactor_client
    route_guide_active_reactor_client.cpp
    reactor_client.h
    reactor_client_routeguide.h
    reactor_eventloop.h
    latest_slot.h
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
    usecases/live_vehicle_position.h
    usecases/live_vehicle_position.cpp
    usecases/trip_recorder.h
    usecases/trip_recorder.cpp
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

# Tests subdirectory
add_subdirectory(tests)
