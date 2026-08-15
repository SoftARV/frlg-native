# Unit tests link the real `agb` archive, so they cover the shipped object files
# rather than a recompile with different flags. Only the members a test actually
# references are pulled in, which keeps the PPU tests clear of the game library.
#
# Two labels: `unit` is the fast path, `golden` runs the port for thousands of
# frames and takes about a minute. `ctest -L unit` is what you want while
# working; plain `ctest` runs both.

function(frlg_add_test name)
    add_executable("${name}" "${name}.c" harness.c)
    target_link_libraries("${name}" PRIVATE agb)
    target_include_directories("${name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options("${name}" PRIVATE -Wall -Wextra)
    add_test(NAME "${name}" COMMAND "${name}")
    set_tests_properties("${name}" PROPERTIES LABELS unit)
endfunction()

frlg_add_test(test_sha1)
frlg_add_test(test_cart)
frlg_add_test(test_flash)
target_compile_options(test_flash PRIVATE -iquote "${FRLG_VENDOR_DIR}/include")
# The one test that writes a file. Per build directory, not /tmp: two presets'
# suites running at once shared the fixed path and erased each other's chip.
target_compile_definitions(test_flash PRIVATE
    FRLG_TEST_SAVE_PATH="${CMAKE_CURRENT_BINARY_DIR}/test_flash.sav")
frlg_add_test(test_dma_timed)
frlg_add_test(test_psg)
frlg_add_test(test_ppu_objects)
frlg_add_test(test_ppu_bg_affine)
frlg_add_test(test_ppu_windows)
frlg_add_test(test_ppu_blend)
frlg_add_test(test_ppu_mosaic)
frlg_add_test(test_ppu_bitmap)
frlg_add_test(test_ppu_scanline_irq)

# The seam is compiled in rather than linked from the host archive: the test
# supplies its own present and log, which is the only way to see what a stage
# did and what a failing one was blamed for.
frlg_add_test(test_render)
target_sources(test_render PRIVATE
    "${CMAKE_SOURCE_DIR}/platform/host/src/render.c")
target_include_directories(test_render PRIVATE
    "${CMAKE_SOURCE_DIR}/platform/host/include")

# The mixer works on the game's own audio structures, so its test needs the
# vendor headers -- by quote path only, as everywhere else.
# The PSG test names the hardware registers, which live in the vendor headers.
target_compile_options(test_psg PRIVATE -iquote "${FRLG_VENDOR_DIR}/include")

foreach(m4a_test test_m4a_envelope test_m4a_mix test_m4a_frame test_m4a_track
                  test_m4a_output_mix)
    frlg_add_test("${m4a_test}")
    target_compile_options("${m4a_test}" PRIVATE -iquote "${FRLG_VENDOR_DIR}/include")
endforeach()

# Extraction serves the cartridge's bytes in place of a definition, which is
# only correct if the two agree. They are produced by different compilers and
# need not: struct BgTemplate is four bytes in both, and agbcc packs its
# bitfields across the storage units where ours does not -- same size, different
# bits, and two screens with no backgrounds. A size comparison cannot see that;
# this compares the bytes.
# It needs a build that compiles the data in, to compare against -- this build
# cannot check itself, having replaced the definitions it would be checking.
# Point FRLG_REFERENCE_BINARY at one (a -DFRLG_GAME_DATA_FROM_ROM=OFF build) and
# the test appears; without one there is nothing to compare and it does not.
if(FRLG_GAME_DATA_FROM_ROM AND FRLG_REFERENCE_BINARY)
    add_test(NAME extraction_bytes
        COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/verify_extraction.py"
                "${FRLG_REFERENCE_BINARY}" "${FRLG_SYMBOL_LIST}"
                "${FRLG_VENDOR_DIR}/pokefirered.gba")
    set_tests_properties(extraction_bytes PROPERTIES LABELS unit TIMEOUT 600)
endif()

set(golden "${CMAKE_SOURCE_DIR}/tools/golden.py")

# The thresholds are what make the golden tier survivable, so they are checked
# on their own, without needing a capture at all.
add_test(NAME golden_thresholds COMMAND python3 "${golden}" --self-test)
set_tests_properties(golden_thresholds PROPERTIES LABELS unit)

# Needs goldens, which are generated per machine and never committed:
#   python3 tools/golden.py --binary <port> --bless
add_test(NAME golden
    COMMAND python3 "${golden}"
            --binary "$<TARGET_FILE:frlg-native>"
            --out "${CMAKE_BINARY_DIR}/golden")
set_tests_properties(golden PROPERTIES LABELS golden TIMEOUT 900)

# The mixer running and the mixer producing sound are different things, and only
# one of them is visible in a screenshot. This needs no sound hardware: the null
# host refuses to open a device and the port measures what it mixed regardless.
add_test(NAME audio
    COMMAND python3 "${CMAKE_SOURCE_DIR}/tools/audio_check.py"
            --binary "$<TARGET_FILE:frlg-native>")
set_tests_properties(audio PROPERTIES LABELS golden TIMEOUT 600)
