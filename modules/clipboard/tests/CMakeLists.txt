# The clipboard module's tests: its pieces, and sessions that use it (on
# clipboards of the tests'). The SDK's test support is shared.
function(rcsdk_clipboard_test name)
    cmake_parse_arguments(TEST "PORTS" "TIMEOUT" "SOURCES" ${ARGN})
    add_executable(rcsdk_test_clipboard_${name} ${TEST_SOURCES})
    target_link_libraries(rcsdk_test_clipboard_${name} PRIVATE rcsdk_clipboard_internal)
    target_include_directories(rcsdk_test_clipboard_${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests"
                                                                     "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME rcsdk.clipboard.${name} COMMAND rcsdk_test_clipboard_${name})
    if(TEST_TIMEOUT)
        set_tests_properties(rcsdk.clipboard.${name} PROPERTIES TIMEOUT ${TEST_TIMEOUT})
    endif()
    if(TEST_PORTS)
        set_tests_properties(rcsdk.clipboard.${name} PROPERTIES RESOURCE_LOCK loopback_ports)
    endif()
endfunction()

rcsdk_clipboard_test(content SOURCES content.cpp)
# macOS: files on a pasteboard of the test's own, never the general one.
if(APPLE AND NOT IOS)
    rcsdk_clipboard_test(pasteboard_macos SOURCES pasteboard_macos.mm TIMEOUT 60)
    set_source_files_properties(pasteboard_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rcsdk_test_clipboard_pasteboard_macos PRIVATE "-framework AppKit" "-framework ImageIO")
endif()
# Windows: the other end's files as virtual files (the data object, no clipboard).
if(WIN32)
    rcsdk_clipboard_test(stand_in_windows SOURCES stand_in_windows.cpp TIMEOUT 120)
    target_link_libraries(rcsdk_test_clipboard_stand_in_windows PRIVATE ole32 shell32)
endif()
