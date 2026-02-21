# SPDX-License-Identifier: Apache-2.0
# Loom tests

# Helper function for Yosys script tests (using slang frontend)
function(add_yosys_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME yosys_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${YOSYS_SLANG_PLUGIN}
            -m ${CMAKE_BINARY_DIR}/passes/scan_insert/scan_insert.so
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(yosys_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;yosys_slang_ext;scan_insert"
        TIMEOUT 60
    )
endfunction()

# Helper function for DPI bridge tests (uses slang frontend for DPI detection)
function(add_loom_instrument_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME yosys_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${YOSYS_SLANG_PLUGIN}
            -m ${CMAKE_BINARY_DIR}/passes/loom_instrument/loom_instrument.so
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(yosys_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;yosys_slang_ext;loom_instrument"
        TIMEOUT 60
    )
endfunction()

# Helper function for reset_extract tests (uses slang + reset_extract)
function(add_reset_extract_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME yosys_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${YOSYS_SLANG_PLUGIN}
            -m ${CMAKE_BINARY_DIR}/passes/reset_extract/reset_extract.so
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(yosys_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;yosys_slang_ext;reset_extract"
        TIMEOUT 60
    )
endfunction()

# Reset extract test
add_reset_extract_test(reset_extract)

# Register Yosys script tests
add_yosys_test(scan_basic)
add_yosys_test(scan_equiv)

# DPI bridge tests (uses loom_instrument pass)
add_loom_instrument_test(dpi_bridge)
add_loom_instrument_test(dpi_open_array)
add_loom_instrument_test(dpi_fixed_array)

# Flop enable tests (needs scan_insert + loom_instrument)
add_test(
    NAME yosys_loom_instrument
    COMMAND ${YOSYS_BIN}
        -m ${CMAKE_BINARY_DIR}/passes/scan_insert/scan_insert.so
        -m ${CMAKE_BINARY_DIR}/passes/loom_instrument/loom_instrument.so
        -s ${CMAKE_CURRENT_SOURCE_DIR}/loom_instrument/run.ys
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/loom_instrument
)
set_tests_properties(yosys_loom_instrument PROPERTIES
    DEPENDS "yosys_ext;scan_insert;loom_instrument"
    TIMEOUT 60
)

# Helper function for emu_top tests (uses slang + all loom passes)
function(add_emu_top_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME yosys_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${YOSYS_SLANG_PLUGIN}
            -m ${CMAKE_BINARY_DIR}/passes/reset_extract/reset_extract.so
            -m ${CMAKE_BINARY_DIR}/passes/scan_insert/scan_insert.so
            -m ${CMAKE_BINARY_DIR}/passes/loom_instrument/loom_instrument.so
            -m ${CMAKE_BINARY_DIR}/passes/emu_top/emu_top.so
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(yosys_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top"
        TIMEOUT 60
    )
endfunction()

# Helper function for slang-only tests (no loom passes)
function(add_yosys_slang_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME yosys_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${YOSYS_SLANG_PLUGIN}
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(yosys_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;yosys_slang_ext"
        TIMEOUT 60
    )
endfunction()

# FSM extraction test (uses slang frontend)
add_yosys_slang_test(fsm_extract)

# FSM extraction equivalence test (Verilator --timing)
add_test(NAME fsm_extract_equiv
    COMMAND make sim
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/fsm_extract
)
set_tests_properties(fsm_extract_equiv PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "VERILATOR=${VERILATOR_BIN}"
)

# emu_top tests
add_emu_top_test(emu_top)
add_emu_top_test(shell_regaccess)

# End-to-end DPI open array test using loomc/loomx
add_test(NAME e2e_dpi_open_array
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/dpi_open_array
)
set_tests_properties(e2e_dpi_open_array PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)

# End-to-end DPI fixed-size array test using loomc/loomx
add_test(NAME e2e_dpi_fixed_array
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/dpi_fixed_array
)
set_tests_properties(e2e_dpi_fixed_array PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)

# End-to-end DPI test using loomc/loomx
add_test(NAME e2e_dpi_test
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/e2e
)
set_tests_properties(e2e_dpi_test PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)

# Helper function for mem_shadow E2E tests (transform + simulate)
function(add_mem_shadow_e2e_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME mem_shadow_${TEST_NAME}
        COMMAND make sim
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(mem_shadow_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;yosys_slang_ext;mem_shadow;verilator_ext"
        TIMEOUT 120
        ENVIRONMENT "YOSYS=${YOSYS_BIN};YOSYS_SLANG_PLUGIN=${YOSYS_SLANG_PLUGIN};VERILATOR=${VERILATOR_BIN}"
    )
endfunction()

# mem_shadow E2E test
add_mem_shadow_e2e_test(mem_shadow_e2e)

# Helper function for mem_shadow + emu_top integration tests
function(add_mem_shadow_emu_top_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME mem_shadow_emu_top_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${YOSYS_SLANG_PLUGIN}
            -m ${CMAKE_BINARY_DIR}/passes/reset_extract/reset_extract.so
            -m ${CMAKE_BINARY_DIR}/passes/scan_insert/scan_insert.so
            -m ${CMAKE_BINARY_DIR}/passes/loom_instrument/loom_instrument.so
            -m ${CMAKE_BINARY_DIR}/passes/mem_shadow/mem_shadow.so
            -m ${CMAKE_BINARY_DIR}/passes/emu_top/emu_top.so
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(mem_shadow_emu_top_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;mem_shadow;emu_top"
        TIMEOUT 60
    )
endfunction()

# mem_shadow + emu_top integration test
add_mem_shadow_emu_top_test(mem_shadow_emu_top)

# End-to-end scan dump test using loomc/loomx
add_test(NAME e2e_scan_dump
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/e2e_scan_dump
)
set_tests_properties(e2e_scan_dump PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)

# End-to-end initial DPI test using loomc/loomx
add_test(NAME e2e_initial_dpi
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/initial_dpi
)
set_tests_properties(e2e_initial_dpi PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)

# End-to-end initial DPI error test (should fail with error message)
add_test(NAME e2e_initial_dpi_error
    COMMAND ${CMAKE_COMMAND} -E env
        LOOM_HOME=${CMAKE_SOURCE_DIR}
        VERILATOR=${VERILATOR_BIN}
        ${CMAKE_SOURCE_DIR}/bin/loomc -top initial_dpi_error_test
        ${CMAKE_CURRENT_SOURCE_DIR}/initial_dpi/initial_dpi_error_test.sv
        -work ${CMAKE_CURRENT_BINARY_DIR}/initial_dpi_error
)
set_tests_properties(e2e_initial_dpi_error PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc"
    TIMEOUT 30
    WILL_FAIL TRUE
)

# End-to-end shell register access test using loomc/loomx
add_test(NAME e2e_shell_regaccess
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/shell_regaccess
)
set_tests_properties(e2e_shell_regaccess PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)

# End-to-end AXI-Lite completeness test — every transaction must terminate
add_test(NAME e2e_axil_completeness
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/axil_completeness
)
set_tests_properties(e2e_axil_completeness PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 30
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)

# End-to-end multisim read/write test using loomc/loomx
add_test(NAME e2e_multisim_read_write
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/multisim_read_write
)
set_tests_properties(e2e_multisim_read_write PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)

# End-to-end mem_shadow test (memory preload, DPI, dump, reset, $readmemh, $readmemb, init)
add_test(NAME e2e_mem_shadow
    COMMAND make test
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/e2e_mem_shadow
)
set_tests_properties(e2e_mem_shadow PROPERTIES
    DEPENDS "yosys_ext;yosys_slang_ext;reset_extract;scan_insert;loom_instrument;mem_shadow;emu_top;loomc;loomx;verilator_ext"
    TIMEOUT 120
    ENVIRONMENT "LOOM_HOME=${CMAKE_SOURCE_DIR};VERILATOR=${VERILATOR_BIN}"
)
