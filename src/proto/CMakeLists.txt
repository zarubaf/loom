# SPDX-License-Identifier: Apache-2.0
# Protobuf-generated sources for loom scan map / snapshot

set(PROTO_SRC ${CMAKE_CURRENT_SOURCE_DIR}/loom_snapshot.proto)
set(PROTO_OUT ${CMAKE_CURRENT_BINARY_DIR}/loom_snapshot.pb.cc)
set(PROTO_HDR ${CMAKE_CURRENT_BINARY_DIR}/loom_snapshot.pb.h)

# Generate C++ from .proto using the protoc built by FetchContent
add_custom_command(
    OUTPUT ${PROTO_OUT} ${PROTO_HDR}
    COMMAND protobuf::protoc
        --proto_path=${CMAKE_CURRENT_SOURCE_DIR}
        --cpp_out=${CMAKE_CURRENT_BINARY_DIR}
        ${PROTO_SRC}
    DEPENDS ${PROTO_SRC} protobuf::protoc
    COMMENT "Running protoc on loom_snapshot.proto"
    VERBATIM
)

add_library(loom_proto OBJECT ${PROTO_OUT} ${PROTO_HDR})
target_include_directories(loom_proto PUBLIC ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(loom_proto PUBLIC libprotobuf-lite)
