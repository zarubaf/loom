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
# WHOLE_ARCHIVE ensures all symbols from loom_host (including loom_vpi.cpp's
# vpi_control/vpi_printf) are included in the executable, even though loomx
# doesn't reference them directly. User DPI libraries loaded via dlopen
# resolve these symbols at runtime.
add_executable(loomx loomx.cpp)
target_include_directories(loomx PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_SOURCE_DIR}/src/util
)
target_link_libraries(loomx PRIVATE
    "$<LINK_LIBRARY:WHOLE_ARCHIVE,loom_host>"
    ${CMAKE_DL_LIBS}
)
target_compile_features(loomx PRIVATE cxx_std_20)
