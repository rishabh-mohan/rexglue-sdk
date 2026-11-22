# cmake/ppc_test_pipeline.cmake
# PPC assembly test build pipeline
# Provides ppc_add_test_binary() for assembling, linking, and generating
# symbol maps from PPC assembly test files.

# Script mode, used by the symbol-map rule in ppc_add_test_binary():
#   cmake -DPPC_NM=<nm> -DPPC_NM_INPUT=<obj> -DPPC_NM_OUTPUT=<map> -P ppc_test_pipeline.cmake
# Runs nm with its stdout on a pipe and writes the captured bytes to <map> unchanged. The bundled
# Cygwin powerpc-none-elf-nm.exe crashes (0xC0000005) when its stdout is redirected to a file.
# file(WRITE) opens its output in text mode, which turns every LF into CRLF on Windows, so the map is
# written with file(CONFIGURE ... NEWLINE_STYLE LF) (binary mode, LF). With @ONLY, file(CONFIGURE)
# changes only @VAR@ references and #cmakedefine lines; nm output has neither, and the check below
# fails the step rather than write a map that could differ from nm's output.
if(CMAKE_SCRIPT_MODE_FILE AND DEFINED PPC_NM_OUTPUT)
    execute_process(
        COMMAND "${PPC_NM}" --numeric-sort "${PPC_NM_INPUT}"
        OUTPUT_VARIABLE _ppc_nm_output
        RESULT_VARIABLE _ppc_nm_result
    )
    if(NOT _ppc_nm_result STREQUAL "0")
        message(FATAL_ERROR "${PPC_NM} --numeric-sort ${PPC_NM_INPUT} failed: ${_ppc_nm_result}")
    endif()
    if(_ppc_nm_output MATCHES "@|#cmakedefine")
        message(FATAL_ERROR "nm output for ${PPC_NM_INPUT} contains '@' or '#cmakedefine'; not written")
    endif()
    file(REMOVE "${PPC_NM_OUTPUT}")
    if(_ppc_nm_output STREQUAL "")
        file(WRITE "${PPC_NM_OUTPUT}" "")
    else()
        file(CONFIGURE OUTPUT "${PPC_NM_OUTPUT}" CONTENT "${_ppc_nm_output}" @ONLY NEWLINE_STYLE LF)
    endif()
    return()
endif()

# Locate PPC toolchain in tools/ directory
set(PPC_TOOLS_DIR "${PROJECT_SOURCE_DIR}/tools/binutils")

if(WIN32)
    set(PPC_ASSEMBLER "${PPC_TOOLS_DIR}/powerpc-none-elf-as.exe")
    set(PPC_LINKER "${PPC_TOOLS_DIR}/powerpc-none-elf-ld.exe")
    set(PPC_NM "${PPC_TOOLS_DIR}/powerpc-none-elf-nm.exe")
else()
    set(PPC_ASSEMBLER "${PPC_TOOLS_DIR}/powerpc-none-elf-as")
    set(PPC_LINKER "${PPC_TOOLS_DIR}/powerpc-none-elf-ld")
    set(PPC_NM "${PPC_TOOLS_DIR}/powerpc-none-elf-nm")
endif()

# Verify toolchain exists
if(NOT EXISTS "${PPC_ASSEMBLER}")
    message(FATAL_ERROR "PPC assembler not found at ${PPC_ASSEMBLER}")
endif()
if(NOT EXISTS "${PPC_LINKER}")
    message(FATAL_ERROR "PPC linker not found at ${PPC_LINKER}")
endif()

message(STATUS "PowerPC assembler: ${PPC_ASSEMBLER}")
message(STATUS "PowerPC linker: ${PPC_LINKER}")

# Convert Windows paths to Cygwin paths for binutils (Windows only)
function(_ppc_convert_to_cygwin_path WINDOWS_PATH OUTPUT_VAR)
    if(WIN32 AND WINDOWS_PATH MATCHES "^([A-Za-z]):")
        string(REGEX REPLACE "^([A-Za-z]):" "/cygdrive/\\1" CYGWIN_PATH "${WINDOWS_PATH}")
        string(SUBSTRING "${CYGWIN_PATH}" 10 1 DRIVE_LETTER)
        string(TOLOWER "${DRIVE_LETTER}" DRIVE_LOWER)
        string(REGEX REPLACE "^/cygdrive/[A-Za-z]" "/cygdrive/${DRIVE_LOWER}" CYGWIN_PATH "${CYGWIN_PATH}")
        string(REPLACE "\\" "/" CYGWIN_PATH "${CYGWIN_PATH}")
        set(${OUTPUT_VAR} "${CYGWIN_PATH}" PARENT_SCOPE)
    else()
        set(${OUTPUT_VAR} "${WINDOWS_PATH}" PARENT_SCOPE)
    endif()
endfunction()

# Add a PPC test binary from an assembly file.
# Assembles .s -> .o, links .o -> .bin, generates .o -> .map.
# Appends output files to PPC_TEST_BINS, PPC_TEST_MAPS, PPC_TEST_OBJS
# global properties.
function(ppc_add_test_binary ASM_FILE OBJ_DIR BIN_DIR)
    get_filename_component(BASE_NAME ${ASM_FILE} NAME_WE)

    set(OBJ_FILE ${OBJ_DIR}/${BASE_NAME}.o)
    set(BIN_FILE ${BIN_DIR}/${BASE_NAME}.bin)
    set(MAP_FILE ${BIN_DIR}/${BASE_NAME}.map)

    _ppc_convert_to_cygwin_path("${OBJ_FILE}" OBJ_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${BIN_FILE}" BIN_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${MAP_FILE}" MAP_FILE_CYGWIN)
    _ppc_convert_to_cygwin_path("${ASM_FILE}" ASM_FILE_CYGWIN)

    # Assemble .s to .o
    add_custom_command(
        OUTPUT ${OBJ_FILE}
        COMMAND ${PPC_ASSEMBLER}
                -a32 -be -mregnames -mpower7 -maltivec -mvsx -mvmx128 -R
                -o ${OBJ_FILE_CYGWIN}
                ${ASM_FILE_CYGWIN}
        DEPENDS ${ASM_FILE}
        COMMENT "Assembling ${BASE_NAME}.s"
        VERBATIM
    )

    # Link .o to .bin
    add_custom_command(
        OUTPUT ${BIN_FILE}
        COMMAND ${PPC_LINKER}
                -A powerpc:common32 -melf32ppc -EB -nostdlib
                --oformat=binary -Ttext=0x82010000 -e 0x82010000
                -o ${BIN_FILE_CYGWIN}
                ${OBJ_FILE_CYGWIN}
        DEPENDS ${OBJ_FILE}
        COMMENT "Linking ${BASE_NAME}.bin"
        VERBATIM
    )

    # Generate symbol map. nm runs under cmake -P (script mode, top of this file) so that its
    # stdout is a pipe rather than a file; see the note there.
    add_custom_command(
        OUTPUT ${MAP_FILE}
        COMMAND ${CMAKE_COMMAND}
                -DPPC_NM=${PPC_NM}
                -DPPC_NM_INPUT=${OBJ_FILE_CYGWIN}
                -DPPC_NM_OUTPUT=${MAP_FILE}
                -P ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
        DEPENDS ${OBJ_FILE} ${CMAKE_CURRENT_FUNCTION_LIST_FILE}
        COMMENT "Generating symbol map for ${BASE_NAME}"
        VERBATIM
    )

    set_property(GLOBAL APPEND PROPERTY PPC_TEST_BINS ${BIN_FILE})
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_MAPS ${MAP_FILE})
    set_property(GLOBAL APPEND PROPERTY PPC_TEST_OBJS ${OBJ_FILE})
endfunction()
