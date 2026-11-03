# addu94 — top-level CMakeLists
cmake_minimum_required(VERSION 3.28)

project(addu94
    VERSION 0.1.0
    DESCRIPTION "Self-healing, self-improving song builder for the Akai MPK Mini on Linux"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
# We use #include only (no named modules) — skip CMake's C++26 module
# dependency scanning (simpler flags, faster builds, clang-tidy friendly).
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(compiler_flags)
include(sanitizers)

option(ADDU94_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(ADDU94_BUILD_TESTS "Build test executables" ON)
option(ADDU94_SANITIZE "Sanitizers for test targets: off|asan|tsan" "off")

find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
include(FindPkgConfig)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)

# First-party core (grows per milestone).
add_library(addu94_core STATIC
    src/log/logger.cpp
    src/core/toml_min.cpp
    src/midi/alsa_client.cpp
    src/audio/pipewire_backend.cpp
    src/audio/backends.cpp
    src/audio/engine.cpp
    src/audio/synth.cpp
    src/app/play_engine.cpp
    src/music/transport.cpp
    src/mapping/mapping.cpp
    src/project/smf.cpp
    src/project/document.cpp
    src/project/journal.cpp
    src/ops/bench.cpp
    src/device/manifest.cpp
    src/device/probe.cpp
)
target_include_directories(addu94_core PUBLIC src)
target_include_directories(addu94_core SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(addu94_core PUBLIC Threads::Threads ALSA::ALSA PkgConfig::PIPEWIRE)
target_compile_features(addu94_core PUBLIC cxx_std_26)
addu94_apply_warnings(addu94_core)
# Sanitize the core lib too when enabled: uninstrumented code inside an
# instrumented binary breaks TSan happens-before tracking (phantom races).
addu94_apply_sanitizers(addu94_core)

# Main binary.
add_executable(addu94 src/app/main.cpp)
target_link_libraries(addu94 PRIVATE addu94_core)
addu94_apply_warnings(addu94)
addu94_apply_sanitizers(addu94)  # at least link the runtime (core lib is instrumented)

if(ADDU94_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS addu94 RUNTIME DESTINATION bin)
