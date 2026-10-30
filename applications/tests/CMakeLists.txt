# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 anderewrey

# Smoke matrix: every client application against every server application, each run to its end on a fresh
# server, then one comparison of what the clients received. Adding a server, a client or a coroutine twin
# application is one line in a list below; a scenario added to an application needs nothing here.
set(SMOKE_SERVERS
    route_guide_sync_server
    route_guide_callback_server
)
set(SMOKE_CLIENTS
    route_guide_sync_client
    route_guide_callback_client
    route_guide_active_reactor_client
    route_guide_reactor_usecases
    route_guide_active_reactor_client_coro
    route_guide_reactor_usecases_coro
)
# Each coroutine twin application, and the callback application its scenarios are compared with.
set(SMOKE_TWINS
    route_guide_active_reactor_client_coro=route_guide_active_reactor_client
    route_guide_reactor_usecases_coro=route_guide_reactor_usecases
)

set(smoke_dir ${CMAKE_CURRENT_BINARY_DIR}/smoke_matrix)

# Every server binds, and every client dials, the fixed port 50051, hence the lock shared with
# callback_smoke_test. The cells are the fixture of the comparison, so running it alone runs them first.
foreach(server IN LISTS SMOKE_SERVERS)
  foreach(client IN LISTS SMOKE_CLIENTS)
    string(REGEX REPLACE "^route_guide_" "" client_name ${client})
    string(REGEX REPLACE "^route_guide_(.*)_server$" "\\1" server_name ${server})
    set(test_name smoke_matrix.${client_name}.${server_name})
    add_test(NAME ${test_name}
        COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/smoke_matrix_cell.sh
            $<TARGET_FILE:${server}> $<TARGET_FILE:${client}> ${smoke_dir}/${client}@${server}.digest
    )
    set_tests_properties(${test_name} PROPERTIES
        TIMEOUT 300
        RESOURCE_LOCK routeguide_port_50051
        FIXTURES_SETUP smoke_matrix
    )
  endforeach()
endforeach()

# The script takes each list as one argument, separated by spaces.
string(JOIN " " smoke_servers ${SMOKE_SERVERS})
string(JOIN " " smoke_clients ${SMOKE_CLIENTS})
string(JOIN " " smoke_twins ${SMOKE_TWINS})
add_test(NAME smoke_matrix.compare
    COMMAND sh ${CMAKE_CURRENT_SOURCE_DIR}/smoke_matrix_compare.sh
        ${smoke_dir} "${smoke_servers}" "${smoke_clients}" "${smoke_twins}"
)
set_tests_properties(smoke_matrix.compare PROPERTIES
    FIXTURES_REQUIRED smoke_matrix
)
