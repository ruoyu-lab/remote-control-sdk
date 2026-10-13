# Examples use only the public interface (RemoteControl::Core, the official modules and components).

# A module of the examples', built from the public headers alone, as a host's own would be.
add_library(rcsdk_example_pointer STATIC pointer/pointer_module.cpp)
target_link_libraries(rcsdk_example_pointer PUBLIC RemoteControl::Core)

add_executable(rcsdk_cli cli/main.cpp)
target_link_libraries(rcsdk_cli PRIVATE RemoteControl::Core RemoteControl::Clipboard RemoteControl::FileTransfer
                      RemoteControl::Power
                      RemoteControl::TextCaret RemoteControl::Thumbnail rcsdk_example_pointer)
if(APPLE)
    target_link_libraries(rcsdk_cli PRIVATE "-framework CoreFoundation")
endif()
if(IOS) # The simulator's: an identifier to grant permissions to, and why it asks.
    set_target_properties(rcsdk_cli PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/cmake/ios_test_info.plist.in"
        MACOSX_BUNDLE_GUI_IDENTIFIER "local.rcsdk.rcsdk_cli"
        MACOSX_BUNDLE_BUNDLE_NAME "rcsdk_cli")
endif()
# A stand-in for a host's own server for RCS_TRANSPORT_HOST (rcsdk_cli --transport host --relay).
if(NOT IOS)
    add_executable(rcsdk_host_relay host-relay/relay.cpp)
endif()
if(WIN32)
    target_link_libraries(rcsdk_cli PRIVATE ws2_32)
    target_link_libraries(rcsdk_host_relay PRIVATE ws2_32)
endif()

if(WIN32)
    add_executable(rcsdk_windows_viewer windows/viewer.cpp)
    target_link_libraries(rcsdk_windows_viewer PRIVATE RemoteControl::Core d3d11 dxgi user32 gdi32)
    # A service that shares the machine from the sign-in screen on (unattended access).
    add_executable(rcsdk_unattended unattended/main.cpp)
    target_link_libraries(rcsdk_unattended PRIVATE RemoteControl::Core RemoteControl::Power RemoteControl::Supervisor
                          advapi32)
elseif(APPLE AND NOT IOS)
    # The same on macOS: a LaunchDaemon holds the session, a LaunchAgent (login window and users) is its agent.
    add_executable(rcsdk_unattended unattended/macos.cpp)
    target_link_libraries(rcsdk_unattended PRIVATE RemoteControl::Core RemoteControl::Power
                          "-framework CoreFoundation" "-framework SystemConfiguration")
    # Screen Recording and Accessibility grants that survive rebuilds (as rcsdk_sign_for_development below).
    add_custom_command(TARGET rcsdk_unattended POST_BUILD
        COMMAND codesign --force --sign - --identifier local.rcsdk.unattended
                "-r=designated => identifier \"local.rcsdk.unattended\"" "$<TARGET_FILE:rcsdk_unattended>"
        VERBATIM)
endif()

# macOS ties Screen Recording and Accessibility grants to an app's designated
# requirement. An ad-hoc signature's default requirement is its code hash,
# which changes on every build; requiring only the bundle identifier keeps the
# grants across rebuilds. Development only: sign releases with a real identity.
function(rcsdk_sign_for_development target identifier)
    if(APPLE AND NOT IOS)
        add_custom_command(TARGET ${target} POST_BUILD
            COMMAND codesign --force --sign - --identifier ${identifier}
                    "-r=designated => identifier \"${identifier}\"" "$<TARGET_BUNDLE_DIR:${target}>"
            VERBATIM)
    endif()
endfunction()

# Qt Quick: RemoteVideo draws decoded frames with the scene graph's own GPU
# device (QRhi: Metal on macOS, D3D11 on Windows; OpenGL ES on Android).
if(RCSDK_BUILD_QML_DEMO)
    find_package(Qt6 6.6 REQUIRED COMPONENTS Quick QuickControls2 ShaderTools GuiPrivate Network)
    qt_add_executable(rcsdk_qml_demo WIN32 MACOSX_BUNDLE
        qml-demo/main.cpp
        qml-demo/app_input.cpp qml-demo/app_input.hpp
        qml-demo/demo_app.cpp qml-demo/demo_app.hpp
        qml-demo/lan_discovery.cpp qml-demo/lan_discovery.hpp
        qml-demo/remote_session.cpp qml-demo/remote_session.hpp
        qml-demo/remote_video.cpp qml-demo/remote_video.hpp
        qml-demo/video_node.hpp)
    if(ANDROID)
        # The screen through MediaProjection, a viewer's input through an
        # accessibility service: Java in qml-demo/android, packaged by Qt.
        target_sources(rcsdk_qml_demo PRIVATE qml-demo/video_node_gles.cpp qml-demo/screen_projection.hpp
                       qml-demo/screen_projection_android.cpp)
        target_link_libraries(rcsdk_qml_demo PRIVATE EGL GLESv3 android)
        set_target_properties(rcsdk_qml_demo PROPERTIES
            QT_ANDROID_PACKAGE_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/qml-demo/android"
            QT_ANDROID_PACKAGE_NAME local.rcsdk.qmldemo
            QT_ANDROID_MIN_SDK_VERSION 29
            QT_ANDROID_TARGET_SDK_VERSION 35
            QT_ANDROID_APP_NAME "远程控制 Demo")
    else()
        target_sources(rcsdk_qml_demo PRIVATE qml-demo/video_node_rhi.cpp qml-demo/native_planes.hpp)
    endif()
    if(APPLE)
        target_sources(rcsdk_qml_demo PRIVATE qml-demo/native_planes_metal.mm)
        set_source_files_properties(qml-demo/native_planes_metal.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
        target_link_libraries(rcsdk_qml_demo PRIVATE "-framework Metal" "-framework CoreVideo")
        if(IOS) # Sharing the demo's own picture, or the whole screen through its broadcast extension.
            set(broadcast_settings qml-demo/broadcast/broadcast_settings.hpp qml-demo/broadcast/broadcast_settings.mm)
            target_sources(rcsdk_qml_demo PRIVATE qml-demo/app_capture.hpp qml-demo/app_capture_ios.mm
                           qml-demo/screen_broadcast.hpp qml-demo/screen_broadcast_ios.mm ${broadcast_settings}
                           qml-demo/display_rate.hpp qml-demo/display_rate_ios.mm)
            set_source_files_properties(qml-demo/app_capture_ios.mm qml-demo/screen_broadcast_ios.mm
                                        qml-demo/display_rate_ios.mm
                                        qml-demo/broadcast/broadcast_settings.mm qml-demo/broadcast/SampleHandler.mm
                                        PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
            target_link_libraries(rcsdk_qml_demo PRIVATE "-framework ReplayKit" "-framework VideoToolbox"
                                  "-framework CoreMedia" "-framework Security")
            # The broadcast upload extension: its own process, no Qt, about 50 MB.
            add_executable(rcsdk_qml_demo_broadcast MACOSX_BUNDLE qml-demo/broadcast/SampleHandler.mm
                           ${broadcast_settings})
            target_link_libraries(rcsdk_qml_demo_broadcast PRIVATE RemoteControl::Core "-framework ReplayKit"
                                  "-framework Security" "-Wl,-e,_NSExtensionMain")
            target_include_directories(rcsdk_qml_demo_broadcast PRIVATE qml-demo/broadcast)
            set_target_properties(rcsdk_qml_demo_broadcast PROPERTIES
                OUTPUT_NAME RemoteControlBroadcast
                BUNDLE_EXTENSION appex
                XCODE_PRODUCT_TYPE com.apple.product-type.app-extension
                MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/qml-demo/broadcast/Info.plist.in"
                MACOSX_BUNDLE_GUI_IDENTIFIER local.rcsdk.qmldemo.broadcast
                XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER local.rcsdk.qmldemo.broadcast
                XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
                XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/qml-demo/shared.entitlements")
            set_target_properties(rcsdk_qml_demo PROPERTIES
                XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${CMAKE_CURRENT_SOURCE_DIR}/qml-demo/shared.entitlements"
                XCODE_EMBED_APP_EXTENSIONS rcsdk_qml_demo_broadcast)
            add_dependencies(rcsdk_qml_demo rcsdk_qml_demo_broadcast)
        endif()
    elseif(WIN32)
        target_sources(rcsdk_qml_demo PRIVATE qml-demo/native_planes_d3d11.cpp)
        target_link_libraries(rcsdk_qml_demo PRIVATE d3d11)
    endif()
    set_target_properties(rcsdk_qml_demo PROPERTIES
        OUTPUT_NAME RemoteControlQmlDemo
        AUTOMOC ON
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/qml-demo/Info.plist.in")
    set_source_files_properties(qml-demo/Main.qml PROPERTIES QT_RESOURCE_ALIAS Main.qml)
    qt_add_qml_module(rcsdk_qml_demo
        URI RemoteControlDemo
        VERSION 1.0
        RESOURCE_PREFIX /qt/qml # Where the engine looks for modules (QTP0001).
        QML_FILES qml-demo/Main.qml)
    qt_add_shaders(rcsdk_qml_demo "rcsdk_qml_demo_shaders"
        PREFIX "/"
        BASE qml-demo
        FILES qml-demo/shaders/nv12.vert qml-demo/shaders/nv12.frag)
    target_link_libraries(rcsdk_qml_demo PRIVATE
        RemoteControl::Core RemoteControl::Clipboard RemoteControl::Power Qt6::Quick Qt6::QuickControls2
        Qt6::GuiPrivate Qt6::Network)
    target_include_directories(rcsdk_qml_demo PRIVATE qml-demo)
    if(IOS) # iPhone and iPad; signed by Xcode with the developer's team.
        set_target_properties(rcsdk_qml_demo PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_CURRENT_SOURCE_DIR}/qml-demo/Info.ios.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER local.rcsdk.qmldemo
            XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER local.rcsdk.qmldemo
            XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2")
    endif()
    rcsdk_sign_for_development(rcsdk_qml_demo local.rcsdk.qmldemo)
    if(MSVC)
        target_compile_options(rcsdk_qml_demo PRIVATE /utf-8)
    endif()
endif()
