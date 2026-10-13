# The supervisor's tests, through its public interface. The SDK's test support
# is shared.
function(rcsdk_supervisor_test name)
    cmake_parse_arguments(TEST "" "TIMEOUT" "SOURCES" ${ARGN})
    add_executable(rcsdk_test_supervisor_${name} ${TEST_SOURCES})
    target_link_libraries(rcsdk_test_supervisor_${name} PRIVATE RemoteControl::Supervisor)
    target_include_directories(rcsdk_test_supervisor_${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    add_test(NAME rcsdk.supervisor.${name} COMMAND rcsdk_test_supervisor_${name})
    if(TEST_TIMEOUT)
        set_tests_properties(rcsdk.supervisor.${name} PROPERTIES TIMEOUT ${TEST_TIMEOUT})
    endif()
endfunction()

rcsdk_supervisor_test(api SOURCES api.c)
set_target_properties(rcsdk_test_supervisor_api PROPERTIES LINKER_LANGUAGE CXX)
rcsdk_supervisor_test(os_sessions SOURCES os_sessions.cpp)
# The supervisor starts helpers only as LocalSystem: anywhere else it skips
# (77). Run it as a SYSTEM scheduled task to test it.
if(WIN32)
    rcsdk_supervisor_test(run SOURCES supervisor.cpp TIMEOUT 120)
    set_tests_properties(rcsdk.supervisor.run PROPERTIES SKIP_RETURN_CODE 77)
endif()
