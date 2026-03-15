# SPDX-License-Identifier: Apache-2.0
include(ExternalProject)

set(VERILATOR_TAG "v5.044" CACHE STRING "Verilator version to build")

# Verilator build dependencies:
# - autoconf (to generate configure script)
# - bison, flex (for parser)
# - python3 (for code generation)

# On macOS, pass compiler and Homebrew include paths so Verilator's autoconf
# picks up the right toolchain (needed for coroutine / C++20 support).
if(APPLE)
    if(EXISTS "/opt/homebrew")
        set(_BREW "/opt/homebrew")
    else()
        set(_BREW "/usr/local")
    endif()
    set(_VERILATOR_ENV
        "CXX=${CMAKE_CXX_COMPILER}"
        "CC=${CMAKE_C_COMPILER}"
        "PATH=${_BREW}/opt/bison/bin:${_BREW}/opt/flex/bin:$ENV{PATH}"
        "CPPFLAGS=-I${_BREW}/opt/flex/include"
    )
else()
    set(_VERILATOR_ENV "")
endif()

ExternalProject_Add(verilator_ext
    GIT_REPOSITORY https://github.com/verilator/verilator.git
    GIT_TAG        ${VERILATOR_TAG}
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES ""  # No submodules needed
    PREFIX         ${CMAKE_BINARY_DIR}/verilator
    CONFIGURE_COMMAND ${CMAKE_COMMAND} -E env ${_VERILATOR_ENV}
        autoconf
    COMMAND ${CMAKE_COMMAND} -E env ${_VERILATOR_ENV}
        ./configure --prefix=<INSTALL_DIR>
    BUILD_COMMAND   ${CMAKE_COMMAND} -E env ${_VERILATOR_ENV}
        make -j${NPROC}
    INSTALL_COMMAND ${CMAKE_COMMAND} -E env ${_VERILATOR_ENV}
        make install
    BUILD_IN_SOURCE TRUE
)

ExternalProject_Get_Property(verilator_ext INSTALL_DIR)
set(VERILATOR_PREFIX ${INSTALL_DIR} CACHE PATH "" FORCE)
set(VERILATOR_BIN    ${INSTALL_DIR}/bin/verilator CACHE FILEPATH "" FORCE)
set(VERILATOR_ROOT   ${INSTALL_DIR}/share/verilator CACHE PATH "" FORCE)

# Make verilator binary available as an imported executable
add_executable(verilator IMPORTED GLOBAL)
set_target_properties(verilator PROPERTIES
    IMPORTED_LOCATION ${VERILATOR_BIN}
)
add_dependencies(verilator verilator_ext)
