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
    scenarios/support.h
    scenarios/support.cpp
    scenarios/get_feature.h
    scenarios/get_feature.cpp
    scenarios/list_features_continuous.h
    scenarios/list_features_continuous.cpp
    scenarios/list_features_turn_by_turn.h
    scenarios/list_features_turn_by_turn.cpp
    scenarios/record_route.h
    scenarios/record_route.cpp
    scenarios/route_chat_continuous.h
    scenarios/route_chat_continuous.cpp
    scenarios/route_chat_turn_by_turn.h
    scenarios/route_chat_turn_by_turn.cpp
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
