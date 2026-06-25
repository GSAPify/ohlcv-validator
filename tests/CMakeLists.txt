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
    test_arbitrator.cpp
    test_live_validation.cpp
    test_violation_log.cpp
    test_backoff.cpp
    test_capture_writer.cpp
    ${CMAKE_SOURCE_DIR}/src/ingest/parser.cpp
    ${CMAKE_SOURCE_DIR}/src/validate/validator.cpp
)
target_include_directories(unit_tests PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(unit_tests PRIVATE
    GTest::gtest_main
    simdjson::simdjson
    nlohmann_json::nlohmann_json
    Threads::Threads
)

gtest_discover_tests(unit_tests)

# Integration test for the WS client's reconnect — links the real client + its
# Boost/OpenSSL deps + a local TLS server, so it's kept out of the fast unit_tests.
add_executable(reconnect_test
    test_reconnect.cpp
    ${CMAKE_SOURCE_DIR}/src/ingest/alpaca_client.cpp
)
target_include_directories(reconnect_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(reconnect_test PRIVATE
    GTest::gtest_main
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    nlohmann_json::nlohmann_json
    spdlog::spdlog
    Threads::Threads
)
gtest_discover_tests(reconnect_test)
