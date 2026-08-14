cmake_minimum_required(VERSION 3.24)
project(frlg-native C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(FRLG_32BIT "Build 32-bit (required until the 64-bit migration, see docs/adr/0003)" ON)
# Whether the game's data comes from the player's ROM or from the binary.
#
# OFF is ADR 0006's developer path: the decompilation's own C definitions are
# compiled in, which is how six phases of work were done. ON is the shipping
# path -- those definitions are not compiled and the symbols resolve into the
# imported cart image instead. Nothing distributable can be built with it OFF.
option(FRLG_GAME_DATA_FROM_ROM "Take game data from the imported ROM, not the binary" OFF)

set(FRLG_HOST_BACKEND "sdl3" CACHE STRING "Host backend: sdl3 or null")
set_property(CACHE FRLG_HOST_BACKEND PROPERTY STRINGS sdl3 null)

set(FRLG_VENDOR_DIR "${CMAKE_CURRENT_SOURCE_DIR}/vendor/pokefirered")
if(NOT EXISTS "${FRLG_VENDOR_DIR}/src/main.c")
    message(FATAL_ERROR
        "vendor/pokefirered is empty. Run: git submodule update --init --recursive")
endif()

# The game embeds 4-byte pointers in its data blobs; see docs/adr/0003-pointer-width.md.
if(FRLG_32BIT)
    if(CMAKE_SIZEOF_VOID_P EQUAL 8 AND NOT CMAKE_C_FLAGS MATCHES "-m32")
        message(FATAL_ERROR
            "FRLG_32BIT is ON but the compiler targets 64-bit. Use a 32-bit preset, "
            "or configure with -DCMAKE_C_FLAGS=-m32.")
    endif()
elseif(NOT FRLG_PTR64_READY)
    message(FATAL_ERROR
        "64-bit builds need the data pipeline migration (docs/ROADMAP.md phase 8).")
endif()

enable_testing()

foreach(dir platform/agb platform/host ports/desktop tools tests)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${dir}/CMakeLists.txt")
        add_subdirectory("${dir}")
    endif()
endforeach()

message(STATUS "frlg-native: 32-bit=${FRLG_32BIT} backend=${FRLG_HOST_BACKEND}")
