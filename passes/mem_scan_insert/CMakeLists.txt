# SPDX-License-Identifier: Apache-2.0
# mem_scan_insert Yosys pass
# Inserts memory scan interface for SRAM state capture

add_library(mem_scan_insert MODULE mem_scan_insert.cc)
# Don't link against libyosys - symbols resolved at runtime from yosys executable
target_include_directories(mem_scan_insert PRIVATE ${YOSYS_INCLUDE})
target_compile_definitions(mem_scan_insert PRIVATE
    _YOSYS_
    YOSYS_ENABLE_PLUGINS
    YOSYS_ENABLE_GLOB
    YOSYS_ENABLE_ZLIB
)
# On macOS, use -undefined dynamic_lookup to allow unresolved symbols
if(APPLE)
    set_target_properties(mem_scan_insert PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        LINK_FLAGS "-undefined dynamic_lookup"
    )
else()
    set_target_properties(mem_scan_insert PROPERTIES
        PREFIX ""
        SUFFIX ".so"
    )
endif()
add_dependencies(mem_scan_insert yosys_ext)
