find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(unit_tests
    test_timing.cpp
    test_timestamp.cpp
    test_parser.cpp
    ${CMAKE_SOURCE_DIR}/src/ingest/parser.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    simdjson::simdjson
)

gtest_discover_tests(unit_tests)
