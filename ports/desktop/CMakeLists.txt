# Which ROM build the manifest describes, and it is not a free choice: the two
# builds put the game's data in different places. A modern .text is larger, so
# everything after it shifts -- script_data sits 66,524 bytes apart between them
# -- and a manifest from the modern build names addresses no cartridge has.
#
#   dev     the MODERN=1 build, for development against our own ROM
#   retail  the byte-matching build, which is what a player supplies
#
# See docs/spikes/0001-relocation-table.md, finding 2.
set(FRLG_GAME_DATA "dev" CACHE STRING "ROM build the manifest describes: dev or retail")
set_property(CACHE FRLG_GAME_DATA PROPERTY STRINGS dev retail)

if(FRLG_GAME_DATA STREQUAL "retail")
    set(rom_stem "pokefirered")
    set(rom_objs "${FRLG_VENDOR_DIR}/build/firered/src")
    set(rom_make "make -C ${FRLG_VENDOR_DIR} compare LDFLAGS='-Map ../../pokefirered.map --emit-relocs' && make -C ${FRLG_VENDOR_DIR} syms")
elseif(FRLG_GAME_DATA STREQUAL "dev")
    set(rom_stem "pokefirered_modern")
    set(rom_objs "${FRLG_VENDOR_DIR}/build/firered_modern/src")
    set(rom_make "make -C ${FRLG_VENDOR_DIR} modern && make -C ${FRLG_VENDOR_DIR} syms MODERN=1")
else()
    message(FATAL_ERROR "FRLG_GAME_DATA is '${FRLG_GAME_DATA}'; expected dev or retail")
endif()
message(STATUS "frlg-native: game data from the ${FRLG_GAME_DATA} build (${rom_stem})")

set(rom_elf "${FRLG_VENDOR_DIR}/${rom_stem}.elf")
set(rom_gba "${FRLG_VENDOR_DIR}/${rom_stem}.gba")
set(sym "${FRLG_VENDOR_DIR}/${rom_stem}.sym")
if(NOT EXISTS "${sym}")
    message(FATAL_ERROR "Missing ${sym}. Run: ${rom_make}")
endif()

# The ROM's data holds the addresses the original build gave its own code and
# RAM, so each is rewritten once when the image is loaded. The table naming them
# is generated from the same build the bindings come from -- it has to be, since
# the two describe each other. See docs/spikes/0005-relocation-classes.md.
set(relocs "${CMAKE_CURRENT_BINARY_DIR}/agb_relocations.c")
add_custom_command(
    OUTPUT "${relocs}"
    COMMAND "${CMAKE_COMMAND}" -E env python3
            "${CMAKE_SOURCE_DIR}/tools/gen_relocations.py"
            "${rom_elf}"
            "${rom_gba}"
            -o "${relocs}" --report
    DEPENDS "${CMAKE_SOURCE_DIR}/tools/gen_relocations.py"
            "${rom_elf}"
    COMMENT "generating the cart relocation table"
    VERBATIM)

# Its own library so that both links can pull it in whole: the trial link needs
# to see the symbols it names, or they are never discovered as unbound.
add_library(cartreloc STATIC "${relocs}")
target_include_directories(cartreloc PRIVATE "${CMAKE_SOURCE_DIR}/platform/agb/include")

set(bindings "${CMAKE_CURRENT_BINARY_DIR}/bindings.rsp")

# --whole-archive pulls every game object in regardless, so the trial link needs
# no real entry point. Using the actual main.c would drag host and pthread into
# the unresolved list and bind them as if they were missing game routines.
set(trial_main "${CMAKE_CURRENT_BINARY_DIR}/trial_main.c")
file(WRITE "${trial_main}" "int main(void) { return 0; }\n")

# Symbols nothing defines: ROM data, RAM variables, and routines the port has not
# written yet. Found by trial link, because only the linker knows what the host
# C library already provides -- see tools/find_unbound.sh.
# Deferred subsystems, by defining source rather than by name. script.c is
# deliberately absent: the script VM is game logic, and a silent no-op there
# would corrupt state rather than skip a feature.
# Both files are outputs. Declaring only the response file leaves the stub
# source undeclared, so a build that newly implements a stubbed symbol links
# against the previous stub object and fails once before succeeding.
add_custom_command(
    OUTPUT "${bindings}" "${CMAKE_CURRENT_BINARY_DIR}/agb_stubs.c"
    COMMAND sh "${CMAKE_SOURCE_DIR}/tools/gen_deferred_syms.sh"
            "${rom_objs}"
            "${CMAKE_CURRENT_BINARY_DIR}/deferred.txt"
            m4a.o m4a_1.o
            agb_flash.o agb_flash_1m.o agb_flash_le.o agb_flash_mx.o
            librfu_rfu.o librfu_stwi.o librfu_sio32id.o librfu_intr.o sloopsvc.o
            multiboot.o libgcnmultiboot.o isagbprn.o
    COMMAND sh "${CMAKE_SOURCE_DIR}/tools/find_unbound.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/missing.txt"
            "${CMAKE_C_COMPILER}" -m32 "${trial_main}"
            -Wl,--whole-archive "$<TARGET_FILE:game>" "$<TARGET_FILE:cartreloc>"
            -Wl,--no-whole-archive
            "$<TARGET_FILE:agb>" -lm
            -o "${CMAKE_CURRENT_BINARY_DIR}/trial.out"
    COMMAND "${CMAKE_COMMAND}" -E env python3
            "${CMAKE_SOURCE_DIR}/tools/gen_symbol_bindings.py"
            "${sym}" "${CMAKE_CURRENT_BINARY_DIR}/missing.txt"
            -o "${bindings}" --elf "${rom_elf}" --driver --report
            --stub-source "${CMAKE_CURRENT_BINARY_DIR}/agb_stubs.c"
            --soft-list "${CMAKE_CURRENT_BINARY_DIR}/deferred.txt"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS game agb cartreloc "${CMAKE_SOURCE_DIR}/tools/gen_symbol_bindings.py"
    COMMENT "resolving unbound game symbols"
    VERBATIM)

add_custom_target(frlg_bindings DEPENDS "${bindings}")

set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/agb_stubs.c"
    PROPERTIES GENERATED TRUE)

add_executable(frlg-native main.c crash.c "${CMAKE_CURRENT_BINARY_DIR}/agb_stubs.c")
add_dependencies(frlg-native frlg_bindings)

target_link_libraries(frlg-native PRIVATE
    -Wl,--whole-archive game cartreloc -Wl,--no-whole-archive
    agb host pthread)
# -rdynamic so the unimplemented-stub backtrace resolves names.
# The manifest describes one exact image, so the port can say whether the file
# it was handed is that image. Computed from the ROM the generators just read,
# which is the only way the two cannot drift apart.
file(SHA1 "${rom_gba}" rom_sha1)
message(STATUS "frlg-native: manifest describes ${rom_sha1}")

if(FRLG_GAME_DATA STREQUAL "retail")
    set(rom_title "Pokemon FireRed (rev 0)")
else()
    set(rom_title "Pokemon FireRed (rev 0, development build)")
endif()

# Development default only: the shipping build takes the path from the player,
# and ships no ROM at all (ADR 0006).
target_compile_definitions(frlg-native PRIVATE
    FRLG_DEFAULT_ROM="${rom_gba}"
    FRLG_ROM_SHA1="${rom_sha1}"
    FRLG_ROM_TITLE="${rom_title}")

# Two of the sequencer's tuning values are absolutes from upstream's linker
# script rather than variables, declared `extern char []` and read as their own
# address. Same values it uses. gMaxLines matters: non-zero would switch on the
# mixer's scanline budget, which cannot work here (ARCHITECTURE.md 6.7).
set(FRLG_SOUND_DEFSYMS -Wl,--defsym,gNumMusicPlayers=4 -Wl,--defsym,gMaxLines=0)

# -no-pie because the bindings are absolute symbols. `--defsym sym=agb_cart+N`
# defines an ABSOLUTE address, which a position-independent executable does not
# rebase when the loader moves the image -- so every bound symbol would point at
# unmapped memory. This is the absolute-symbol-binding risk ADR 0006 records.
target_link_options(frlg-native PRIVATE "@${bindings}" -rdynamic -no-pie ${FRLG_SOUND_DEFSYMS})
