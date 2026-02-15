include(ExternalProject)

set(YOSYS_TAG "v0.48" CACHE STRING "Yosys version to build")

# Determine library extension based on platform
if(APPLE)
    set(YOSYS_LIB_EXT "dylib")
else()
    set(YOSYS_LIB_EXT "so")
endif()

ExternalProject_Add(yosys_ext
    GIT_REPOSITORY https://github.com/YosysHQ/yosys.git
    GIT_TAG        ${YOSYS_TAG}
    GIT_SHALLOW    TRUE
    PREFIX         ${CMAKE_BINARY_DIR}/yosys
    CONFIGURE_COMMAND ""
    BUILD_COMMAND   make -j${NPROC} PREFIX=<INSTALL_DIR> ENABLE_LIBYOSYS=1
    INSTALL_COMMAND make install PREFIX=<INSTALL_DIR> ENABLE_LIBYOSYS=1
    BUILD_IN_SOURCE TRUE
)

ExternalProject_Get_Property(yosys_ext INSTALL_DIR)
set(YOSYS_PREFIX   ${INSTALL_DIR} CACHE PATH "" FORCE)
set(YOSYS_INCLUDE  ${INSTALL_DIR}/share/yosys/include CACHE PATH "" FORCE)
set(YOSYS_DATDIR   ${INSTALL_DIR}/share/yosys CACHE PATH "" FORCE)
set(YOSYS_BIN      ${INSTALL_DIR}/bin/yosys CACHE FILEPATH "" FORCE)

# Create include directory so CMake doesn't complain at configure time
file(MAKE_DIRECTORY ${YOSYS_INCLUDE})

# Import libyosys as a target
add_library(libyosys SHARED IMPORTED GLOBAL)
set_target_properties(libyosys PROPERTIES
    IMPORTED_LOCATION ${INSTALL_DIR}/lib/libyosys.${YOSYS_LIB_EXT}
    INTERFACE_INCLUDE_DIRECTORIES ${YOSYS_INCLUDE}
)
add_dependencies(libyosys yosys_ext)
