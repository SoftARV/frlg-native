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
set(FRLG_DESTATIC "${CMAKE_SOURCE_DIR}/tools/destatic.py")
set(FRLG_SPLIT_SYMBOLS "${CMAKE_SOURCE_DIR}/tools/split_data_symbols.py")
set(FRLG_HOIST "${CMAKE_SOURCE_DIR}/tools/hoist_static.py")

# Source patches: unified diffs against upstream .c files, applied to a copy in
# the build tree before anything else runs.
#
# This is the mechanism for a change an insertion cannot express -- replacing a
# function body rather than adding a line to it. A fork would do the same job by
# copying the file here and editing it, which would put pret's code in this
# repository; a diff holds only what we changed. See ADR 0023.
set(FRLG_GAME_PATCH_DIR "${CMAKE_SOURCE_DIR}/platform/game/overrides")

# Statics declared inside a function. `static` there means storage, not linkage,
# so destatic cannot touch them -- the declaration has to move out to file scope
# instead, under a name of its own. The list is written by hand because there is
# no generator that knows which function each belongs to. ADR 0020.
#
# Addresses come from the linker map (tools/resolve_by_map.py), never from
# matching bytes: an eight-byte animation is `{ANIMCMD_FRAME(n, 5), ANIMCMD_END}`
# in dozens of places, so content matching binds whichever copy it meets first.
# sAnim_Cursor_Fist was bound that way to 0x260104, a coincidence 1.1 MB from
# where it lives.
#
# Two entries may name the same symbol: each hoist renames the first definition
# still called that, so the entries are consumed in the order they appear here
# and must be listed in the order the definitions appear in the file.
set(FRLG_HOIST_STATICS
    "string_util.c=lengths=frlg_string_util_lengths#0x231ea8"
    "event_object_movement.c=jumpLandingFlags=frlg_eom_jumpLandingFlags#0x3a7044"
    "event_object_movement.c=bikeTireTracks_Transitions=frlg_eom_bikeTireTracks#0x3a70ac"
    "pokemon_storage_system_data.c=sOamData_Cursor=frlg_pss_sOamData_Cursor#0x3d34c8"
    "pokemon_storage_system_data.c=sOamData_CursorShadow=frlg_pss_sOamData_CursorShadow#0x3d34d0"
    "pokemon_storage_system_data.c=sAnim_Cursor_Bouncing=frlg_pss_sAnim_Cursor_Bouncing#0x3d34d8"
    "pokemon_storage_system_data.c=sAnim_Cursor_Still=frlg_pss_sAnim_Cursor_Still#0x3d34e4"
    "pokemon_storage_system_data.c=sAnim_Cursor_Open=frlg_pss_sAnim_Cursor_Open#0x3d34ec"
    "pokemon_storage_system_data.c=sAnim_Cursor_Fist=frlg_pss_sAnim_Cursor_Fist#0x3d34f4"
    "trainer_fan_club.c=sFanClubMemberIds=frlg_tfc_sFanClubMemberIds_gain#0x456938"
    "trainer_fan_club.c=sFanClubMemberIds=frlg_tfc_sFanClubMemberIds_lose#0x456940")
# Read at link time too, from another directory's scope.
set(FRLG_HOIST_STATICS "${FRLG_HOIST_STATICS}" CACHE INTERNAL "hoisted statics")

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
# Two lists exist. The default is the one every configuration has been verified
# against; FRLG_GAME_DATA_STATICS selects the full one, which also extracts the
# file-scope statics and leaves essentially no game data in the binary.
#
# This is an option rather than a file somebody swaps in, because swapping it in
# is exactly how a build directory called "zero-play" quietly became a
# globals-only build: the list is restored, the directory is rebuilt, and its
# name still claims otherwise.
option(FRLG_GAME_DATA_STATICS "Extract file-scope statics from the ROM as well" ON)
if(FRLG_GAME_DATA_STATICS)
    include("${CMAKE_CURRENT_LIST_DIR}/game_data_symbols_statics.cmake")
    set(FRLG_SYMBOL_LIST "${CMAKE_CURRENT_LIST_DIR}/game_data_symbols_statics.cmake"
        CACHE INTERNAL "the extraction list in force")
else()
    include("${CMAKE_CURRENT_LIST_DIR}/game_data_symbols.cmake")
    set(FRLG_SYMBOL_LIST "${CMAKE_CURRENT_LIST_DIR}/game_data_symbols.cmake"
        CACHE INTERNAL "the extraction list in force")
endif()

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
set(FRLG_GAME_PATCH_LAYOUT load_save.c battle_anim_normal.c)

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
                         pokemon_storage_system_tasks.c pokemon_storage_system_graphics.c
                         region_map.c battle_controllers.c trade_scene.c teachy_tv.c)
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
        # Not inside the symbol-list loop below: a file can have a table to lift
        # without defining any extracted symbol of its own, and nesting this
        # there skipped it without saying so -- trainer_fan_club.c kept both its
        # copies while the bindings for them were written and left unreferenced.
        foreach(h ${FRLG_HOIST_STATICS})
            string(REGEX MATCH "^([^=]+)=([^=]+)=([^#]+)#(.*)$" _ "${h}")
            if(CMAKE_MATCH_1 STREQUAL base)
                list(APPEND post COMMAND "${CMAKE_COMMAND}" -E env python3
                     "${FRLG_HOIST}" "${c}" "${CMAKE_MATCH_2}=${CMAKE_MATCH_3}")
                list(APPEND extra_deps "${FRLG_HOIST}")
            endif()
        endforeach()
        foreach(entry ${FRLG_GAME_DATA_SYMBOLS})
            string(REGEX MATCH "^([^=]+)=(.*)$" _ "${entry}")
            if(CMAKE_MATCH_1 STREQUAL base)
                # Globals are cut from the source; statics only lose the
                # `static` keyword and are replaced by the linker. ADR 0020.
                execute_process(COMMAND python3 "${FRLG_SPLIT_SYMBOLS}"
                                "${FRLG_SYMBOL_LIST}" "${base}" --cut
                                OUTPUT_VARIABLE to_cut)
                execute_process(COMMAND python3 "${FRLG_SPLIT_SYMBOLS}"
                                "${FRLG_SYMBOL_LIST}" "${base}" --destatic
                                OUTPUT_VARIABLE to_destatic)
                if(to_cut)
                    list(APPEND post COMMAND "${CMAKE_COMMAND}" -E env python3
                         "${FRLG_CUT_DATA}" "${c}" ${to_cut})
                    list(APPEND extra_deps "${FRLG_CUT_DATA}")
                endif()
                if(to_destatic)
                    list(APPEND post COMMAND "${CMAKE_COMMAND}" -E env python3
                         "${FRLG_DESTATIC}" "${c}" ${to_destatic})
                    list(APPEND extra_deps "${FRLG_DESTATIC}")
                endif()
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

    # A patched source is cpp'd from a copy in the build tree; the submodule
    # stays untouched. Quoted includes still resolve -- cpp tries the including
    # file's own directory first and -iquote include second, and upstream
    # headers are found by the second anyway.
    set(patch "${FRLG_GAME_PATCH_DIR}/${name}.patch")
    set(source_in "${rel}")
    set(patch_step "")
    set(patch_dep "")
    if(EXISTS "${patch}")
        set(patched "${stage_dir}/${name}.patched.c")
        set(source_in "${patched}")
        set(patch_dep "${patch}")
        set(patch_step
            COMMAND "${CMAKE_COMMAND}" -E copy "${FRLG_VENDOR_DIR}/${rel}" "${patched}"
            COMMAND patch --forward --silent -p1 "${patched}" "${patch}")
    endif()

    add_custom_command(
        OUTPUT "${c}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage_dir}"
        ${patch_step}
        COMMAND "${CMAKE_C_COMPILER}" ${cppflags} "${source_in}" -o "${i}"
        COMMAND sh -c "'${FRLG_PREPROC}' '${i}' charmap.txt > '${c}'"
        ${post}
        WORKING_DIRECTORY "${FRLG_VENDOR_DIR}"
        DEPENDS "${FRLG_VENDOR_DIR}/${rel}" "${FRLG_PREPROC}" ${patch_dep}
                ${FRLG_PRELUDE_DEPS} ${extra_deps}
        COMMENT "preproc ${rel}"
        VERBATIM)

    set(${out_var} "${c}" PARENT_SCOPE)
endfunction()

# The port's own game-layer sources (ADR 0022). Same treatment as an upstream
# one -- the game's headers, the game's prelude, and preproc -- because that is
# what the code has to compile against and because the port's UI strings need
# the game's text encoding: `_("PORT")` becomes bytes only if preproc sees it.
#
# Separate from the function above because that one names its input relative to
# the vendor tree and these live outside it. Everything else is deliberately the
# same, including the working directory, which is what makes charmap.txt and
# `-iquote include` resolve.
function(frlg_preprocess_port_game abs out_var)
    get_filename_component(name "${abs}" NAME_WE)
    set(stage_dir "${CMAKE_CURRENT_BINARY_DIR}/pp")
    set(i "${stage_dir}/${name}.i")
    set(c "${stage_dir}/${name}.c")

    add_custom_command(
        OUTPUT "${c}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage_dir}"
        # The host headers are added here and nowhere else: these sources may
        # call downward past platform/agb (ADR 0022), and an upstream source
        # never should.
        COMMAND "${CMAKE_C_COMPILER}" ${FRLG_GAME_CPPFLAGS}
                -I "${CMAKE_SOURCE_DIR}/platform/host/include" "${abs}" -o "${i}"
        COMMAND sh -c "'${FRLG_PREPROC}' '${i}' charmap.txt > '${c}'"
        # The same widening the upstream sources get: this code names the game's
        # structs, so it has to agree with them about how big they are.
        COMMAND "${CMAKE_COMMAND}" -E env python3 "${FRLG_PATCH_STRUCTS}" "${c}"
        WORKING_DIRECTORY "${FRLG_VENDOR_DIR}"
        DEPENDS "${abs}" "${FRLG_PREPROC}" "${FRLG_PATCH_STRUCTS}"
                ${FRLG_PRELUDE_DEPS}
        COMMENT "preproc port ${name}.c"
        VERBATIM)

    set(${out_var} "${c}" PARENT_SCOPE)
endfunction()
