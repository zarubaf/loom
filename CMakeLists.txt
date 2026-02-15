# SPDX-License-Identifier: Apache-2.0
cmake_minimum_required(VERSION 3.20)
project(loom LANGUAGES CXX)

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
include(cmake/FetchGoogleTest.cmake)

# ---------- Plugins ----------
add_subdirectory(passes)
add_subdirectory(frontend)

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
install(TARGETS scan_insert dpi_bridge mem_shadow
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}/loom
)

# Install yosys-slang plugin
install(FILES ${YOSYS_SLANG_PLUGIN}
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/loom
    RENAME slang.so
)

# Install CLI wrapper
install(PROGRAMS ${CMAKE_SOURCE_DIR}/bin/loom
    DESTINATION ${CMAKE_INSTALL_BINDIR}
)
