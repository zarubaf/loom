include(FetchContent)

FetchContent_Declare(replxx
    GIT_REPOSITORY https://github.com/AmokHuginnsson/replxx.git
    GIT_TAG        release-0.0.4
    GIT_SHALLOW    TRUE
)

set(REPLXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(replxx)
