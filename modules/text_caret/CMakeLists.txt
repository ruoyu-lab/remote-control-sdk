# Where text typed now would go on the shared machine (rcsdk/text_caret.h),
# "rcsdk.text-caret".
set(rcsdk_text_caret_sources src/module.cpp src/reader.cpp)
if(APPLE AND NOT IOS)
    list(APPEND rcsdk_text_caret_sources src/reader_macos.mm)
    set_source_files_properties(src/reader_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set(rcsdk_text_caret_libraries "-framework ApplicationServices" "-framework CoreFoundation")
elseif(WIN32)
    list(APPEND rcsdk_text_caret_sources src/reader_windows.cpp)
    set(rcsdk_text_caret_libraries user32 ole32 oleaut32 uiautomationcore)
else()
    list(APPEND rcsdk_text_caret_sources src/reader_none.cpp)
endif()

function(rcsdk_text_caret_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The library: the SDK's public headers and its own, nothing of src/.
add_library(rcsdk_text_caret ${rcsdk_text_caret_sources})
add_library(RemoteControl::TextCaret ALIAS rcsdk_text_caret)
set_target_properties(rcsdk_text_caret PROPERTIES
    EXPORT_NAME TextCaret
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    INSTALL_NAME_DIR "@rpath"
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rcsdk_text_caret
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(rcsdk_text_caret PUBLIC rcsdk_core PRIVATE Threads::Threads ${rcsdk_text_caret_libraries})
target_compile_definitions(rcsdk_text_caret PRIVATE RCSDK_BUILDING_TEXT_CARET)
rcsdk_text_caret_warnings(rcsdk_text_caret)
install(TARGETS rcsdk_text_caret EXPORT RemoteControlSDKTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/rcsdk DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The same, beside the SDK's internal objects, for tests that reach both.
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_library(rcsdk_text_caret_internal STATIC ${rcsdk_text_caret_sources})
    target_include_directories(rcsdk_text_caret_internal PUBLIC include src)
    target_link_libraries(rcsdk_text_caret_internal PUBLIC rcsdk_internal PRIVATE ${rcsdk_text_caret_libraries})
    rcsdk_text_caret_warnings(rcsdk_text_caret_internal)
    add_subdirectory(tests)
endif()
