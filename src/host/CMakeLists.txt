# SPDX-License-Identifier: Apache-2.0
# Build libloom_host static library and loom_sim_main object library

# --- Static library: all host sources except main ---
add_library(loom_host STATIC
    loom.cpp
    loom_transport_socket.cpp
    loom_transport_xdma.cpp
    ${CMAKE_SOURCE_DIR}/src/dpi/loom_dpi_service.cpp
    loom_vpi.cpp
    loom_shell.cpp
)

target_include_directories(loom_host PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/util
    ${CMAKE_SOURCE_DIR}/src/include
    ${CMAKE_SOURCE_DIR}/src/dpi
)
target_compile_features(loom_host PUBLIC cxx_std_20)
target_link_libraries(loom_host PUBLIC replxx::replxx loom_proto)

# --- Object library: main entry point (linked by e2e tests with user DPI code) ---
add_library(loom_sim_main OBJECT loom_sim_main.cpp)
target_include_directories(loom_sim_main PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(loom_sim_main PUBLIC cxx_std_20)
target_link_libraries(loom_sim_main PUBLIC loom_host)
