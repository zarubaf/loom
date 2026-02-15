include(ExternalProject)

# yosys-slang builds as a cmake project using our yosys-config
set(YOSYS_SLANG_SOURCE ${CMAKE_SOURCE_DIR}/third_party/yosys-slang)
set(YOSYS_SLANG_BINARY ${CMAKE_BINARY_DIR}/yosys-slang)

# yosys-slang requires C++20
ExternalProject_Add(yosys_slang_ext
    SOURCE_DIR         ${YOSYS_SLANG_SOURCE}
    BINARY_DIR         ${YOSYS_SLANG_BINARY}
    CMAKE_ARGS
        -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
        -DCMAKE_CXX_STANDARD=20
        -DYOSYS_CONFIG=${YOSYS_PREFIX}/bin/yosys-config
        -DBUILD_AS_PLUGIN=ON
    INSTALL_COMMAND    ""  # No install step needed
    BUILD_ALWAYS       FALSE
    DEPENDS            yosys_ext
)

# Output paths
set(YOSYS_SLANG_PLUGIN ${YOSYS_SLANG_BINARY}/slang.so CACHE FILEPATH "" FORCE)
