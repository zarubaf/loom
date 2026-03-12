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
    URL      https://github.com/protocolbuffers/protobuf/archive/refs/tags/v3.21.12.tar.gz
    URL_HASH SHA256=930c2c3b5ecc6c9c12615cf5ad93f1cd6e12d0aba862b572e076259970ac3a53
    SOURCE_SUBDIR cmake
)

FetchContent_MakeAvailable(protobuf)
