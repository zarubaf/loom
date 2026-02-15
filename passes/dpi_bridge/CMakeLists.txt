# SPDX-License-Identifier: Apache-2.0
# DPI Bridge pass - converts DPI placeholders to hardware bridges

add_library(dpi_bridge MODULE dpi_bridge.cc)

target_include_directories(dpi_bridge PRIVATE ${YOSYS_INCLUDE})

target_compile_definitions(dpi_bridge PRIVATE
    _YOSYS_
    YOSYS_ENABLE_PLUGINS
    YOSYS_ENABLE_GLOB
    YOSYS_ENABLE_ZLIB
)

# Don't link against libyosys directly - symbols resolve at runtime
if(APPLE)
    set_target_properties(dpi_bridge PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        LINK_FLAGS "-undefined dynamic_lookup"
    )
else()
    set_target_properties(dpi_bridge PROPERTIES
        PREFIX ""
    )
endif()

add_dependencies(dpi_bridge yosys_ext)
