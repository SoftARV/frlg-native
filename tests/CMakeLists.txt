# Unit tests link the real `agb` archive, so they cover the shipped object files
# rather than a recompile with different flags. Only the members a test actually
# references are pulled in, which keeps the PPU tests clear of the game library.

function(frlg_add_test name)
    add_executable("${name}" "${name}.c")
    target_link_libraries("${name}" PRIVATE agb)
    target_include_directories("${name}" PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_options("${name}" PRIVATE -Wall -Wextra)
    add_test(NAME "${name}" COMMAND "${name}")
endfunction()

frlg_add_test(test_ppu_objects)

frlg_add_test(test_ppu_bg_affine)
