# scan_insert Yosys pass
# Inserts scan chains into the design for state capture/restore

add_library(scan_insert MODULE scan_insert.cc)
target_link_libraries(scan_insert PRIVATE libyosys)
set_target_properties(scan_insert PROPERTIES
    PREFIX ""
    SUFFIX ".so"
)

# Unit tests
# add_executable(scan_insert_test scan_insert_test.cc)
# target_link_libraries(scan_insert_test PRIVATE libyosys GTest::gtest_main)
# gtest_discover_tests(scan_insert_test)
