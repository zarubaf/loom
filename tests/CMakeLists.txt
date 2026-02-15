# SPDX-License-Identifier: Apache-2.0
# Loom tests

# Helper function for Yosys script tests (using read_verilog)
function(add_yosys_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME yosys_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${CMAKE_BINARY_DIR}/passes/scan_insert/scan_insert.so
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(yosys_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;scan_insert"
        TIMEOUT 60
    )
endfunction()

# Helper function for Yosys tests using slang frontend
function(add_yosys_slang_test TEST_NAME)
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
function(add_dpi_bridge_test TEST_NAME)
    set(TEST_DIR ${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME})
    add_test(
        NAME yosys_${TEST_NAME}
        COMMAND ${YOSYS_BIN}
            -m ${YOSYS_SLANG_PLUGIN}
            -m ${CMAKE_BINARY_DIR}/passes/dpi_bridge/dpi_bridge.so
            -s ${TEST_DIR}/run.ys
        WORKING_DIRECTORY ${TEST_DIR}
    )
    set_tests_properties(yosys_${TEST_NAME} PROPERTIES
        DEPENDS "yosys_ext;yosys_slang_ext;dpi_bridge"
        TIMEOUT 60
    )
endfunction()

# Register Yosys script tests
add_yosys_test(scan_basic)
add_yosys_test(scan_equiv)

# DPI bridge tests
add_dpi_bridge_test(dpi_bridge)

# End-to-end tests using slang frontend
add_yosys_slang_test(e2e)
