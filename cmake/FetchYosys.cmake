include(ExternalProject)

set(YOSYS_TAG "v0.62" CACHE STRING "Yosys version to build")

# Determine brew prefix based on platform (Yosys uses .so on all platforms)
if(APPLE)
    # Homebrew paths (Apple Silicon vs Intel)
    if(EXISTS "/opt/homebrew")
        set(BREW_PREFIX "/opt/homebrew")
    else()
        set(BREW_PREFIX "/usr/local")
    endif()
    # Set environment for Homebrew dependencies (bison, libffi, readline)
    set(YOSYS_BUILD_ENV
        "PATH=${BREW_PREFIX}/opt/bison/bin:${BREW_PREFIX}/opt/flex/bin:$ENV{PATH}"
        "PKG_CONFIG_PATH=${BREW_PREFIX}/opt/libffi/lib/pkgconfig:${BREW_PREFIX}/opt/readline/lib/pkgconfig:$ENV{PKG_CONFIG_PATH}"
        "LDFLAGS=-L${BREW_PREFIX}/opt/readline/lib -L${BREW_PREFIX}/opt/libffi/lib"
        "CPPFLAGS=-I${BREW_PREFIX}/opt/readline/include -I${BREW_PREFIX}/opt/libffi/include"
    )
else()
    set(YOSYS_BUILD_ENV "")
endif()

ExternalProject_Add(yosys_ext
    GIT_REPOSITORY https://github.com/YosysHQ/yosys.git
    GIT_TAG        ${YOSYS_TAG}
    GIT_SHALLOW    TRUE
    PREFIX         ${CMAKE_BINARY_DIR}/yosys
    CONFIGURE_COMMAND ""
    BUILD_COMMAND   ${CMAKE_COMMAND} -E env ${YOSYS_BUILD_ENV}
        make -j${NPROC} PREFIX=<INSTALL_DIR> ENABLE_LIBYOSYS=1
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${YOSYS_BUILD_ENV}
        make install PREFIX=<INSTALL_DIR> ENABLE_LIBYOSYS=1
    BUILD_IN_SOURCE TRUE
)

ExternalProject_Get_Property(yosys_ext INSTALL_DIR)
set(YOSYS_PREFIX   ${INSTALL_DIR} CACHE PATH "" FORCE)
set(YOSYS_INCLUDE  ${INSTALL_DIR}/share/yosys/include CACHE PATH "" FORCE)
set(YOSYS_DATDIR   ${INSTALL_DIR}/share/yosys CACHE PATH "" FORCE)
set(YOSYS_BIN      ${INSTALL_DIR}/bin/yosys CACHE FILEPATH "" FORCE)

# Create include directory so CMake doesn't complain at configure time
file(MAKE_DIRECTORY ${YOSYS_INCLUDE})

# Import libyosys as a target (Yosys always uses .so, even on macOS)
add_library(libyosys SHARED IMPORTED GLOBAL)
set_target_properties(libyosys PROPERTIES
    IMPORTED_LOCATION ${INSTALL_DIR}/lib/yosys/libyosys.so
    INTERFACE_INCLUDE_DIRECTORIES ${YOSYS_INCLUDE}
)
add_dependencies(libyosys yosys_ext)
