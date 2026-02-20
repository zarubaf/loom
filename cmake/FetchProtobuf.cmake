# SPDX-License-Identifier: Apache-2.0
# Fetch and build protobuf (lite runtime only, static)
#
# We use v3.21.12 (last 3.x release) which has a self-contained CMake
# build without the abseil-cpp dependency that newer versions require.

include(FetchContent)

set(protobuf_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(protobuf_INSTALL OFF CACHE BOOL "" FORCE)
set(protobuf_BUILD_PROTOC_BINARIES ON CACHE BOOL "" FORCE)
set(protobuf_WITH_ZLIB OFF CACHE BOOL "" FORCE)

FetchContent_Declare(protobuf
    GIT_REPOSITORY https://github.com/protocolbuffers/protobuf.git
    GIT_TAG        v3.21.12
    GIT_SHALLOW    TRUE
    SOURCE_SUBDIR  cmake
)

FetchContent_MakeAvailable(protobuf)
