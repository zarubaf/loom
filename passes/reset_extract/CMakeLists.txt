# SPDX-License-Identifier: Apache-2.0
# reset_extract Yosys pass
# Extracts reset values and strips async resets for scan-based init

add_library(reset_extract MODULE reset_extract.cc)
# Don't link against libyosys - symbols resolved at runtime from yosys executable
target_include_directories(reset_extract PRIVATE ${YOSYS_INCLUDE})
target_compile_definitions(reset_extract PRIVATE
    _YOSYS_
    YOSYS_ENABLE_PLUGINS
    YOSYS_ENABLE_GLOB
    YOSYS_ENABLE_ZLIB
)
# On macOS, use -undefined dynamic_lookup to allow unresolved symbols
if(APPLE)
    set_target_properties(reset_extract PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        LINK_FLAGS "-undefined dynamic_lookup"
    )
else()
    set_target_properties(reset_extract PROPERTIES
        PREFIX ""
        SUFFIX ".so"
    )
endif()
add_dependencies(reset_extract yosys_ext)
