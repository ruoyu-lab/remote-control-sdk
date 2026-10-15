# All SDK code compiles once into an object library. The public library
# (RemoteControl::Core) is built from it; tests link the same objects through
# rcsdk_internal to reach internal modules.

add_library(rcsdk_objects OBJECT
    agent/agent_server.cpp
    agent/link_messages.cpp
    agent/machine.cpp
    agent/host_encoder.cpp
    agent/media_agent.cpp
    agent/remote.cpp
    agent/video_pipeline.cpp
    api/c_api.cpp
    audio/codec.cpp
    audio/mixer.cpp
    audio/playout.cpp
    codecs/factory.cpp
    crypto/composed.cpp
    crypto/cpace.cpp
    media/cursor.cpp
    modules/machine_host.cpp
    modules/module.cpp
    modules/registry.cpp
    protocol/connection.cpp
    protocol/fec.cpp
    protocol/media_keys.cpp
    protocol/messages.cpp
    protocol/rate_control.cpp
    protocol/udp_connection.cpp
    protocol/route_demux.cpp
    protocol/stall_watch.cpp
    protocol/tcp_demux.cpp
    protocol/udp_demux.cpp
    session/audio_stream.cpp
    session/checks.cpp
    session/config.cpp
    session/connect.cpp
    session/cursor_stream.cpp
    session/data_channel.cpp
    session/forwarder.cpp
    session/group_plan.cpp
    session/host_decoder.cpp
    session/host_sources.cpp
    session/modules.cpp
    session/negotiation.cpp
    session/path_info.cpp
    session/session.cpp
    session/share.cpp
    session/share_agent.cpp
    session/share_viewer.cpp
    session/share_video.cpp
    session/signals.cpp
    session/stream_plan.cpp
    session/thumbnail.cpp
    session/view.cpp
    seams/seams.cpp
    util/log.cpp
    util/sleep.cpp
    transport/credentials.cpp
    transport/framed_socket.cpp
    transport/handshake.cpp
    transport/host_channel.cpp
    transport/ice.cpp
    transport/interfaces.cpp
    transport/local_stream.cpp
    transport/network_changes.cpp
    transport/secure_channel.cpp
    transport/socket.cpp
    transport/stun.cpp
    transport/tls.cpp
    transport/turn.cpp
    transport/turn_tcp_socket.cpp
    transport/tcp_stream.cpp
    transport/websocket.cpp)

if(APPLE)
    # What macOS and iOS share: VideoToolbox, pixel buffers, AAC.
    set(rcsdk_platform_sources
        platform/apple/aac.mm
        platform/apple/codecs.mm
        platform/apple/decoder.mm
        platform/apple/encoder.mm
        platform/apple/frames.mm
        platform/apple/network.mm
        platform/apple/sample_buffers.mm
        platform/apple/wifi.mm)
    if(IOS)
        list(APPEND rcsdk_platform_sources
            platform/ios/app_state.mm
            platform/ios/audio.mm
            platform/ios/audio_engine.mm
            platform/ios/system.mm)
        set(rcsdk_system_libraries
            "-framework Foundation" "-framework UIKit" "-framework CoreVideo" "-framework CoreMedia"
            "-framework VideoToolbox" "-framework Security" "-framework Network"
            "-framework AudioToolbox" "-framework AVFoundation")
    else()
        list(APPEND rcsdk_platform_sources
            platform/macos/audio.mm
            platform/macos/audio_tap.mm
            platform/macos/capture.mm
            platform/macos/cursor_tracker.mm
            platform/macos/input.mm
            platform/macos/microphone.mm
            platform/macos/system.mm
            platform/macos/window_server.mm)
        target_sources(rcsdk_objects PRIVATE platform/other/application.cpp)
        set(rcsdk_system_libraries
            "-framework Foundation" "-framework AppKit" "-framework ScreenCaptureKit" "-framework ApplicationServices"
            "-framework CoreGraphics" "-framework CoreVideo" "-framework CoreMedia"
            "-framework VideoToolbox" "-framework Security" "-framework Network"
            "-framework AudioToolbox" "-framework CoreAudio" "-framework IOKit" "-framework AVFoundation"
            "-framework CoreWLAN" bsm)
    endif()
    target_sources(rcsdk_objects PRIVATE ${rcsdk_platform_sources} platform/other/desktop.cpp platform/other/java.cpp
                                         platform/other/wifi_power.cpp)
    set_source_files_properties(${rcsdk_platform_sources} PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
elseif(WIN32)
    target_sources(rcsdk_objects PRIVATE
        platform/windows/audio.cpp
        platform/windows/capture.cpp
        platform/windows/decoder.cpp
        platform/windows/desktop.cpp
        platform/windows/encoder.cpp
        platform/windows/gdi_cursor.cpp
        platform/windows/helper.cpp
        platform/windows/input.cpp
        platform/windows/network.cpp
        platform/windows/nvdec.cpp
        platform/windows/nvenc.cpp
        platform/windows/nvenc_api12.cpp
        platform/windows/nvenc_api13.cpp
        platform/windows/onevpl.cpp
        platform/windows/pointer_input.cpp
        platform/windows/surfaces.cpp
        platform/windows/system.cpp
        platform/windows/thumbnails.cpp
        platform/windows/tls_connection.cpp
        platform/windows/window.cpp
        platform/windows/window_capture.cpp
        platform/other/application.cpp
        platform/other/java.cpp)
    # No mfplat: Media Foundation is loaded at run time (win::mfplat), since
    # Windows N/KN editions and Windows Server may not have it.
    set(rcsdk_system_libraries
        ws2_32 iphlpapi bcrypt advapi32 wtsapi32 powrprof ole32 oleaut32 mmdevapi avrt user32 gdi32 d3d11 dxgi cfgmgr32
        dxguid d3dcompiler mfuuid wmcodecdspuuid strmiids dwmapi runtimeobject version secur32 crypt32 windowscodecs)
elseif(ANDROID)
    # MediaCodec, AAudio, EGL and hardware buffers from the NDK; Java through the host's VM (rcsdk/android.h).
    if(ANDROID_PLATFORM_LEVEL LESS 29)
        message(FATAL_ERROR "The SDK needs Android 10 (API 29) or later: configure with ANDROID_PLATFORM=android-29 "
                            "or later (Qt's toolchain defaults to android-28)")
    endif()
    target_sources(rcsdk_objects PRIVATE
        platform/android/aac.cpp
        platform/android/audio.cpp
        platform/android/codecs.cpp
        platform/android/decoder.cpp
        platform/android/encoder.cpp
        platform/android/frames.cpp
        platform/android/gl.cpp
        platform/android/jni.cpp
        platform/android/network.cpp
        platform/android/surfaces.cpp
        platform/android/system.cpp
        platform/other/application.cpp
        platform/other/desktop.cpp
        platform/other/network_poll.cpp)
    set(rcsdk_system_libraries mediandk android aaudio EGL GLESv3 log)
elseif(EMSCRIPTEN)
    target_sources(rcsdk_objects PRIVATE platform/web/platform.cpp platform/web/decoder.cpp platform/web/audio.cpp platform/other/application.cpp
                                         platform/other/desktop.cpp platform/other/java.cpp platform/other/wifi_power.cpp
                                         platform/other/networks.cpp platform/other/network_poll.cpp)
else()
    target_sources(rcsdk_objects PRIVATE platform/unsupported/platform.cpp platform/other/application.cpp
                                         platform/other/desktop.cpp platform/other/java.cpp platform/other/wifi_power.cpp
                                         platform/other/networks.cpp platform/other/network_poll.cpp)
endif()

# The codec libraries built in (RCSDK_WITH_*): linked wherever the system's libraries are.
foreach(library IN ITEMS openh264 libvpx libaom dav1d)
    string(TOUPPER "${library}" option)
    if(RCSDK_WITH_${option})
        target_sources(rcsdk_objects PRIVATE codecs/${library}.cpp)
        list(APPEND rcsdk_system_libraries rcsdk_${library})
    endif()
endforeach()

# Cryptography and TLS (RCSDK_CRYPTO_BACKEND): the system's (Windows: TLS in
# its list above), or BearSSL's.
if(RCSDK_CRYPTO_BACKEND STREQUAL "bearssl")
    target_sources(rcsdk_objects PRIVATE crypto/bearssl_armv8.cpp crypto/crypto_bearssl.cpp transport/tls_bearssl.cpp)
    list(APPEND rcsdk_system_libraries rcsdk_bearssl) # Linked wherever the system's libraries are.
elseif(APPLE)
    target_sources(rcsdk_objects PRIVATE crypto/crypto.cpp platform/apple/tls_connection.mm)
    set_source_files_properties(platform/apple/tls_connection.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
else()
    target_sources(rcsdk_objects PRIVATE crypto/crypto.cpp)
endif()

set_target_properties(rcsdk_objects PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON)
target_include_directories(rcsdk_objects
    PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}" "${PROJECT_SOURCE_DIR}/third_party"
            "$<TARGET_PROPERTY:rcsdk_opus,INTERFACE_INCLUDE_DIRECTORIES>")
target_compile_definitions(rcsdk_objects PRIVATE
    RCSDK_BUILDING_CORE RCSDK_VERSION_STRING="${rcsdk_version_text}" "OPUS_EXPORT=")
option(RCSDK_TRACE "Print timestamps of the Windows media pipeline's stages to stderr (diagnostics)" OFF)
if(RCSDK_TRACE)
    target_compile_definitions(rcsdk_objects PRIVATE RCSDK_TRACE)
endif()
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(rcsdk_objects PUBLIC RCSDK_STATIC)
endif()
# No two sources of the library share a name: generators that put a target's
# objects in one directory (Xcode, as an iOS host's project builds the SDK)
# would make both network.o, and the archive would keep one of them.
get_target_property(rcsdk_sources rcsdk_objects SOURCES)
set(rcsdk_source_names)
foreach(source IN LISTS rcsdk_sources)
    get_filename_component(name "${source}" NAME_WE)
    if(name IN_LIST rcsdk_source_names)
        message(FATAL_ERROR "Two sources of rcsdk_objects are both named ${name}: rename one (${source})")
    endif()
    list(APPEND rcsdk_source_names "${name}")
endforeach()

target_link_libraries(rcsdk_objects PUBLIC Threads::Threads ${rcsdk_system_libraries})
if(MSVC)
    # getenv is used for what the supervisor passes on, read once; no _dupenv_s.
    target_compile_definitions(rcsdk_objects PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_compile_options(rcsdk_objects PRIVATE /W4 /utf-8)
else()
    target_compile_options(rcsdk_objects PRIVATE -Wall -Wextra -Wpedantic)
endif()

# ---- Public library ---------------------------------------------------------

add_library(rcsdk_core api/library.cpp $<TARGET_OBJECTS:rcsdk_objects> $<TARGET_OBJECTS:rcsdk_opus>
    $<TARGET_OBJECTS:rcsdk_monocypher>)
add_library(RemoteControl::Core ALIAS rcsdk_core)
set_target_properties(rcsdk_core PROPERTIES
    EXPORT_NAME Core
    VERSION "${PROJECT_VERSION}"
    # Until 1.0 each minor version has an interface of its own.
    SOVERSION "${PROJECT_VERSION_MAJOR}.${PROJECT_VERSION_MINOR}"
    INSTALL_NAME_DIR "@rpath")
target_include_directories(rcsdk_core PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>")
target_link_libraries(rcsdk_core PRIVATE Threads::Threads ${rcsdk_system_libraries})
if(NOT BUILD_SHARED_LIBS)
    target_compile_definitions(rcsdk_core PUBLIC RCSDK_STATIC)
endif()

install(TARGETS rcsdk_core EXPORT RemoteControlSDKTargets
    RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
    LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}"
    ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(DIRECTORY "${PROJECT_SOURCE_DIR}/include/rcsdk" DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}")
install(FILES "${PROJECT_SOURCE_DIR}/LICENSE" "${PROJECT_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/RemoteControlSDK")

# ---- Internal access for tests and the forwarder ---------------------------

if((BUILD_TESTING AND PROJECT_IS_TOP_LEVEL) OR RCSDK_BUILD_FORWARDER)
    add_library(rcsdk_internal STATIC $<TARGET_OBJECTS:rcsdk_objects> $<TARGET_OBJECTS:rcsdk_opus>
    $<TARGET_OBJECTS:rcsdk_monocypher>)
    target_include_directories(rcsdk_internal PUBLIC
        "${PROJECT_SOURCE_DIR}/include" "${CMAKE_CURRENT_SOURCE_DIR}")
    target_compile_definitions(rcsdk_internal PUBLIC RCSDK_STATIC)
    target_link_libraries(rcsdk_internal PUBLIC Threads::Threads ${rcsdk_system_libraries})
endif()
