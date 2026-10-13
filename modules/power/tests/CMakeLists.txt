# The power module's tests: its pieces (sessions that use it are
# tests/session.cpp's). The SDK's test support is shared. None restarts,
# shuts down, sleeps or signs out anything: actions go to a test's machine.
function(rcsdk_power_test name)
    add_executable(rcsdk_test_power_${name} ${name}.cpp)
    target_link_libraries(rcsdk_test_power_${name} PRIVATE rcsdk_power_internal)
    target_include_directories(rcsdk_test_power_${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    add_test(NAME rcsdk.power.${name} COMMAND rcsdk_test_power_${name})
    set_tests_properties(rcsdk.power.${name} PROPERTIES TIMEOUT 60)
endfunction()

rcsdk_power_test(messages)
if(NOT EMSCRIPTEN) # A browser sends no datagrams of its own.
    rcsdk_power_test(wake)
endif()
