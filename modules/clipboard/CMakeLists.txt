# The clipboard (rcsdk/clipboard.h), "rcsdk.clipboard".
set(rcsdk_clipboard_sources
    src/content.cpp
    src/machine_half.cpp
    src/messages.cpp
    src/module.cpp
    src/pastes.cpp
    src/session_half.cpp
    src/system.cpp
    src/watcher.cpp)
if(IOS)
    list(APPEND rcsdk_clipboard_sources src/system_ios.mm)
    set_source_files_properties(src/system_ios.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set(rcsdk_clipboard_libraries "-framework Foundation" "-framework UIKit" "-framework CoreGraphics"
        "-framework ImageIO" "-framework UniformTypeIdentifiers")
elseif(APPLE)
    list(APPEND rcsdk_clipboard_sources src/system_macos.mm src/stand_in_macos.mm)
    set_source_files_properties(src/system_macos.mm src/stand_in_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set(rcsdk_clipboard_libraries "-framework Foundation" "-framework AppKit" "-framework CoreGraphics"
        "-framework ImageIO" "-framework CoreServices")
elseif(WIN32)
    list(APPEND rcsdk_clipboard_sources src/system_windows.cpp src/stand_in_windows.cpp)
    set(rcsdk_clipboard_libraries user32 ole32 shell32 advapi32 windowscodecs)
elseif(ANDROID)
    list(APPEND rcsdk_clipboard_sources src/system_android.cpp)
else()
    list(APPEND rcsdk_clipboard_sources src/system_none.cpp)
endif()

function(rcsdk_clipboard_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The library: the SDK's public headers and its own, nothing of src/.
add_library(rcsdk_clipboard ${rcsdk_clipboard_sources})
add_library(RemoteControl::Clipboard ALIAS rcsdk_clipboard)
set_target_properties(rcsdk_clipboard PROPERTIES
    EXPORT_NAME Clipboard
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    INSTALL_NAME_DIR "@rpath"
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rcsdk_clipboard
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
# Copies of files go through file transfer's public functions (rcsdk/file_transfer.h).
target_link_libraries(rcsdk_clipboard PUBLIC rcsdk_core rcsdk_file_transfer PRIVATE Threads::Threads
                      ${rcsdk_clipboard_libraries})
target_compile_definitions(rcsdk_clipboard PRIVATE RCSDK_BUILDING_CLIPBOARD)
rcsdk_clipboard_warnings(rcsdk_clipboard)
install(TARGETS rcsdk_clipboard EXPORT RemoteControlSDKTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/rcsdk DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The same, beside the SDK's internal objects, for tests that reach both.
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_library(rcsdk_clipboard_internal STATIC ${rcsdk_clipboard_sources})
    target_include_directories(rcsdk_clipboard_internal PUBLIC include src)
    target_link_libraries(rcsdk_clipboard_internal PUBLIC rcsdk_internal rcsdk_file_transfer_internal
                          PRIVATE ${rcsdk_clipboard_libraries})
    rcsdk_clipboard_warnings(rcsdk_clipboard_internal)
    add_subdirectory(tests)
endif()
