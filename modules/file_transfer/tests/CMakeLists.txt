# The file transfer module's tests: its pieces (sessions that use it are
# tests/session.cpp's). The SDK's test support is shared.
function(rcsdk_file_transfer_test name)
    add_executable(rcsdk_test_file_transfer_${name} ${name}.cpp)
    target_link_libraries(rcsdk_test_file_transfer_${name} PRIVATE rcsdk_file_transfer_internal)
    target_include_directories(rcsdk_test_file_transfer_${name} PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    add_test(NAME rcsdk.file_transfer.${name} COMMAND rcsdk_test_file_transfer_${name})
    set_tests_properties(rcsdk.file_transfer.${name} PROPERTIES TIMEOUT 60)
endfunction()

rcsdk_file_transfer_test(paths)
rcsdk_file_transfer_test(messages)
rcsdk_file_transfer_test(flow)
if(NOT EMSCRIPTEN) # A browser's files are the host's (target HOST); its own file system keeps no times.
    rcsdk_file_transfer_test(store)
endif()
