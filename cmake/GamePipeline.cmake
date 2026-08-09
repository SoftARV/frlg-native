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

# Sources carrying ARM inline assembly. Each is already destined for an override
# for an independent reason; see the override table in docs/ARCHITECTURE.md.
set(FRLG_GAME_EXCLUDED
    main.c            # IWRAM clear; overridden anyway for the fiber frame loop
    script.c          # svc 2 (HALT); overridden anyway for the pointer accessor
    m4a.c             # swi 0x2A; overridden anyway for the C mixer
    multiboot.c       # ARM busy-wait; GameCube link, out of scope
    librfu_intr.c     # naked ARM trampolines; RFU wireless, stubbed until phase 10
)

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
    set(stage_dir "${CMAKE_CURRENT_BINARY_DIR}/pp")
    set(i "${stage_dir}/${name}.i")
    set(c "${stage_dir}/${name}.c")

    add_custom_command(
        OUTPUT "${c}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${stage_dir}"
        COMMAND "${CMAKE_C_COMPILER}" ${FRLG_GAME_CPPFLAGS} "${rel}" -o "${i}"
        COMMAND sh -c "'${FRLG_PREPROC}' '${i}' charmap.txt > '${c}'"
        WORKING_DIRECTORY "${FRLG_VENDOR_DIR}"
        DEPENDS "${FRLG_VENDOR_DIR}/${rel}" "${FRLG_PREPROC}"
        COMMENT "preproc ${rel}"
        VERBATIM)

    set(${out_var} "${c}" PARENT_SCOPE)
endfunction()
