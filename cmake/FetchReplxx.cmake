include(FetchContent)

FetchContent_Declare(replxx
    URL      https://github.com/AmokHuginnsson/replxx/archive/refs/tags/release-0.0.4.tar.gz
    URL_HASH SHA256=a22988b2184e1d256e2d111b5749e16ffb1accbf757c7b248226d73c426844c4
)

set(REPLXX_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(replxx)
