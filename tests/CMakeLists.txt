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

frlg_add_test(test_ppu_objects)
frlg_add_test(test_ppu_bg_affine)
frlg_add_test(test_ppu_windows)
frlg_add_test(test_ppu_blend)
frlg_add_test(test_ppu_mosaic)
frlg_add_test(test_ppu_bitmap)
frlg_add_test(test_ppu_scanline_irq)

# The mixer works on the game's own audio structures, so its test needs the
# vendor headers -- by quote path only, as everywhere else.
frlg_add_test(test_m4a_envelope)
target_compile_options(test_m4a_envelope PRIVATE -iquote "${FRLG_VENDOR_DIR}/include")

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
