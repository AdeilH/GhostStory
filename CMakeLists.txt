# ghost_story — top-level CMakeLists
cmake_minimum_required(VERSION 3.28)

project(ghost_story
    VERSION 0.2.0
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

option(GHOST_STORY_WARNINGS_AS_ERRORS "Treat warnings as errors" ON)
option(GHOST_STORY_BUILD_TESTS "Build test executables" ON)
option(GHOST_STORY_SANITIZE "Sanitizers for test targets: off|asan|tsan" "off")
option(GHOST_STORY_UI "Build native Qt6 GUI" ON)

find_package(Threads REQUIRED)
find_package(ALSA REQUIRED)
include(FindPkgConfig)
pkg_check_modules(PIPEWIRE REQUIRED IMPORTED_TARGET libpipewire-0.3)
# JACK fallback backend (spec §3): optional on purpose — a machine without
# the jack dev package still builds; the engine then falls through to ALSA.
# NOT REQUIRED: guard every JACK artifact on JACK_FOUND.
pkg_check_modules(JACK IMPORTED_TARGET jack)

# JACK backend source is only compiled when the jack headers exist.
set(GHOST_STORY_JACK_SOURCES "")
if(JACK_FOUND)
    list(APPEND GHOST_STORY_JACK_SOURCES src/audio/jack_backend.cpp)
endif()

# First-party core (grows per milestone).
add_library(ghost_story_core STATIC
    src/log/logger.cpp
    src/core/toml_min.cpp
    src/midi/alsa_client.cpp
    src/audio/pipewire_backend.cpp
    ${GHOST_STORY_JACK_SOURCES}
    src/audio/alsa_backend.cpp
    src/audio/backends.cpp
    src/audio/engine.cpp
    src/audio/synth.cpp
    src/audio/wav_file.cpp
    src/audio/sfz.cpp
    src/audio/sampler.cpp
    src/audio/drum_kit.cpp
    src/audio/fx.cpp
    src/audio/render_engine.cpp
    src/project/export_wav.cpp
    src/app/play_engine.cpp
    src/app/calibrate.cpp
    src/music/transport.cpp
    src/mapping/mapping.cpp
    src/mapping/audit.cpp
    src/project/smf.cpp
    src/project/document.cpp
    src/project/journal.cpp
    src/ops/bench.cpp
    src/ops/watchdog.cpp
    src/ops/doctor.cpp
    src/ops/crash_handler.cpp
    src/ops/telemetry.cpp
    src/device/manifest.cpp
    src/device/probe.cpp
)
target_include_directories(ghost_story_core PUBLIC src)
target_include_directories(ghost_story_core SYSTEM PUBLIC ${CMAKE_SOURCE_DIR}/third_party)
target_link_libraries(ghost_story_core PUBLIC Threads::Threads ALSA::ALSA PkgConfig::PIPEWIRE)
if(JACK_FOUND)
    # Link libjack and tell backends.cpp to include JackBackend in the
    # factory (PRIVATE: only core sources reference it).
    target_link_libraries(ghost_story_core PUBLIC PkgConfig::JACK)
    target_compile_definitions(ghost_story_core PRIVATE GHOST_STORY_HAVE_JACK)
endif()
target_compile_features(ghost_story_core PUBLIC cxx_std_26)
ghost_story_apply_warnings(ghost_story_core)
# Sanitize the core lib too when enabled: uninstrumented code inside an
# instrumented binary breaks TSan happens-before tracking (phantom races).
ghost_story_apply_sanitizers(ghost_story_core)

# Main binary.
add_executable(ghost_story src/app/main.cpp)
target_link_libraries(ghost_story PRIVATE ghost_story_core)
ghost_story_apply_warnings(ghost_story)
ghost_story_apply_sanitizers(ghost_story)  # at least link the runtime (core lib is instrumented)

# Native Qt6 Widgets GUI (spec §9): dark, dense, hardware-first. Moc is not
# needed (plain widgets + lambdas, no Q_OBJECT), but AUTOMOC stays on so
# future signal-based widgets drop in without CMake churn.
if(GHOST_STORY_UI)
    find_package(Qt6 COMPONENTS Widgets REQUIRED)
    set(CMAKE_AUTOMOC ON)
    add_library(ghost_story_ui STATIC
        src/ui/main_window.cpp
        src/ui/controller_mirror.cpp
        src/ui/fx_panel.cpp
        src/ui/arrangement_view.cpp
        src/ui/track_strip.cpp
        src/ui/health_strip.cpp
        src/ui/mic_panel.cpp
        src/ui/app_gui.cpp
    )
    target_include_directories(ghost_story_ui PUBLIC src)
    target_link_libraries(ghost_story_ui PUBLIC Qt6::Widgets ghost_story_core)
    target_compile_features(ghost_story_ui PUBLIC cxx_std_26)
    ghost_story_apply_warnings(ghost_story_ui)
    target_compile_definitions(ghost_story_ui PRIVATE GHOST_STORY_UI_ENABLED)
    target_link_libraries(ghost_story PRIVATE ghost_story_ui)
    target_compile_definitions(ghost_story PRIVATE GHOST_STORY_UI_ENABLED)
endif()

if(GHOST_STORY_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

install(TARGETS ghost_story RUNTIME DESTINATION bin)
install(FILES docs/ghost_story.1 DESTINATION share/man/man1)
install(FILES packaging/ghost_story.desktop DESTINATION share/applications)

# MIDI fuzz harness (spec §7.5) — dev/CI tool.
add_executable(fuzz_midi tools/fuzz_midi/fuzz_midi.cpp)
target_link_libraries(fuzz_midi PRIVATE ghost_story_core)
ghost_story_apply_warnings(fuzz_midi)
ghost_story_apply_sanitizers(fuzz_midi)
