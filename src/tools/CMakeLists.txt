# SPDX-License-Identifier: Apache-2.0
# Build loomc (compilation driver) and loomx (execution host)

# loomc — runs Yosys subprocess + compiles dispatch .so
add_executable(loomc loomc.cpp)
target_include_directories(loomc PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/util
    ${CMAKE_SOURCE_DIR}/third_party/picosha2
)
target_compile_features(loomc PRIVATE cxx_std_20)

# loomx — dlopen-based execution host
add_executable(loomx loomx.cpp)
target_include_directories(loomx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/util
)
target_link_libraries(loomx PRIVATE loom_host ${CMAKE_DL_LIBS})
target_compile_features(loomx PRIVATE cxx_std_20)
