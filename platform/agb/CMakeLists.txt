include(${CMAKE_SOURCE_DIR}/cmake/GamePipeline.cmake)

add_library(agb STATIC
    src/memmap.c
    src/dma.c
    src/bios.c
    src/frame.c
    src/irq.c
    src/ppu.c
    src/psg.c
    src/cart.c
    src/flash.c
    src/m4a_clear.c
    src/m4a_mixer.c
    src/m4a_track.c
    src/unimplemented.c)

target_include_directories(agb PUBLIC include)

# -iquote, not -I: vendor/include carries a strings.h that would hijack POSIX
# <strings.h> for host headers. See docs/ARCHITECTURE.md 4.1.
target_compile_options(agb PRIVATE -iquote "${FRLG_VENDOR_DIR}/include")
target_link_libraries(agb PUBLIC m)

frlg_collect_game_sources(game_rel_sources)

set(game_pp_sources "")
foreach(rel ${game_rel_sources})
    frlg_preprocess_game("${rel}" pp)
    list(APPEND game_pp_sources "${pp}")
endforeach()

list(LENGTH game_pp_sources game_source_count)
message(STATUS "frlg-native: ${game_source_count} game sources through preproc")

add_library(game STATIC ${game_pp_sources})

# Already preprocessed; -x cpp-output stops cpp running over them a second time.
target_compile_options(game PRIVATE -x cpp-output)
target_link_libraries(game PRIVATE agb)
