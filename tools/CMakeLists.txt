# The mGBA reference capture is a host tool, not part of the port. It links the
# system libmgba, which is 64-bit, so it cannot inherit the port's -m32 and is
# compiled on its own rather than through add_executable. It shares nothing with
# the port but the PPM format the harness reads.
#
# Optional: without libmgba the golden tier still runs, but its goldens can only
# be blessed from our own renderer, which makes it a regression net rather than
# an oracle. See docs/adr/0010-goldens-are-generated.md.

find_library(FRLG_MGBA_LIBRARY mgba)
find_path(FRLG_MGBA_INCLUDE_DIR mgba/core/core.h)

if(FRLG_MGBA_LIBRARY AND FRLG_MGBA_INCLUDE_DIR)
    set(capture "${CMAKE_CURRENT_BINARY_DIR}/mgba-capture")
    add_custom_command(
        OUTPUT "${capture}"
        # No -m32, and gnu11 rather than c11: mGBA's headers use PATH_MAX.
        COMMAND "${CMAKE_C_COMPILER}" -O2 -std=gnu11 -Wall -Wextra
                "-I${FRLG_MGBA_INCLUDE_DIR}"
                "${CMAKE_CURRENT_SOURCE_DIR}/mgba_capture.c"
                "${FRLG_MGBA_LIBRARY}"
                -o "${capture}"
        DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/mgba_capture.c"
        COMMENT "building mgba-capture (golden reference tool)"
        VERBATIM)
    # The target cannot share a name with the file it produces.
    add_custom_target(mgba-capture-tool ALL DEPENDS "${capture}")
    message(STATUS "frlg-native: mgba-capture will be built; goldens can be blessed from the ROM")
else()
    message(STATUS "frlg-native: libmgba not found, goldens can only be blessed from our own renderer")
endif()
