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
