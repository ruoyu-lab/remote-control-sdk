# The supervisor and the sessions to choose from (rcsdk/supervisor.h): a
# Windows service's helper in a user's session.
set(rcsdk_supervisor_sources src/api.cpp)
if(WIN32)
    list(APPEND rcsdk_supervisor_sources src/sessions_windows.cpp src/supervisor_windows.cpp)
    set(rcsdk_supervisor_libraries advapi32 wtsapi32 userenv ws2_32)
else()
    list(APPEND rcsdk_supervisor_sources src/none.cpp)
endif()

function(rcsdk_supervisor_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /utf-8)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

# The library: the SDK's public headers and its own, nothing of src/.
add_library(rcsdk_supervisor ${rcsdk_supervisor_sources})
add_library(RemoteControl::Supervisor ALIAS rcsdk_supervisor)
set_target_properties(rcsdk_supervisor PROPERTIES
    EXPORT_NAME Supervisor
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    INSTALL_NAME_DIR "@rpath"
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rcsdk_supervisor
    PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>" "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(rcsdk_supervisor PUBLIC rcsdk_core PRIVATE Threads::Threads ${rcsdk_supervisor_libraries})
target_compile_definitions(rcsdk_supervisor PRIVATE RCSDK_BUILDING_SUPERVISOR RCSDK_VERSION_STRING="${PROJECT_VERSION}")
rcsdk_supervisor_warnings(rcsdk_supervisor)
install(TARGETS rcsdk_supervisor EXPORT RemoteControlSDKTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY include/rcsdk DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")

if(BUILD_TESTING AND PROJECT_IS_TOP_LEVEL)
    add_subdirectory(tests)
endif()
