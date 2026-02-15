include(FetchContent)

set(SLANG_TAG "v7.0" CACHE STRING "slang version")

FetchContent_Declare(slang
    GIT_REPOSITORY https://github.com/MikePopoloski/slang.git
    GIT_TAG        ${SLANG_TAG}
    GIT_SHALLOW    TRUE
)

# Disable slang tests and tools we don't need
set(SLANG_INCLUDE_TESTS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_TOOLS OFF CACHE BOOL "" FORCE)
set(SLANG_INCLUDE_DOCS OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(slang)
