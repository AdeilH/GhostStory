# ghost_story — tests
set(GS_TEST_LIBS ghost_story_core)

foreach(t spsc_queue mpmc_queue logger json midi music_tools retrigger transport recorder clip_player smf arp toml project synth coverage arrangement mapping watchdog sampler render fx sfz capture)
    add_executable(test_${t} test_${t}.cpp)
    target_include_directories(test_${t} SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(test_${t} PRIVATE ${GS_TEST_LIBS})
    ghost_story_apply_warnings(test_${t})
    ghost_story_apply_sanitizers(test_${t})
    add_test(NAME ${t} COMMAND test_${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 300)
endforeach()
# The sampler suite renders seconds of real-time audio per test; under TSan
# the per-sample loops slow down ~50x and it was the first to hit the 300 s
# ceiling (bank-B pad-mapping tests pushed it over). Give it headroom.
set_tests_properties(sampler PROPERTIES TIMEOUT 900)
