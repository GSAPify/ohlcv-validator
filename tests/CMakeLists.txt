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
    test_order_book.cpp
    test_book_builder.cpp
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

# Loopback-multicast integration test for the feed transport (#1b). Built but
# deliberately NOT gtest_discover'd, so it is NOT in the default ctest/CI run:
# multicast isn't guaranteed on CI runners. Run it locally:
#   ./build/tests/feed_multicast_test
add_executable(feed_multicast_test
    test_feed_multicast.cpp
)
target_include_directories(feed_multicast_test PRIVATE ${CMAKE_SOURCE_DIR}/src)
target_link_libraries(feed_multicast_test PRIVATE GTest::gtest_main Threads::Threads)

# Opt-in ctest registration for the above: off by default (the default
# `ctest --test-dir build` run is unaffected), turned on with
# -DENABLE_LOCAL_TESTS=ON, then selected with `ctest -L local`.
option(ENABLE_LOCAL_TESTS "Register opt-in, runner-unreliable tests (e.g. multicast) with ctest" OFF)
if(ENABLE_LOCAL_TESTS)
    add_test(NAME feed_multicast_test COMMAND feed_multicast_test)
    set_tests_properties(feed_multicast_test PROPERTIES LABELS "local")
endif()

# Optional libFuzzer harness for the Alpaca frame parser (see fuzz_parser.cpp
# for the rationale). Apple clang ships no libFuzzer runtime, so this needs
# Homebrew LLVM's clang++ as CMAKE_CXX_COMPILER. Off by default; not part of
# the default build or ctest run. Enable with -DFUZZER=ON, e.g.:
#   cmake -B build-fuzz -G Ninja -DFUZZER=ON \
#     -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"
#   cmake --build build-fuzz --target fuzz_parser
#   ./build-fuzz/tests/fuzz_parser -max_total_time=60
option(FUZZER "Build the libFuzzer harness for the parser (requires Homebrew LLVM clang++)" OFF)
if(FUZZER)
    add_executable(fuzz_parser
        fuzz_parser.cpp
        ${CMAKE_SOURCE_DIR}/src/ingest/parser.cpp
    )
    target_include_directories(fuzz_parser PRIVATE ${CMAKE_SOURCE_DIR}/src)
    target_link_libraries(fuzz_parser PRIVATE simdjson::simdjson)
    target_compile_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
    target_link_options(fuzz_parser PRIVATE -fsanitize=fuzzer,address,undefined)
endif()
