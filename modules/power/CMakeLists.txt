# The machine's power (rcsdk/power.h), "rcsdk.power": keeping it awake, the
# pieces of waking it, and a viewer's actions on it.
set(rcsdk_power_sources src/module.cpp src/request.cpp src/messages.cpp src/system.cpp)
if(IOS)
    list(APPEND rcsdk_power_sources src/request_ios.mm src/system_none.cpp)
    set_source_files_properties(src/request_ios.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set(rcsdk_power_libraries "-framework UIKit" "-framework Foundation")
elseif(APPLE)
    list(APPEND rcsdk_power_sources src/request_macos.mm src/system_macos.mm)
    set_source_files_properties(src/request_macos.mm src/system_macos.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    set(rcsdk_power_libraries "-framework IOKit" "-framework CoreFoundation" "-framework CoreServices"
        "-framework SystemConfiguration")
elseif(WIN32)
    list(APPEND rcsdk_power_sources src/request_windows.cpp src/system_windows.cpp)
    set(rcsdk_power_libraries kernel32 advapi32 user32 iphlpapi powrprof wtsapi32 ws2_32)
elseif(ANDROID)
    list(APPEND rcsdk_power_sources src/request_android.cpp src/system_none.cpp)
else()
    list(APPEND rcsdk_power_sources src/request_none.cpp src/system_none.cpp)
endif()

function(rcsdk_power_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The library: the SDK's public headers and its own, nothing of src/.
add_library(rcsdk_power ${rcsdk_power_sources})
add_library(RemoteControl::Power ALIAS rcsdk_power)
set_target_properties(rcsdk_power PROPERTIES
    EXPORT_NAME Power
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    INSTALL_NAME_DIR "@rpath"
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rcsdk_power
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(rcsdk_power PUBLIC rcsdk_core PRIVATE Threads::Threads ${rcsdk_power_libraries})
target_compile_definitions(rcsdk_power PRIVATE RCSDK_BUILDING_POWER)
rcsdk_power_warnings(rcsdk_power)
install(TARGETS rcsdk_power EXPORT RemoteControlSDKTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/rcsdk DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

# The same, beside the SDK's internal objects, for tests that reach both
# (sessions that use it are tests/session.cpp's).
if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_library(rcsdk_power_internal STATIC ${rcsdk_power_sources})
    target_include_directories(rcsdk_power_internal PUBLIC include src)
    target_link_libraries(rcsdk_power_internal PUBLIC rcsdk_internal PRIVATE Threads::Threads ${rcsdk_power_libraries})
    rcsdk_power_warnings(rcsdk_power_internal)
    add_subdirectory(tests)
endif()
