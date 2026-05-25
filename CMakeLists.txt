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

# Homebrew prefix discovery so find_package picks up keg-only and headers-only libs.
if(APPLE)
    execute_process(
        COMMAND brew --prefix
        OUTPUT_VARIABLE HOMEBREW_PREFIX
        OUTPUT_STRIP_TRAILING_WHITESPACE
    )
    list(PREPEND CMAKE_PREFIX_PATH "${HOMEBREW_PREFIX}")
endif()

find_package(Boost REQUIRED)
find_package(spdlog REQUIRED)
find_package(simdjson REQUIRED)
find_package(nlohmann_json REQUIRED)

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

enable_testing()
add_subdirectory(tests)
