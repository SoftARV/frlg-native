# Reproduces upstream's compile pipeline for game sources: cpp -> preproc -> cc.
# preproc expands the _("...") literals into the game's character encoding and
# resolves INCBIN, so it must run with the vendor tree as the working directory.

set(FRLG_PREPROC "${FRLG_VENDOR_DIR}/tools/preproc/preproc")
set(FRLG_AGB_INCLUDE "${CMAKE_SOURCE_DIR}/platform/agb/include")

# vendor/include stays off the bracket chain on purpose: the game ships a
# strings.h that would otherwise hijack POSIX <strings.h> for host headers.
# For the same reason it must not be named twice — cpp de-duplicates the search
# path and would drop the -iquote entry.
# -std must match the compile step: preprocessing at a newer standard bakes in
# host headers the older standard then rejects (C23 nullptr_t under -std=gnu11).
# The prelude is force-included into every one of these, and this stage expands
# it away: what the compiler later sees is the already-expanded copy, so a change
# to the prelude that does not re-run this stage has no effect at all and no
# error to show for it. Naming it as a dependency is what makes an edit land.
# Its own headers go in the same list; vendor's are pinned, and a bump is its own
# commit.
set(FRLG_PRELUDE_DEPS
    "${FRLG_AGB_INCLUDE}/agb/prelude.h"
    "${FRLG_AGB_INCLUDE}/agb/memmap.h"
    "${FRLG_AGB_INCLUDE}/agb/dma.h"
    "${FRLG_AGB_INCLUDE}/agb/frame.h")

set(FRLG_GAME_CPPFLAGS
    -m32 -E -x c -std=gnu11
    -I "${FRLG_AGB_INCLUDE}"
    -include agb/prelude.h
    -iquote include
    -DFIRERED -DREVISION=0 -DENGLISH -DMODERN=1
    -Wno-trigraphs)

# Sources carrying ARM inline assembly that no host target can assemble.
# See the override table in docs/ARCHITECTURE.md.
# Translation units that are only data.
#
# ADR 0006 says the binary ships no game data and the player's ROM supplies it,
# and the mechanism for that already exists -- a symbol the linker cannot resolve
# is bound to its address inside the cart region. What was missing is that these
# were never *unresolved*: the decompilation defines them in C, so they compiled
# in and the binding machinery never saw them. Not compiling them is what hands
# them to the machinery.
#
# Only files with no code in them can go here. That is checked rather than
# assumed -- see tools/check_data_only.py, which fails the build if one of these
# grows a function.
# m4a_tables.c is deliberately absent: its jump table holds pointers to the
# sequencer's own routines, which this port replaces, and the copy in the ROM
# names the cartridge's. That one stays compiled.
set(FRLG_CUT_DATA "${CMAKE_SOURCE_DIR}/tools/patch_data_definitions.py")

set(FRLG_GAME_DATA_ONLY
    graphics.c
    tilesets.c
    data.c
    strings.c
    move_descriptions.c
    trainer_tower_sets.c
    decoration.c
    union_room_message.c
    keyboard_text.c
    mystery_gift_scripts.c
    mystery_event_msg.c
    bg_regs.c)

# A file listed above that grows a function would have it silently dropped, and
# the symbol would bind to whatever the ROM holds at that address -- ARM machine
# code this port cannot execute. Checked against the ROM build's own objects,
# which is the only place these files are compiled at all.
if(FRLG_GAME_DATA_FROM_ROM AND FRLG_GAME_DATA_ONLY)
    execute_process(
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/check_data_only.py"
                arm-none-eabi-readelf "${FRLG_VENDOR_DIR}/build/firered/src"
                ${FRLG_GAME_DATA_ONLY}
        RESULT_VARIABLE data_only_ok
        OUTPUT_VARIABLE data_only_says)
    if(NOT data_only_ok EQUAL 0)
        message(FATAL_ERROR "FRLG_GAME_DATA_ONLY names a file that contains code")
    endif()
    message(STATUS "frlg-native: ${data_only_says}")
endif()

# Data defined in a file that also holds code, which is most of it. The whole
# file cannot be dropped, so the definition is cut out of the preprocessed copy
# and replaced with a declaration; the symbol is then unresolved and binds into
# the cart region like any other.
#
# The list is generated -- tools/gen_data_symbols.py, from the ROM build's own
# symbol table and the decompilation's sources -- because a hand-written one goes
# stale the moment the pin moves, and a symbol that quietly started being
# compiled in again would say nothing.
include("${CMAKE_CURRENT_LIST_DIR}/game_data_symbols.cmake")

set(FRLG_GAME_EXCLUDED
    multiboot.c       # ARM busy-wait; GameCube link, out of scope
    librfu_intr.c     # naked ARM trampolines; RFU wireless, stubbed until phase 10

    # RFU wireless-adapter drivers. These spin on adapter registers that never
    # change without the hardware, which reads as a hang rather than an error.
    # The link*.c game logic above them stays compiled. Phase 10 implements the
    # transport; see docs/ARCHITECTURE.md 6.9.
    librfu_rfu.c
    librfu_stwi.c
    librfu_sio32id.c
    sloopsvc.c

    # Save flash drivers. ReadFlashId copies Thumb code into a stack buffer and
    # calls it, which no native target can do. Replaced by a host-file backed
    # implementation in phase 5; see docs/ARCHITECTURE.md 6.8.
    agb_flash.c
    agb_flash_1m.c
    agb_flash_le.c
    agb_flash_mx.c

    # Debugger print facility. Writes to no$gba/AGB magic I/O addresses that
    # exist in no real region. A host-console implementation would be a genuine
    # improvement over the original; tracked as a later enhancement.
    isagbprn.c
)

# m4a.c is upstream C and needs no override, but two statements in it are
# hardware that no host can run: a BIOS call, and a busy-wait on a scanline our
# frame model can never present. Both are removed from the preprocessed copy by
# a script that fails when either is absent, so a submodule bump is reported
# rather than silently changing what gets built. See docs/ARCHITECTURE.md 6.7.
set(FRLG_GAME_STRIP_WAITS m4a.c main.c script.c)
set(FRLG_STRIP_WAITS "${CMAKE_SOURCE_DIR}/tools/strip_hardware_waits.py")

# load_save.c clears one variable using the combined size of two, which is only
# correct because upstream's linker script places them adjacently. Nothing does
# that here, so the fill runs off the end. See tools/patch_layout_assumptions.py.
set(FRLG_GAME_PATCH_LAYOUT load_save.c)
set(FRLG_PATCH_LAYOUT "${CMAKE_SOURCE_DIR}/tools/patch_layout_assumptions.py")

# The GBA has no MMU, so upstream may read through a null pointer and get
# garbage rather than a fault. naming_screen.c does. See
# tools/patch_null_tolerance.py.
# The cartridge was built by agbcc, which rounds structure sizes up to a multiple
# of four. Types whose natural size is not already a multiple of four therefore
# have a different layout in the ROM than in this build, and every table of them
# read out of the cart is misparsed. See tools/patch_struct_layout.py and the
# override table in docs/ARCHITECTURE.md.
set(FRLG_PATCH_STRUCTS "${CMAKE_SOURCE_DIR}/tools/patch_struct_layout.py")

set(FRLG_GAME_PATCH_NULL naming_screen.c load_save.c overworld.c battle_transition.c
                         sprite.c trainer_card.c pokemon_summary_screen.c
                         pokemon_storage_system_tasks.c region_map.c
                         battle_controllers.c trade_scene.c)
set(FRLG_PATCH_NULL "${CMAKE_SOURCE_DIR}/tools/patch_null_tolerance.py")

# main.c's only ARM assembly is an IWRAM clear inside `#if MODERN`, so upstream's
# own non-modern path avoids it. MODERN gates nothing but NOINLINE and an abs()
# macro, so this changes no layout or ABI -- and RegisterRamReset is ours anyway,
# which is what the modern path was working around.
set(FRLG_GAME_MODERN0 main.c)

# DestroyTask indexes gTasks with whatever it is handed, and FindTaskIdByFunc
# hands it TASK_NONE (255) when the task is already gone -- 10200 bytes past a
# 640-byte array. The read is harmless on a GBA; here it found a non-zero byte
# and unlinked a task that does not exist, writing gTasks[0].prev and orphaning
# the head of the list. RunTasks walks that list, so the running screen simply
# stopped being called: the title screen froze on a black frame and never
# returned to the intro.
#
# Upstream fixed this themselves in revision A, behind `#if REVISION >= 0xA`.
# REVISION appears exactly once in task.c, so building that one file at revision
# A applies their fix and changes nothing else. See docs/ARCHITECTURE.md 4.2.
set(FRLG_GAME_REVA task.c)

function(frlg_collect_game_sources out_var)
    file(GLOB_RECURSE all_c RELATIVE "${FRLG_VENDOR_DIR}" "${FRLG_VENDOR_DIR}/src/*.c")
    set(result "")
    foreach(rel ${all_c})
        if(rel MATCHES "\\.inc\\.c$")
            continue()
        endif()
        get_filename_component(base "${rel}" NAME)
        if(base IN_LIST FRLG_GAME_EXCLUDED)
            continue()
        endif()
        if(FRLG_GAME_DATA_FROM_ROM AND base IN_LIST FRLG_GAME_DATA_ONLY)
            continue()
        endif()
        list(APPEND result "${rel}")
    endforeach()
    set(${out_var} "${result}" PARENT_SCOPE)
endfunction()

function(frlg_preprocess_game rel out_var)
    get_filename_component(name "${rel}" NAME_WE)
    get_filename_component(base "${rel}" NAME)
    set(stage_dir "${CMAKE_CURRENT_BINARY_DIR}/pp")
    set(i "${stage_dir}/${name}.i")
    set(c "${stage_dir}/${name}.c")

    set(cppflags ${FRLG_GAME_CPPFLAGS})
    if(base IN_LIST FRLG_GAME_MODERN0)
        list(TRANSFORM cppflags REPLACE "^-DMODERN=1$" "-DMODERN=0")
    endif()
    if(base IN_LIST FRLG_GAME_REVA)
        list(TRANSFORM cppflags REPLACE "^-DREVISION=0$" "-DREVISION=0xA")
    endif()

    # A file may need more than one of these, so they accumulate rather than
    # choosing between themselves.
    set(post "")
    set(extra_deps "")

    # Runs on every game source rather than a named list: the type reaches 270 of
    # them, and a file that stopped matching would otherwise revert to the narrow
    # layout without saying so. See tools/patch_struct_layout.py.
    list(APPEND post COMMAND "${CMAKE_COMMAND}" -E env python3
         "${FRLG_PATCH_STRUCTS}" "${c}")
    list(APPEND extra_deps "${FRLG_PATCH_STRUCTS}")

    if(FRLG_GAME_DATA_FROM_ROM)
        foreach(entry ${FRLG_GAME_DATA_SYMBOLS})
            string(REGEX MATCH "^([^=]+)=(.*)$" _ "${entry}")
            if(CMAKE_MATCH_1 STREQUAL base)
                string(REPLACE "," ";" wanted "${CMAKE_MATCH_2}")
                list(APPEND post COMMAND "${CMAKE_COMMAND}" -E env python3
                     "${FRLG_CUT_DATA}" "${c}" ${wanted})
                list(APPEND extra_deps "${FRLG_CUT_DATA}")
            endif()
        endforeach()
    endif()
    foreach(pair "FRLG_GAME_STRIP_WAITS;${FRLG_STRIP_WAITS}"
                 "FRLG_GAME_PATCH_LAYOUT;${FRLG_PATCH_LAYOUT}"
                 "FRLG_GAME_PATCH_NULL;${FRLG_PATCH_NULL}")
        list(GET pair 0 list_name)
        list(GET pair 1 script)
        if(base IN_LIST ${list_name})
            list(APPEND post COMMAND "${CMAKE_COMMAND}" -E env python3 "${script}" "${c}")
            list(APPEND extra_deps "${script}")
        endif()
    endforeach()

    add_custom_command(
        OUTPUT "${c}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage_dir}"
        COMMAND "${CMAKE_C_COMPILER}" ${cppflags} "${rel}" -o "${i}"
        COMMAND sh -c "'${FRLG_PREPROC}' '${i}' charmap.txt > '${c}'"
        ${post}
        WORKING_DIRECTORY "${FRLG_VENDOR_DIR}"
        DEPENDS "${FRLG_VENDOR_DIR}/${rel}" "${FRLG_PREPROC}" ${FRLG_PRELUDE_DEPS}
                ${extra_deps}
        COMMENT "preproc ${rel}"
        VERBATIM)

    set(${out_var} "${c}" PARENT_SCOPE)
endfunction()
