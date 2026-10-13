# The text caret module's tests: its pieces (sessions that use it are
# tests/session.cpp's). The SDK's test support is shared.
set(rcsdk_text_caret_test_sources reader.cpp)
if(WIN32)
    list(APPEND rcsdk_text_caret_test_sources "${PROJECT_SOURCE_DIR}/tests/support/test_window_win.cpp")
endif()
add_executable(rcsdk_test_text_caret_reader ${rcsdk_text_caret_test_sources})
target_link_libraries(rcsdk_test_text_caret_reader PRIVATE rcsdk_text_caret_internal)
target_include_directories(rcsdk_test_text_caret_reader PRIVATE "${PROJECT_SOURCE_DIR}/tests")
add_test(NAME rcsdk.text_caret.reader COMMAND rcsdk_test_text_caret_reader)
