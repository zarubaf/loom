# SPDX-License-Identifier: Apache-2.0
# scan_insert Yosys pass
# Inserts scan chains into the design for state capture/restore

add_library(scan_insert MODULE scan_insert.cc)
# Don't link against libyosys - symbols resolved at runtime from yosys executable
target_include_directories(scan_insert PRIVATE ${YOSYS_INCLUDE})
target_compile_definitions(scan_insert PRIVATE
    _YOSYS_
    YOSYS_ENABLE_PLUGINS
    YOSYS_ENABLE_GLOB
    YOSYS_ENABLE_ZLIB
)
target_link_libraries(scan_insert PRIVATE loom_proto)
# On macOS, use -undefined dynamic_lookup to allow unresolved symbols
if(APPLE)
    set_target_properties(scan_insert PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        LINK_FLAGS "-undefined dynamic_lookup"
    )
else()
    set_target_properties(scan_insert PROPERTIES
        PREFIX ""
        SUFFIX ".so"
    )
endif()
add_dependencies(scan_insert yosys_ext)
