/* The public C headers, compiled as C with warnings as errors (see
 * CMakeLists): every *_INIT macro initialises every field. */
#include <rcsdk/agent.h>
#include <rcsdk/android.h>
#include <rcsdk/apple.h>
#include <rcsdk/clipboard.h>
#include <rcsdk/file_transfer.h>
#include <rcsdk/gpu.h>
#include <rcsdk/host_codec.h>
#include <rcsdk/host_transport.h>
#include <rcsdk/module.h>
#include <rcsdk/power.h>
#include <rcsdk/rate_control.h>
#include <rcsdk/rcsdk.h>
#include <rcsdk/supervisor.h>
#include <rcsdk/text_caret.h>
#include <rcsdk/thumbnail.h>
#include <rcsdk/tuning.h>

int publicHeadersC(void) {
    rcs_agent_link_config v_agent_link_config = RCS_AGENT_LINK_CONFIG_INIT;
    rcs_agent_config v_agent_config = RCS_AGENT_CONFIG_INIT;
    rcs_agent_callbacks v_agent_callbacks = RCS_AGENT_CALLBACKS_INIT;
    rcs_gpu_frame_info v_gpu_frame_info = RCS_GPU_FRAME_INFO_INIT;
    rcs_sample_buffer_info v_sample_buffer_info = RCS_SAMPLE_BUFFER_INFO_INIT;
    rcs_gpu_device_config v_gpu_device_config = RCS_GPU_DEVICE_CONFIG_INIT;
    rcs_host_encoder_config v_host_encoder_config = RCS_HOST_ENCODER_CONFIG_INIT;
    rcs_host_decoder_config v_host_decoder_config = RCS_HOST_DECODER_CONFIG_INIT;
    rcs_encoded_output_config v_encoded_output_config = RCS_ENCODED_OUTPUT_CONFIG_INIT;
    rcs_encoded_audio v_encoded_audio = RCS_ENCODED_AUDIO_INIT;
    rcs_host_transport_config v_host_transport_config = RCS_HOST_TRANSPORT_CONFIG_INIT;
    rcs_module_shared v_module_shared = RCS_MODULE_SHARED_INIT;
    rcs_picture v_picture = RCS_PICTURE_INIT;
    rcs_rate_decider_config v_rate_decider_config = RCS_RATE_DECIDER_CONFIG_INIT;
    rcs_capabilities v_capabilities = RCS_CAPABILITIES_INIT;
    rcs_share_video_config v_share_video_config = RCS_SHARE_VIDEO_CONFIG_INIT;
    rcs_encoder_config v_encoder_config = RCS_ENCODER_CONFIG_INIT;
    rcs_capture_config v_capture_config = RCS_CAPTURE_CONFIG_INIT;
    rcs_send_audio_config v_send_audio_config = RCS_SEND_AUDIO_CONFIG_INIT;
    rcs_control_config v_control_config = RCS_CONTROL_CONFIG_INIT;
    rcs_viewers_config v_viewers_config = RCS_VIEWERS_CONFIG_INIT;
    rcs_view_request_config v_view_request_config = RCS_VIEW_REQUEST_CONFIG_INIT;
    rcs_decoder_config v_decoder_config = RCS_DECODER_CONFIG_INIT;
    rcs_receive_audio_config v_receive_audio_config = RCS_RECEIVE_AUDIO_CONFIG_INIT;
    rcs_ice_servers_config v_ice_servers_config = RCS_ICE_SERVERS_CONFIG_INIT;
    rcs_reconnect_config v_reconnect_config = RCS_RECONNECT_CONFIG_INIT;
    rcs_log_config v_log_config = RCS_LOG_CONFIG_INIT;
    rcs_module_config v_module_config = RCS_MODULE_CONFIG_INIT;
    rcs_device_identity_config v_device_identity_config = RCS_DEVICE_IDENTITY_CONFIG_INIT;
    rcs_session_config v_session_config = RCS_SESSION_CONFIG_INIT;
    rcs_data_message v_data_message = RCS_DATA_MESSAGE_INIT;
    rcs_frame v_frame = RCS_FRAME_INIT;
    rcs_audio_frame v_audio_frame = RCS_AUDIO_FRAME_INIT;
    rcs_encoded_packet v_encoded_packet = RCS_ENCODED_PACKET_INIT;
    rcs_input_event v_input_event = RCS_INPUT_EVENT_INIT;
    rcs_stream_info v_stream_info = RCS_STREAM_INFO_INIT;
    rcs_peer_info v_peer_info = RCS_PEER_INFO_INIT;
    rcs_agent_info v_agent_info = RCS_AGENT_INFO_INIT;
    rcs_desktop_info v_desktop_info = RCS_DESKTOP_INFO_INIT;
    rcs_control_info v_control_info = RCS_CONTROL_INFO_INIT;
    rcs_viewer_info v_viewer_info = RCS_VIEWER_INFO_INIT;
    rcs_viewer_decision v_viewer_decision = RCS_VIEWER_DECISION_INIT;
    rcs_capture_stats v_capture_stats = RCS_CAPTURE_STATS_INIT;
    rcs_video_send_stats v_video_send_stats = RCS_VIDEO_SEND_STATS_INIT;
    rcs_audio_send_stats v_audio_send_stats = RCS_AUDIO_SEND_STATS_INIT;
    rcs_video_receive_stats v_video_receive_stats = RCS_VIDEO_RECEIVE_STATS_INIT;
    rcs_audio_receive_stats v_audio_receive_stats = RCS_AUDIO_RECEIVE_STATS_INIT;
    rcs_network_stats v_network_stats = RCS_NETWORK_STATS_INIT;
    rcs_input_stats v_input_stats = RCS_INPUT_STATS_INIT;
    rcs_data_stats v_data_stats = RCS_DATA_STATS_INIT;
    rcs_module_stats v_module_stats = RCS_MODULE_STATS_INIT;
    rcs_session_callbacks v_session_callbacks = RCS_SESSION_CALLBACKS_INIT;
    rcs_video_tuning_config v_video_tuning_config = RCS_VIDEO_TUNING_CONFIG_INIT;
    rcs_audio_tuning_config v_audio_tuning_config = RCS_AUDIO_TUNING_CONFIG_INIT;
    rcs_network_tuning_config v_network_tuning_config = RCS_NETWORK_TUNING_CONFIG_INIT;
    rcs_clipboard_content v_clipboard_content = RCS_CLIPBOARD_CONTENT_INIT;
    rcs_clipboard_settings v_clipboard_settings = RCS_CLIPBOARD_SETTINGS_INIT;
    rcs_clipboard_peer v_clipboard_peer = RCS_CLIPBOARD_PEER_INIT;
    rcs_clipboard_stats v_clipboard_stats = RCS_CLIPBOARD_STATS_INIT;
    rcs_text_caret_settings v_text_caret_settings = RCS_TEXT_CARET_SETTINGS_INIT;
    rcs_text_caret_peer v_text_caret_peer = RCS_TEXT_CARET_PEER_INIT;
    rcs_os_session_info v_os_session_info = RCS_OS_SESSION_INFO_INIT;
    rcs_supervisor_config v_supervisor_config = RCS_SUPERVISOR_CONFIG_INIT;
    rcs_supervisor_callbacks v_supervisor_callbacks = RCS_SUPERVISOR_CALLBACKS_INIT;
    rcs_supervisor_status v_supervisor_status = RCS_SUPERVISOR_STATUS_INIT;
    rcs_power_settings v_power_settings = RCS_POWER_SETTINGS_INIT;
    rcs_power_status v_power_status = RCS_POWER_STATUS_INIT;
    rcs_power_wake v_power_wake = RCS_POWER_WAKE_INIT;
    rcs_power_capabilities v_power_capabilities = RCS_POWER_CAPABILITIES_INIT;
    rcs_power_peer v_power_peer = RCS_POWER_PEER_INIT;
    rcs_file_transfer_settings v_file_transfer_settings = RCS_FILE_TRANSFER_SETTINGS_INIT;
    rcs_file_offer v_file_offer = RCS_FILE_OFFER_INIT;
    rcs_file_transfer_info v_file_transfer_info = RCS_FILE_TRANSFER_INFO_INIT;
    rcs_file_peer v_file_peer = RCS_FILE_PEER_INIT;
    rcs_audio_session_config v_audio_session_config = RCS_AUDIO_SESSION_CONFIG_INIT;
    rcs_background_config v_background_config = RCS_BACKGROUND_CONFIG_INIT;
    rcs_streams_config v_streams_config = RCS_STREAMS_CONFIG_INIT;
    rcs_encodings_config v_encodings_config = RCS_ENCODINGS_CONFIG_INIT;
    rcs_paths_config v_paths_config = RCS_PATHS_CONFIG_INIT;
    rcs_forwarder_config v_forwarder_config = RCS_FORWARDER_CONFIG_INIT;
    rcs_viewer_video v_viewer_video = RCS_VIEWER_VIDEO_INIT;
    rcs_group_stats v_group_stats = RCS_GROUP_STATS_INIT;
    rcs_viewer_video_stats v_viewer_video_stats = RCS_VIEWER_VIDEO_STATS_INIT;
    rcs_forwarder_stats v_forwarder_stats = RCS_FORWARDER_STATS_INIT;
    rcs_hardware_buffer_info v_hardware_buffer_info = RCS_HARDWARE_BUFFER_INFO_INIT;
    rcs_clipboard_files v_clipboard_files = RCS_CLIPBOARD_FILES_INIT;
    rcs_file_publication v_file_publication = RCS_FILE_PUBLICATION_INIT;
    rcs_file_fetch_request v_file_fetch_request = RCS_FILE_FETCH_REQUEST_INIT;
    int sized = 1;
    sized = sized && sizeof(v_file_transfer_settings) > 0 && sizeof(v_file_offer) > 0 &&
            sizeof(v_file_transfer_info) > 0 && sizeof(v_file_peer) > 0;
    sized = sized && sizeof(v_power_settings) > 0 && sizeof(v_power_status) > 0;
    sized = sized && sizeof(v_power_wake) > 0 && sizeof(v_power_capabilities) > 0 && sizeof(v_power_peer) > 0;
    sized = sized && sizeof(v_agent_link_config) > 0;
    sized = sized && sizeof(v_agent_config) > 0;
    sized = sized && sizeof(v_agent_callbacks) > 0;
    sized = sized && sizeof(v_gpu_frame_info) > 0;
    sized = sized && sizeof(v_sample_buffer_info) > 0;
    sized = sized && sizeof(v_gpu_device_config) > 0;
    sized = sized && sizeof(v_host_encoder_config) > 0;
    sized = sized && sizeof(v_host_decoder_config) > 0;
    sized = sized && sizeof(v_encoded_output_config) > 0;
    sized = sized && sizeof(v_encoded_audio) > 0;
    sized = sized && sizeof(v_host_transport_config) > 0;
    sized = sized && sizeof(v_module_shared) > 0;
    sized = sized && sizeof(v_picture) > 0;
    sized = sized && sizeof(v_rate_decider_config) > 0;
    sized = sized && sizeof(v_capabilities) > 0;
    sized = sized && sizeof(v_share_video_config) > 0;
    sized = sized && sizeof(v_encoder_config) > 0;
    sized = sized && sizeof(v_capture_config) > 0;
    sized = sized && sizeof(v_send_audio_config) > 0;
    sized = sized && sizeof(v_control_config) > 0;
    sized = sized && sizeof(v_viewers_config) > 0;
    sized = sized && sizeof(v_view_request_config) > 0;
    sized = sized && sizeof(v_decoder_config) > 0;
    sized = sized && sizeof(v_receive_audio_config) > 0;
    sized = sized && sizeof(v_ice_servers_config) > 0;
    sized = sized && sizeof(v_reconnect_config) > 0;
    sized = sized && sizeof(v_log_config) > 0;
    sized = sized && sizeof(v_module_config) > 0;
    sized = sized && sizeof(v_device_identity_config) > 0;
    sized = sized && sizeof(v_session_config) > 0;
    sized = sized && sizeof(v_data_message) > 0;
    sized = sized && sizeof(v_frame) > 0;
    sized = sized && sizeof(v_audio_frame) > 0;
    sized = sized && sizeof(v_encoded_packet) > 0;
    sized = sized && sizeof(v_input_event) > 0;
    sized = sized && sizeof(v_stream_info) > 0;
    sized = sized && sizeof(v_peer_info) > 0;
    sized = sized && sizeof(v_agent_info) > 0;
    sized = sized && sizeof(v_desktop_info) > 0;
    sized = sized && sizeof(v_control_info) > 0;
    sized = sized && sizeof(v_viewer_info) > 0;
    sized = sized && sizeof(v_viewer_decision) > 0;
    sized = sized && sizeof(v_capture_stats) > 0;
    sized = sized && sizeof(v_video_send_stats) > 0;
    sized = sized && sizeof(v_audio_send_stats) > 0;
    sized = sized && sizeof(v_video_receive_stats) > 0;
    sized = sized && sizeof(v_audio_receive_stats) > 0;
    sized = sized && sizeof(v_network_stats) > 0;
    sized = sized && sizeof(v_input_stats) > 0;
    sized = sized && sizeof(v_data_stats) > 0;
    sized = sized && sizeof(v_module_stats) > 0;
    sized = sized && sizeof(v_session_callbacks) > 0;
    sized = sized && sizeof(v_video_tuning_config) > 0;
    sized = sized && sizeof(v_audio_tuning_config) > 0;
    sized = sized && sizeof(v_network_tuning_config) > 0;
    sized = sized && sizeof(v_clipboard_content) > 0;
    sized = sized && sizeof(v_clipboard_settings) > 0;
    sized = sized && sizeof(v_clipboard_peer) > 0;
    sized = sized && sizeof(v_clipboard_stats) > 0;
    sized = sized && sizeof(v_text_caret_settings) > 0;
    sized = sized && sizeof(v_text_caret_peer) > 0;
    sized = sized && sizeof(v_os_session_info) > 0;
    sized = sized && sizeof(v_supervisor_config) > 0;
    sized = sized && sizeof(v_supervisor_callbacks) > 0;
    sized = sized && sizeof(v_supervisor_status) > 0;
    sized = sized && sizeof(v_audio_session_config) > 0 && sizeof(v_background_config) > 0;
    sized = sized && sizeof(v_streams_config) > 0 && sizeof(v_encodings_config) > 0 && sizeof(v_paths_config) > 0;
    sized = sized && sizeof(v_forwarder_config) > 0 && sizeof(v_viewer_video) > 0 && sizeof(v_group_stats) > 0;
    sized = sized && sizeof(v_viewer_video_stats) > 0 && sizeof(v_forwarder_stats) > 0;
    sized = sized && sizeof(v_hardware_buffer_info) > 0 && sizeof(v_clipboard_files) > 0;
    sized = sized && sizeof(v_file_publication) > 0 && sizeof(v_file_fetch_request) > 0;
    return sized;
}
