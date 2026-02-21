# SPDX-License-Identifier: Apache-2.0
# mem_shadow Yosys pass
# Inserts shadow access ports on memories for debug read/write

add_library(mem_shadow MODULE mem_shadow.cc)
# Don't link against libyosys - symbols resolved at runtime from yosys executable
target_include_directories(mem_shadow PRIVATE ${YOSYS_INCLUDE})
target_compile_definitions(mem_shadow PRIVATE
    _YOSYS_
    YOSYS_ENABLE_PLUGINS
    YOSYS_ENABLE_GLOB
    YOSYS_ENABLE_ZLIB
)
target_link_libraries(mem_shadow PRIVATE loom_proto)
# On macOS, use -undefined dynamic_lookup to allow unresolved symbols
if(APPLE)
    set_target_properties(mem_shadow PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        LINK_FLAGS "-undefined dynamic_lookup"
    )
else()
    set_target_properties(mem_shadow PROPERTIES
        PREFIX ""
        SUFFIX ".so"
    )
endif()
add_dependencies(mem_shadow yosys_ext)
