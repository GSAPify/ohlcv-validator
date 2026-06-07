cmake_minimum_required(VERSION 3.20)

# Use Boost's own CMake config rather than the deprecated FindBoost module.
if(POLICY CMP0167)
    cmake_policy(SET CMP0167 NEW)
endif()

project(ohlcv-validator
    VERSION 0.0.1
    DESCRIPTION "Low-latency OHLCV market data validator"
    LANGUAGES CXX
)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

add_compile_options(-Wall -Wextra -Wpedantic -Wshadow -Wconversion -Werror)
add_compile_options($<$<CONFIG:Release>:-O3>)
add_compile_options($<$<CONFIG:Release>:-march=native>)

# Optional sanitizer build, e.g. -DSANITIZER="address;undefined" or -DSANITIZER=thread.
# Leaves the default (unset) build completely untouched. address and thread are
# mutually exclusive; combine address with undefined for the fullest memory/UB run.
set(SANITIZER "" CACHE STRING "Sanitizers to build with (address;undefined;thread)")
if(SANITIZER)
    foreach(san IN LISTS SANITIZER)
        add_compile_options(-fsanitize=${san})
        add_link_options(-fsanitize=${san})
    endforeach()
    add_compile_options(-fno-omit-frame-pointer -g)
endif()

# Homebrew prefix discovery so find_package picks up keg-only and headers-only libs.
if(APPLE)
    execute_process(
        COMMAND brew --prefix
        OUTPUT_VARIABLE HOMEBREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    list(PREPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")
endif()

# Homebrew installs OpenSSL keg-only; point CMake at it explicitly.
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    execute_process(
        COMMAND brew --prefix openssl
        OUTPUT_VARIABLE OPENSSL_ROOT_DIR
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
endif()

find_package(Boost REQUIRED)
find_package(spdlog REQUIRED)
find_package(simdjson REQUIRED)
find_package(nlohmann_json REQUIRED)
find_package(OpenSSL REQUIRED)
find_package(Threads REQUIRED)

add_executable(ohlcv_validator
    src/main.cpp
)
target_include_directories(ohlcv_validator PRIVATE src)
target_link_libraries(ohlcv_validator PRIVATE
    Boost::headers
    spdlog::spdlog
    simdjson::simdjson
    nlohmann_json::nlohmann_json
)

add_executable(alpaca_ingest
    src/bin/alpaca_ingest.cpp
    src/ingest/alpaca_client.cpp
    src/ingest/parser.cpp
)
target_include_directories(alpaca_ingest PRIVATE src)
target_link_libraries(alpaca_ingest PRIVATE
    Boost::headers
    OpenSSL::SSL
    OpenSSL::Crypto
    spdlog::spdlog
    simdjson::simdjson
    nlohmann_json::nlohmann_json
    Threads::Threads
)

# Synthetic binary dataset generator for the replay benchmark.
add_executable(gen_dataset
    src/bin/gen_dataset.cpp
)
target_include_directories(gen_dataset PRIVATE src)

# Headline artifact: replay a binary dataset through the validator and report
# a per-record latency histogram.
add_executable(replay_bench
    src/bin/replay_bench.cpp
    src/validate/validator.cpp
)
target_include_directories(replay_bench PRIVATE src)

# Multi-threaded replay benchmark: shard by symbol across cores.
add_executable(replay_bench_mt
    src/bin/replay_bench_mt.cpp
    src/validate/validator.cpp
)
target_include_directories(replay_bench_mt PRIVATE src)
target_link_libraries(replay_bench_mt PRIVATE Threads::Threads)

# Per-record latency histogram via rdtscp. Builds on any arch (non-x86 uses a
# coarse fallback timer); only meaningful on an x86 host with an invariant TSC.
add_executable(replay_bench_rdtsc
    src/bin/replay_bench_rdtsc.cpp
    src/validate/validator.cpp
)
target_include_directories(replay_bench_rdtsc PRIVATE src)

# Producer/consumer pipeline over the lock-free SPSC ring: throughput
# (padded vs packed cursors) + handoff latency. Linux pins the two threads.
add_executable(replay_bench_spsc
    src/bin/replay_bench_spsc.cpp
    src/validate/validator.cpp
)
target_include_directories(replay_bench_spsc PRIVATE src)
target_link_libraries(replay_bench_spsc PRIVATE Threads::Threads)

enable_testing()
add_subdirectory(tests)
