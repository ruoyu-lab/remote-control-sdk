# Public-interface tests link RemoteControl::Core; the rest link the internal
# objects to exercise modules directly.

# iOS: test programs are application bundles with an identifier (local.rcsdk.<target>)
# that permissions can be granted to, e.g.
#   xcrun simctl privacy booted grant microphone local.rcsdk.rcsdk_test_ios_platform
function(rcsdk_ios_bundle target)
    if(IOS)
        set_target_properties(${target} PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${PROJECT_SOURCE_DIR}/cmake/ios_test_info.plist.in"
            MACOSX_BUNDLE_GUI_IDENTIFIER "local.rcsdk.${target}"
            MACOSX_BUNDLE_BUNDLE_NAME "${target}")
    endif()
endfunction()

# PORTS: the test binds fixed loopback ports; such tests never run at once
# (ctest -j), as they would take each other's ports. SOCKETS: it opens the
# system's sockets or local endpoints, which a browser has not (not built in
# WebAssembly). ARGS: its command line.
function(rcsdk_test name)
    cmake_parse_arguments(TEST "PUBLIC;PORTS;SOCKETS" "TIMEOUT" "SOURCES;ARGS" ${ARGN})
    if(TEST_SOCKETS AND EMSCRIPTEN)
        return() # A browser has no sockets of its own, nor local endpoints.
    endif()
    add_executable(rcsdk_test_${name} ${TEST_SOURCES})
    if(TEST_PUBLIC)
        target_link_libraries(rcsdk_test_${name} PRIVATE RemoteControl::Core)
    else()
        target_link_libraries(rcsdk_test_${name} PRIVATE rcsdk_internal)
    endif()
    target_include_directories(rcsdk_test_${name} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}")
    add_test(NAME rcsdk.${name} COMMAND rcsdk_test_${name} ${TEST_ARGS})
    rcsdk_ios_bundle(rcsdk_test_${name})
    if(TEST_TIMEOUT)
        set_tests_properties(rcsdk.${name} PROPERTIES TIMEOUT ${TEST_TIMEOUT})
    endif()
    if(TEST_PORTS)
        set_tests_properties(rcsdk.${name} PROPERTIES RESOURCE_LOCK loopback_ports)
    endif()
endfunction()

rcsdk_test(c_api PUBLIC SOURCES c_api.c)
target_link_libraries(rcsdk_test_c_api PRIVATE RemoteControl::Clipboard) # An official module, as a host links it.
set_target_properties(rcsdk_test_c_api PROPERTIES LINKER_LANGUAGE CXX)
rcsdk_test(view_geometry PUBLIC SOURCES public_view_geometry.cpp)
# Hosts compile the public headers with their own warnings; they must stay quiet.
# (MSVC is left out: in C it flags the anonymous union of rcs_event at /W4.)
rcsdk_test(public_headers PUBLIC SOURCES public_headers.c public_headers.cpp)
target_link_libraries(rcsdk_test_public_headers PRIVATE RemoteControl::Clipboard RemoteControl::FileTransfer
                      RemoteControl::Power RemoteControl::TextCaret RemoteControl::Thumbnail RemoteControl::Supervisor)
if(NOT MSVC)
    target_compile_options(rcsdk_test_public_headers PRIVATE -Wall -Wextra -Wpedantic -Werror)
endif()

rcsdk_test(agent PORTS SOCKETS SOURCES agent.cpp TIMEOUT 120)
if(TARGET rcsdk_test_agent)
    set_tests_properties(rcsdk.agent PROPERTIES SKIP_RETURN_CODE 77) # Android: no agent link from adb.
    target_link_libraries(rcsdk_test_agent PRIVATE rcsdk_thumbnail_internal)
endif()
rcsdk_test(audio SOURCES audio.cpp)
rcsdk_test(mixer SOURCES mixer.cpp)
rcsdk_test(crypto SOURCES crypto.cpp)
# The BearSSL backend's ARMv8 AES and SHA-256 against BearSSL's software;
# skipped where the processor has not the instructions (x86-64).
if(RCSDK_CRYPTO_BACKEND STREQUAL "bearssl")
    rcsdk_test(crypto_armv8 SOURCES crypto_armv8.cpp)
    target_include_directories(rcsdk_test_crypto_armv8 PRIVATE "${PROJECT_SOURCE_DIR}/third_party")
    set_tests_properties(rcsdk.crypto_armv8 PROPERTIES SKIP_RETURN_CODE 77)
endif()
rcsdk_test(cursor SOURCES cursor.cpp)
rcsdk_test(cursor_stream SOURCES cursor_stream.cpp)
rcsdk_test(h264 SOURCES h264.cpp)
rcsdk_test(group SOURCES group.cpp)
rcsdk_test(web_codec_config SOURCES web_codec_config.cpp)
rcsdk_test(web_json SOURCES web_json.cpp)
rcsdk_test(web_decoded SOURCES web_decoded.cpp)
# The browser package's options (web/glue.cpp), in WebAssembly, where the glue is built.
if(EMSCRIPTEN)
    rcsdk_test(web_glue SOURCES web_glue.cpp)
    target_link_libraries(rcsdk_test_web_glue PRIVATE rcsdk_clipboard_internal rcsdk_file_transfer_internal)
endif()
# The browser viewer example's server (examples/web/serve.mjs), in node where
# there is one: what makes it safe by default. Loopback, ports the system picks.
if(NOT EMSCRIPTEN AND NOT CMAKE_CROSSCOMPILING)
    find_program(RCSDK_TEST_NODE NAMES node)
    if(RCSDK_TEST_NODE)
        add_test(NAME rcsdk.web_serve COMMAND "${RCSDK_TEST_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/web_serve.mjs"
                                              "${PROJECT_SOURCE_DIR}/examples/web/serve.mjs")
        set_tests_properties(rcsdk.web_serve PROPERTIES TIMEOUT 30)
    endif()
endif()
rcsdk_test(host_channel SOURCES host_channel.cpp)
rcsdk_test(keys SOURCES keys.cpp)
rcsdk_test(av1 SOURCES av1.cpp)
if(RCSDK_WITH_LIBVPX) # Its frames come from the libvpx built in.
    rcsdk_test(vpx SOURCES vpx.cpp)
endif()
rcsdk_test(nal SOURCES nal.cpp)
rcsdk_test(modules SOURCES modules.cpp)
rcsdk_test(negotiation SOURCES negotiation.cpp)
rcsdk_test(yuv SOURCES yuv.cpp)
rcsdk_test(pictures SOURCES pictures.cpp)
rcsdk_test(sampling SOURCES sampling.cpp)
# Windows: the capture compositor turns a turned display's image upright (on the GPU; skips without one).
if(WIN32)
    rcsdk_test(surface_turns SOURCES surface_turns.cpp)
    set_tests_properties(rcsdk.surface_turns PROPERTIES SKIP_RETURN_CODE 77)
endif()
rcsdk_test(protocol SOURCES protocol.cpp)
rcsdk_test(fec SOURCES fec.cpp)
rcsdk_test(stall_watch SOURCES stall_watch.cpp)
rcsdk_test(queues SOURCES queues.cpp)
rcsdk_test(secure_channel PORTS SOCKETS SOURCES secure_channel.cpp TIMEOUT 20)
rcsdk_test(tcp PORTS SOCKETS SOURCES tcp.cpp TIMEOUT 60)
# The findings of docs/SECURITY-ASSESSMENT.md, shown over loopback.
rcsdk_test(security PORTS SOCKETS SOURCES security.cpp TIMEOUT 60)
rcsdk_test(udp PORTS SOCKETS SOURCES udp.cpp TIMEOUT 90)
# The forwarder (server/forwarder): tokens, the stream key, its messages, and
# the forwarder over loopback against ends that break its rules.
if(TARGET rcsfwd_objects)
    rcsdk_test(forwarder PORTS SOCKETS SOURCES forwarder.cpp TIMEOUT 90)
    target_link_libraries(rcsdk_test_forwarder PRIVATE rcsfwd_objects)
endif()
rcsdk_test(signals SOURCES signals.cpp)
rcsdk_test(stun SOURCES stun.cpp)
rcsdk_test(thumbnail SOURCES thumbnail.cpp)
target_link_libraries(rcsdk_test_thumbnail PRIVATE rcsdk_thumbnail_internal)
rcsdk_test(ice PORTS SOCKETS SOURCES ice.cpp TIMEOUT 30)
rcsdk_test(turn PORTS SOCKETS SOURCES turn.cpp TIMEOUT 30)
rcsdk_test(tls SOCKETS SOURCES tls.cpp)
rcsdk_test(websocket SOURCES websocket.cpp)
# TURN over TLS end to end: the tests' TURN server behind a TLS terminator in
# Python with a certificate openssl makes; skipped (77) without them, and on
# Windows, where it runs by hand against a Mac (see tls_relay.cpp).
# Not in WebAssembly: a browser starts no processes, and does TLS itself.
if(NOT EMSCRIPTEN)
    find_program(RCSDK_TEST_PYTHON NAMES python3)
    find_program(RCSDK_TEST_OPENSSL NAMES openssl)
    rcsdk_test(tls_relay PORTS SOURCES tls_relay.cpp TIMEOUT 180
               ARGS --python "${RCSDK_TEST_PYTHON}" --openssl "${RCSDK_TEST_OPENSSL}"
                    --script "${CMAKE_CURRENT_SOURCE_DIR}/support/tls_terminator.py")
    set_tests_properties(rcsdk.tls_relay PROPERTIES SKIP_RETURN_CODE 77)
    # A forwarder behind a TLS proxy (wss://): the same terminator in front of its WebSocket entry.
    if(TARGET rcsfwd_objects)
        rcsdk_test(forwarder_tls PORTS SOURCES forwarder_tls.cpp TIMEOUT 120
                   ARGS --python "${RCSDK_TEST_PYTHON}" --openssl "${RCSDK_TEST_OPENSSL}"
                        --script "${CMAKE_CURRENT_SOURCE_DIR}/support/tls_terminator.py")
        target_link_libraries(rcsdk_test_forwarder_tls PRIVATE rcsfwd_objects)
        set_tests_properties(rcsdk.forwarder_tls PROPERTIES SKIP_RETURN_CODE 77)
    endif()
endif()
rcsdk_test(rate_control SOURCES rate_control.cpp)
rcsdk_test(codec_roundtrip SOURCES codec_roundtrip.cpp TIMEOUT 600)
# On the real desktop, with windows of its own; skips (77) where the screen
# cannot be captured. Not with others at once: it reads what the screen shows.
if(APPLE AND NOT IOS)
    rcsdk_test(capture_targets SOURCES capture_targets.cpp support/test_window_mac.mm TIMEOUT 120)
    set_source_files_properties(support/test_window_mac.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
elseif(WIN32)
    rcsdk_test(capture_targets SOURCES capture_targets.cpp support/test_window_win.cpp TIMEOUT 120)
    target_link_libraries(rcsdk_test_capture_targets PRIVATE dbghelp)
endif()
# iOS, in the simulator: the audio session's coordinator, playback, the application's state.
if(IOS)
    rcsdk_test(ios_platform SOURCES ios_platform.mm)
    set_source_files_properties(ios_platform.mm PROPERTIES COMPILE_OPTIONS "-fobjc-arc")
    target_link_libraries(rcsdk_test_ios_platform PRIVATE rcsdk_clipboard_internal rcsdk_power_internal)
endif()
if(TARGET rcsdk_test_capture_targets)
    target_link_libraries(rcsdk_test_capture_targets PRIVATE rcsdk_thumbnail_internal)
    set_tests_properties(rcsdk.capture_targets PROPERTIES SKIP_RETURN_CODE 77 RUN_SERIAL ON)
endif()
# The session scenarios run as separate tests (rcsdk.session.<name>); a missing
# H.264 codec makes them skip (exit 77), not pass. rcsdk.session.list fails when
# this list and the table in session.cpp differ.
set(rcsdk_session_scenarios
    refusals streaming_gpu streaming_cpu recovery updates keyframes host_sources host_gpu_frames sample_buffers hardware_buffers stream_changes large_pictures cursor
    audio_changes udp keep_listening listen_after_refusals reconnect p2p p2p_keep_listening
    p2p_stale_signal data data_between_connections relay adaptive degradation wireless wifi_sound rate_decisions host_encoder host_codec host_decoder encoded_source encoded_packets record_only host_codec_errors host_encoded negotiation audio control host_input host_input_types caps_lock text_caret power power_actions power_wake power_actions_agent modules clipboard clipboard_viewers
    file_transfer file_transfer_control file_transfer_large file_transfer_reconnect file_transfer_resume
    file_transfer_host file_transfer_agent file_transfer_pending file_transfer_publish clipboard_files
    clipboard_files_viewers clipboard_files_agent agents
    machine restart capture_interruption identity pin callback_lifecycle host_datagram host_stream host_keep_listening
    host_listen_after_refusals picture_formats hardware_first
    host_reconnect host_backpressure host_errors host_inline host_device session_logs viewers_tcp viewers_udp
    viewers_p2p viewers_relay viewer_control viewers_busy admission parity lost_pictures viewer_audio streams pausing
    viewers_rate viewers_join_storm viewers_lossy viewers_quorum viewers_codecs viewers_encodings viewers_layers
    failures migration
    path_hold path_hold_expired host_restart host_viewers forwarder forwarder_refusals forwarder_moves
    forwarder_entries forwarder_late forwarder_uplink forwarder_rooms forwarder_token_refresh forwarder_host
    forwarder_control forwarder_clipboard forwarder_files forwarder_files_resume forwarder_microphone migration_p2p
    migration_relay network_policy standby standby_p2p migration_tcp path_hold_tcp path_hold_expired_tcp tcp_no_migrate
    standby_tcp migration_relay_tcp adaptive_tcp path_stalls refused_change)
add_executable(rcsdk_test_session session.cpp)
rcsdk_ios_bundle(rcsdk_test_session)
# The official modules' scenarios too (the clipboard's tests have its fake clipboard; file transfer's, its
# free space seam).
target_link_libraries(rcsdk_test_session PRIVATE rcsdk_clipboard_internal rcsdk_file_transfer_internal
                                                 rcsdk_power_internal rcsdk_text_caret_internal)
target_include_directories(rcsdk_test_session PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}"
                                                      "${PROJECT_SOURCE_DIR}/modules/clipboard/tests")
# The forwarder's scenarios run one in this process (skipped where it is not built).
if(TARGET rcsfwd_objects)
    target_link_libraries(rcsdk_test_session PRIVATE rcsfwd_objects)
    target_compile_definitions(rcsdk_test_session PRIVATE RCSDK_TEST_FORWARDER)
endif()
add_test(NAME rcsdk.session.list COMMAND rcsdk_test_session --check-list ${rcsdk_session_scenarios})
foreach(scenario IN LISTS rcsdk_session_scenarios)
    if(scenario STREQUAL "host_device" AND NOT WIN32)
        continue()
    endif()
    add_test(NAME rcsdk.session.${scenario} COMMAND rcsdk_test_session ${scenario})
    set_tests_properties(rcsdk.session.${scenario} PROPERTIES TIMEOUT 90 SKIP_RETURN_CODE 77
                                                             RESOURCE_LOCK loopback_ports)
endforeach()
# 1 GiB over loopback beside the picture: 7..35 s on a Mac, longer under load (256 MiB under a sanitizer).
set_tests_properties(rcsdk.session.file_transfer_large PROPERTIES TIMEOUT 180)
# Two runs of half a minute each, 1080p60 through the airtime model.
set_tests_properties(rcsdk.session.wifi_sound PROPERTIES TIMEOUT 180)

# Install the SDK and build a separate project against the installed package.
if(NOT CMAKE_CROSSCOMPILING)
    add_test(NAME rcsdk.installed_package
        COMMAND "${CMAKE_COMMAND}"
            "-DSDK_BINARY_DIR=${PROJECT_BINARY_DIR}"
            "-DSDK_CONFIG=$<CONFIG>"
            "-DSDK_GENERATOR=${CMAKE_GENERATOR}"
            "-DSDK_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
            "-DCONSUMER_SOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}/consumer"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/installed_consumer.cmake")
    set_tests_properties(rcsdk.installed_package PROPERTIES TIMEOUT 120)
endif()

# The fuzz targets, in a build made for them (RCSDK_BUILD_FUZZERS).
if(RCSDK_BUILD_FUZZERS)
    add_subdirectory(fuzz)
endif()
