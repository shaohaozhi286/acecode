find_package(GTest CONFIG REQUIRED)

file(GLOB_RECURSE ACECODE_TEST_SOURCES CONFIGURE_DEPENDS
    ${CMAKE_CURRENT_SOURCE_DIR}/*_test.cpp
)

add_executable(acecode_unit_tests ${ACECODE_TEST_SOURCES})

target_link_libraries(acecode_unit_tests PRIVATE
    acecode_testable
    GTest::gtest
    GTest::gtest_main
)

target_include_directories(acecode_unit_tests PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

include(GoogleTest)
gtest_discover_tests(acecode_unit_tests
    PROPERTIES LABELS "unit"
    DISCOVERY_MODE PRE_TEST
)
