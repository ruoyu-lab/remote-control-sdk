function(run_checked)
    execute_process(COMMAND ${ARGV} RESULT_VARIABLE result
        OUTPUT_VARIABLE output ERROR_VARIABLE error)
    if(NOT result EQUAL 0)
        message(FATAL_ERROR "Consumer verification failed (${result})\n${output}\n${error}")
    endif()
endfunction()

set(prefix "${SDK_BINARY_DIR}/consumer-check/universal-install")
set(consumer_build "${SDK_BINARY_DIR}/consumer-check/build")
# Fresh prefix so stale exports from older builds cannot hide packaging regressions.
file(REMOVE_RECURSE "${prefix}")
run_checked("${CMAKE_COMMAND}" --install "${SDK_BINARY_DIR}"
    --prefix "${prefix}" --config "${SDK_CONFIG}")
# The SDK is framework-neutral: the package must not depend on Qt.
file(GLOB_RECURSE package_files "${prefix}/*RemoteControlSDK*.cmake")
foreach(package_file IN LISTS package_files)
    file(READ "${package_file}" package_contents)
    if(package_contents MATCHES "Qt6")
        message(FATAL_ERROR "The SDK package references Qt: ${package_file}")
    endif()
endforeach()
run_checked("${CMAKE_COMMAND}" -S "${CONSUMER_SOURCE_DIR}" -B "${consumer_build}"
    -G "${SDK_GENERATOR}"
    "-DCMAKE_PREFIX_PATH=${prefix}"
    "-DCMAKE_CXX_COMPILER=${SDK_CXX_COMPILER}"
    "-DCMAKE_BUILD_TYPE=${SDK_CONFIG}"
    "-DCMAKE_DISABLE_FIND_PACKAGE_Qt6=ON")
run_checked("${CMAKE_COMMAND}" --build "${consumer_build}" --config "${SDK_CONFIG}")
if(WIN32)
    set(executable "${consumer_build}/consumer.exe")
    if(EXISTS "${consumer_build}/${SDK_CONFIG}/consumer.exe")
        set(executable "${consumer_build}/${SDK_CONFIG}/consumer.exe")
    endif()
    # The installed DLL; a PATH passed as an argument would be split at its semicolons.
    set(ENV{PATH} "${prefix}/bin;$ENV{PATH}")
    run_checked("${executable}")
else()
    run_checked("${consumer_build}/consumer")
endif()
message(STATUS "The installed SDK package builds and runs in a separate project")
