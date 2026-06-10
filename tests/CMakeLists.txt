find_package(GTest REQUIRED)
include(GoogleTest)

add_executable(unit_tests
    test_timing.cpp
    test_timestamp.cpp
    test_parser.cpp
    test_validator.cpp
    test_histogram.cpp
    test_alloc_guard.cpp
    test_spsc_ring.cpp
    test_live_validation.cpp
    ${CMAKE_SOURCE_DIR}/src/ingest/parser.cpp
    ${CMAKE_SOURCE_DIR}/src/validate/validator.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    simdjson::simdjson
    Threads::Threads
)

gtest_discover_tests(unit_tests)
