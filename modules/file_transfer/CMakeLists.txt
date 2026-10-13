# File transfer (rcsdk/file_transfer.h), "rcsdk.file-transfer".
set(rcsdk_file_transfer_sources
    src/common.cpp
    src/files.cpp
    src/machine_half.cpp
    src/messages.cpp
    src/module.cpp
    src/paths.cpp
    src/record.cpp
    src/session_half.cpp
    src/store.cpp)
if(WIN32)
    list(APPEND rcsdk_file_transfer_sources src/files_windows.cpp)
else()
    list(APPEND rcsdk_file_transfer_sources src/files_posix.cpp)
endif()

function(rcsdk_file_transfer_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The library: the SDK's public headers and its own, nothing of src/.
add_library(rcsdk_file_transfer ${rcsdk_file_transfer_sources})
add_library(RemoteControl::FileTransfer ALIAS rcsdk_file_transfer)
set_target_properties(rcsdk_file_transfer PROPERTIES
    EXPORT_NAME FileTransfer
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    INSTALL_NAME_DIR "@rpath"
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rcsdk_file_transfer
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(rcsdk_file_transfer PUBLIC rcsdk_core PRIVATE Threads::Threads)
target_compile_definitions(rcsdk_file_transfer PRIVATE RCSDK_BUILDING_FILE_TRANSFER)
rcsdk_file_transfer_warnings(rcsdk_file_transfer)
install(TARGETS rcsdk_file_transfer EXPORT RemoteControlSDKTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/rcsdk DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The same, beside the SDK's internal objects, for tests that reach both.
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_library(rcsdk_file_transfer_internal STATIC ${rcsdk_file_transfer_sources})
    target_include_directories(rcsdk_file_transfer_internal PUBLIC include src)
    target_link_libraries(rcsdk_file_transfer_internal PUBLIC rcsdk_internal)
    rcsdk_file_transfer_warnings(rcsdk_file_transfer_internal)
    add_subdirectory(tests)
endif()
