# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(loom LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)  # Required for .so plugins
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Parallel jobs for external projects
include(ProcessorCount)
ProcessorCount(NPROC)
if(NOT NPROC OR NPROC EQUAL 0)
    set(NPROC 4)
endif()

# ---------- Dependencies ----------
include(cmake/FetchYosys.cmake)
include(cmake/FetchYosysSlang.cmake)  # SystemVerilog frontend (includes slang)
include(cmake/FetchVerilator.cmake)
include(cmake/FetchGoogleTest.cmake)
include(cmake/FetchReplxx.cmake)

# ---------- Plugins ----------
add_subdirectory(passes)

# ---------- Host library ----------
add_subdirectory(src/host)

# ---------- Tools (loomc, loomx) ----------
add_subdirectory(src/tools)

# ---------- Tests ----------
enable_testing()
add_subdirectory(tests)

# ---------- Installation ----------
include(GNUInstallDirs)

# Install Yosys binaries
install(DIRECTORY ${YOSYS_PREFIX}/bin/
    DESTINATION ${CMAKE_INSTALL_BINDIR}
    USE_SOURCE_PERMISSIONS
    FILES_MATCHING PATTERN "yosys*"
)

# Install Yosys data files
install(DIRECTORY ${YOSYS_PREFIX}/share/yosys/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/yosys
)

# Install Loom plugins
install(TARGETS scan_insert loom_instrument mem_shadow
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/loom
)

# Install yosys-slang plugin
install(FILES ${YOSYS_SLANG_PLUGIN}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/loom
    RENAME slang.so
)

# Install loomc and loomx
install(TARGETS loomc loomx
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

# Install RTL files
install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/rtl/
    DESTINATION ${CMAKE_INSTALL_DATADIR}/loom/rtl
    FILES_MATCHING PATTERN "*.sv"
)

# Install public headers (svdpi.h, vpi_user.h)
install(DIRECTORY ${CMAKE_SOURCE_DIR}/src/include/
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/loom
    FILES_MATCHING PATTERN "*.h"
)

# Install headers for dispatch compilation
install(FILES ${CMAKE_SOURCE_DIR}/src/host/loom_dpi_service.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/loom
)

# Install shared Makefile fragments
install(FILES ${CMAKE_SOURCE_DIR}/src/util/mk/loom.mk
    DESTINATION ${CMAKE_INSTALL_DATADIR}/loom
)

# Install code generator scripts
install(PROGRAMS ${CMAKE_SOURCE_DIR}/scripts/loom_dpi_codegen.py
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
