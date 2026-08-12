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
set(FRLG_GAME_CPPFLAGS
    -m32 -E -x c -std=gnu11
    -I "${FRLG_AGB_INCLUDE}"
    -include agb/prelude.h
    -iquote include
    -DFIRERED -DREVISION=0 -DENGLISH -DMODERN=1
    -Wno-trigraphs)

# Sources carrying ARM inline assembly that no host target can assemble.
# See the override table in docs/ARCHITECTURE.md.
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
set(FRLG_GAME_PATCH_NULL naming_screen.c load_save.c overworld.c battle_transition.c
                         sprite.c trainer_card.c pokemon_summary_screen.c)
set(FRLG_PATCH_NULL "${CMAKE_SOURCE_DIR}/tools/patch_null_tolerance.py")

# main.c's only ARM assembly is an IWRAM clear inside `#if MODERN`, so upstream's
# own non-modern path avoids it. MODERN gates nothing but NOINLINE and an abs()
# macro, so this changes no layout or ABI -- and RegisterRamReset is ours anyway,
# which is what the modern path was working around.
set(FRLG_GAME_MODERN0 main.c)

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

    # A file may need more than one of these, so they accumulate rather than
    # choosing between themselves.
    set(post "")
    set(extra_deps "")
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
        DEPENDS "${FRLG_VENDOR_DIR}/${rel}" "${FRLG_PREPROC}" ${extra_deps}
        COMMENT "preproc ${rel}"
        VERBATIM)

    set(${out_var} "${c}" PARENT_SCOPE)
endfunction()
