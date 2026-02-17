# SPDX-License-Identifier: Apache-2.0
# loom_instrument pass - DPI bridge + flop enable instrumentation

add_library(loom_instrument MODULE loom_instrument.cc)

target_include_directories(loom_instrument PRIVATE ${YOSYS_INCLUDE})

target_compile_definitions(loom_instrument PRIVATE
    _YOSYS_
    YOSYS_ENABLE_PLUGINS
    YOSYS_ENABLE_GLOB
    YOSYS_ENABLE_ZLIB
)

# Don't link against libyosys directly - symbols resolve at runtime
if(APPLE)
    set_target_properties(loom_instrument PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        LINK_FLAGS "-undefined dynamic_lookup"
    )
else()
    set_target_properties(loom_instrument PROPERTIES
        PREFIX ""
    )
endif()

add_dependencies(loom_instrument yosys_ext)
