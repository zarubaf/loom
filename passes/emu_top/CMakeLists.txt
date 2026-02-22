# SPDX-License-Identifier: Apache-2.0
# emu_top Yosys pass
# Generates top-level wrapper with clock gating and host interface

add_library(emu_top MODULE emu_top.cc)
# Don't link against libyosys - symbols resolved at runtime from yosys executable
target_include_directories(emu_top PRIVATE
    ${YOSYS_INCLUDE}
    ${CMAKE_SOURCE_DIR}/third_party/picosha2
)
target_compile_definitions(emu_top PRIVATE
    _YOSYS_
    YOSYS_ENABLE_PLUGINS
    YOSYS_ENABLE_GLOB
    YOSYS_ENABLE_ZLIB
)
# On macOS, use -undefined dynamic_lookup to allow unresolved symbols
if(APPLE)
    set_target_properties(emu_top PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        LINK_FLAGS "-undefined dynamic_lookup"
    )
else()
    set_target_properties(emu_top PROPERTIES
        PREFIX ""
        SUFFIX ".so"
    )
endif()
add_dependencies(emu_top yosys_ext)
