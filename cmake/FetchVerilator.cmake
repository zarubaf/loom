# SPDX-License-Identifier: Apache-2.0
include(ExternalProject)

set(VERILATOR_TAG "v5.044" CACHE STRING "Verilator version to build")

# Verilator build dependencies:
# - autoconf (to generate configure script)
# - bison, flex (for parser)
# - python3 (for code generation)

ExternalProject_Add(verilator_ext
    GIT_REPOSITORY https://github.com/verilator/verilator.git
    GIT_TAG        ${VERILATOR_TAG}
    GIT_SHALLOW    TRUE
    GIT_SUBMODULES ""  # No submodules needed
    PREFIX         ${CMAKE_BINARY_DIR}/verilator
    CONFIGURE_COMMAND autoconf COMMAND ./configure --prefix=<INSTALL_DIR>
    BUILD_COMMAND   make -j${NPROC}
    INSTALL_COMMAND make install
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
