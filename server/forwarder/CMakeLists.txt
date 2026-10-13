# The forwarder (include/rcsfwd.h): RCS_TRANSPORT_FORWARDER's server, a
# library (RemoteControl::Forwarder) and the rcs_forwarder program, of the
# SDK's version and released apart from it (install component "forwarder").
# Built from the SDK's internal objects (rcsdk_internal: the hop is the SDK's
# UDP connection), which the SDK's own library holds too.

add_library(rcsfwd_objects OBJECT src/api.cpp src/entry.cpp src/forwarder.cpp src/token.cpp)
set_target_properties(rcsfwd_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rcsfwd_objects PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_link_libraries(rcsfwd_objects PUBLIC rcsdk_internal)
target_compile_definitions(rcsfwd_objects PRIVATE RCSDK_BUILDING_FORWARDER
                                                  RCSDK_VERSION_STRING="${rcsdk_version_text}"
                                          INTERFACE RCSFWD_STATIC) # Tests link the objects themselves.
if(MSVC)
    target_compile_options(rcsfwd_objects PRIVATE /W4 /utf-8)
else()
    target_compile_options(rcsfwd_objects PRIVATE -Wall -Wextra -Wpedantic)
endif()

add_library(rcsfwd SHARED $<TARGET_OBJECTS:rcsfwd_objects>)
add_library(RemoteControl::Forwarder ALIAS rcsfwd)
set_target_properties(rcsfwd PROPERTIES
    EXPORT_NAME Forwarder
    VERSION "${PROJECT_VERSION}"
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    INSTALL_NAME_DIR "@rpath")
target_include_directories(rcsfwd PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>"
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(rcsfwd PRIVATE rcsdk_internal)

add_executable(rcs_forwarder src/main.cpp)
target_link_libraries(rcs_forwarder PRIVATE rcsfwd)
if(APPLE)
    set_target_properties(rcs_forwarder PROPERTIES INSTALL_RPATH "@executable_path/../${CMAKE_INSTALL_LIBDIR}")
elseif(UNIX)
    set_target_properties(rcs_forwarder PROPERTIES INSTALL_RPATH "$ORIGIN/../${CMAKE_INSTALL_LIBDIR}")
endif()

install(TARGETS rcsfwd rcs_forwarder EXPORT RemoteControlForwarderTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}" COMPONENT forwarder
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT forwarder
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}" COMPONENT forwarder)
# rcsfwd.h, and the one SDK header it names (versions, results).
install(FILES include/rcsfwd.h DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}" COMPONENT forwarder)
install(FILES "${PROJECT_SOURCE_DIR}/include/rcsdk/rcsdk.h" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/rcsdk"
        COMPONENT forwarder)
# The deployment samples (docs/guide/24-forwarder.md): a configuration, a
# systemd service and its logrotate rule (Linux), a launchd job (macOS); and
# the licenses of what is built in (BearSSL's among them where it is).
install(FILES deploy/forwarder.conf.example deploy/rcs-forwarder.service deploy/rcs-forwarder.logrotate
              deploy/rcs-forwarder.plist "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/rcs-forwarder" COMPONENT forwarder)
install(EXPORT RemoteControlForwarderTargets NAMESPACE RemoteControl::
    FILE RemoteControlForwarderConfig.cmake
    DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/RemoteControlForwarder" COMPONENT forwarder)
