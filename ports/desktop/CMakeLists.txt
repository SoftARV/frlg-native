set(sym "${FRLG_VENDOR_DIR}/pokefirered_modern.sym")
if(NOT EXISTS "${sym}")
    message(FATAL_ERROR
        "Missing ${sym}. Run: make -C ${FRLG_VENDOR_DIR} modern && make -C ${FRLG_VENDOR_DIR} syms MODERN=1")
endif()

# Where .text ends decides whether a symbol is code (stub) or ROM data (bind to
# the cart). It is layout-specific, so it is read from the ELF rather than
# hardcoded -- see docs/spikes/0001-relocation-table.md.
execute_process(
    COMMAND arm-none-eabi-readelf -SW "${FRLG_VENDOR_DIR}/pokefirered_modern.elf"
    OUTPUT_VARIABLE readelf_out ERROR_QUIET)
string(REGEX MATCH "script_data[ \t]+PROGBITS[ \t]+([0-9a-f]+)" _m "${readelf_out}")
if(CMAKE_MATCH_1)
    set(text_end "0x${CMAKE_MATCH_1}")
else()
    message(FATAL_ERROR "could not read script_data base from the ROM ELF")
endif()
message(STATUS "frlg-native: .text ends at ${text_end}")

set(bindings "${CMAKE_CURRENT_BINARY_DIR}/bindings.rsp")

# Symbols nothing defines: ROM data, RAM variables, and routines the port has not
# written yet. Found by trial link, because only the linker knows what the host
# C library already provides -- see tools/find_unbound.sh.
# Deferred subsystems, by defining source rather than by name. script.c is
# deliberately absent: the script VM is game logic, and a silent no-op there
# would corrupt state rather than skip a feature.
add_custom_command(
    OUTPUT "${bindings}"
    COMMAND sh "${CMAKE_SOURCE_DIR}/tools/gen_deferred_syms.sh"
            "${FRLG_VENDOR_DIR}/build/firered_modern/src"
            "${CMAKE_CURRENT_BINARY_DIR}/deferred.txt"
            m4a.o m4a_1.o
            agb_flash.o agb_flash_1m.o agb_flash_le.o agb_flash_mx.o
            librfu_rfu.o librfu_stwi.o librfu_sio32id.o librfu_intr.o sloopsvc.o
            multiboot.o libgcnmultiboot.o isagbprn.o
    COMMAND sh "${CMAKE_SOURCE_DIR}/tools/find_unbound.sh"
            "${CMAKE_CURRENT_BINARY_DIR}/missing.txt"
            "${CMAKE_C_COMPILER}" -m32 "${CMAKE_CURRENT_SOURCE_DIR}/main.c"
            -I "${CMAKE_SOURCE_DIR}/platform/agb/include"
            -Wl,--whole-archive "$<TARGET_FILE:game>" -Wl,--no-whole-archive
            "$<TARGET_FILE:agb>" -lm
            -o "${CMAKE_CURRENT_BINARY_DIR}/trial.out"
    COMMAND "${CMAKE_COMMAND}" -E env python3
            "${CMAKE_SOURCE_DIR}/tools/gen_symbol_bindings.py"
            "${sym}" "${CMAKE_CURRENT_BINARY_DIR}/missing.txt"
            -o "${bindings}" --text-end "${text_end}" --driver --report
            --stub-source "${CMAKE_CURRENT_BINARY_DIR}/agb_stubs.c"
            --soft-list "${CMAKE_CURRENT_BINARY_DIR}/deferred.txt"
    WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
    DEPENDS game agb "${CMAKE_SOURCE_DIR}/tools/gen_symbol_bindings.py"
    COMMENT "resolving unbound game symbols"
    VERBATIM)

add_custom_target(frlg_bindings DEPENDS "${bindings}")

set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/agb_stubs.c"
    PROPERTIES GENERATED TRUE)

add_executable(frlg-native main.c "${CMAKE_CURRENT_BINARY_DIR}/agb_stubs.c")
add_dependencies(frlg-native frlg_bindings)

target_link_libraries(frlg-native PRIVATE
    -Wl,--whole-archive game -Wl,--no-whole-archive
    agb)
# -rdynamic so the unimplemented-stub backtrace resolves names.
target_link_options(frlg-native PRIVATE "@${bindings}" -rdynamic)
