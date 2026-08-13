add_library(host STATIC)
target_include_directories(host PUBLIC include)

# Backend-independent: the headless build records what the windowed one does, so
# the whole session path can be exercised without a display.
target_sources(host PRIVATE src/session.c)
find_package(Threads REQUIRED)
target_link_libraries(host PUBLIC Threads::Threads)

if(FRLG_HOST_BACKEND STREQUAL "sdl3")
    # 32-bit SDL3 ships no CMake package on some distros, so pkg-config is the
    # portable route until a toolchain file says otherwise.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)

    target_sources(host PRIVATE src/sdl3/host_sdl3.c)
    target_link_libraries(host PUBLIC PkgConfig::SDL3)
    message(STATUS "frlg-native: host backend sdl3 ${SDL3_VERSION}")
else()
    target_sources(host PRIVATE src/null/host_null.c)
    message(STATUS "frlg-native: host backend null (headless)")
endif()
