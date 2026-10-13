# Measurement tools for performance work; not part of the SDK package.
# A latency target runs on the controlled machine, the probe on the viewer.

add_executable(rcsdk_response_probe response_probe.cpp)
target_link_libraries(rcsdk_response_probe PRIVATE RemoteControl::Core)

# What temporal layers and a second encoding session cost each encoder (GROUP-SHARING-DESIGN.md, stage 3),
# and what each encoder and decoder costs per picture in real time.
if(TARGET rcsdk_internal)
    add_executable(rcsdk_encoder_layers encoder_layers.cpp)
    target_link_libraries(rcsdk_encoder_layers PRIVATE rcsdk_internal)
    add_executable(rcsdk_codec_timing codec_timing.cpp)
    target_link_libraries(rcsdk_codec_timing PRIVATE rcsdk_internal)
endif()

# The load a forwarder carries: synthetic sharers and light viewers over its hop (docs/PERFORMANCE.md);
# macOS and Linux (it reads the forwarder's CPU and memory the POSIX way).
if(TARGET rcsfwd_objects AND NOT WIN32)
    add_executable(rcsdk_forwarder_load forwarder_load.cpp)
    target_link_libraries(rcsdk_forwarder_load PRIVATE rcsfwd_objects)
endif()

# The decode probes' VP9 and AV1 keyframes, from the SDK's own libraries.
if(TARGET rcsdk_internal AND RCSDK_WITH_LIBVPX AND RCSDK_WITH_LIBAOM)
    add_executable(rcsdk_codec_probes codec_probes.cpp)
    target_link_libraries(rcsdk_codec_probes PRIVATE rcsdk_internal)
endif()

if(APPLE)
    target_link_libraries(rcsdk_response_probe PRIVATE "-framework CoreFoundation" "-framework CoreVideo")
    add_executable(rcsdk_latency_target latency_target_macos.mm)
    target_compile_options(rcsdk_latency_target PRIVATE -fobjc-arc)
    target_link_libraries(rcsdk_latency_target PRIVATE "-framework AppKit")
    add_executable(rcsdk_audio_latency audio_latency_macos.mm)
    target_compile_options(rcsdk_audio_latency PRIVATE -fobjc-arc)
    target_link_libraries(rcsdk_audio_latency PRIVATE RemoteControl::Core "-framework CoreAudio" "-framework Foundation")
    add_executable(rcsdk_videotoolbox_probe videotoolbox_probe_macos.mm)
    target_compile_options(rcsdk_videotoolbox_probe PRIVATE -fobjc-arc -Wno-four-char-constants)
    target_include_directories(rcsdk_videotoolbox_probe PRIVATE "${PROJECT_SOURCE_DIR}/src")
    target_link_libraries(rcsdk_videotoolbox_probe PRIVATE RemoteControl::Core "-framework VideoToolbox"
                                                          "-framework CoreMedia" "-framework CoreVideo"
                                                          "-framework Foundation")
    add_executable(rcsdk_glass_probe glass_probe_macos.mm)
    target_compile_options(rcsdk_glass_probe PRIVATE -fobjc-arc)
    target_link_libraries(rcsdk_glass_probe PRIVATE "-framework ScreenCaptureKit" "-framework CoreMedia"
                                                    "-framework CoreVideo" "-framework CoreGraphics" "-framework Foundation")
elseif(WIN32)
    target_link_libraries(rcsdk_response_probe PRIVATE d3d11 ws2_32)
    add_executable(rcsdk_latency_target latency_target_windows.cpp)
    target_link_libraries(rcsdk_latency_target PRIVATE user32 gdi32 dwmapi ws2_32)
    add_executable(rcsdk_audio_latency audio_latency_windows.cpp)
    target_link_libraries(rcsdk_audio_latency PRIVATE RemoteControl::Core ole32)
endif()
