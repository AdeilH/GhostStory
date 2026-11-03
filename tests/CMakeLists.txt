# addu94 — tests
set(A94_TEST_LIBS addu94_core)

foreach(t spsc_queue mpmc_queue logger json midi music_tools transport recorder smf arp toml project synth coverage arrangement)
    add_executable(test_${t} test_${t}.cpp)
    target_include_directories(test_${t} SYSTEM PRIVATE ${CMAKE_SOURCE_DIR}/third_party)
    target_link_libraries(test_${t} PRIVATE ${A94_TEST_LIBS})
    addu94_apply_warnings(test_${t})
    addu94_apply_sanitizers(test_${t})
    add_test(NAME ${t} COMMAND test_${t})
    set_tests_properties(${t} PROPERTIES TIMEOUT 300)
endforeach()
