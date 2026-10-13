// The public headers, compiled as C++ with warnings as errors (see
// CMakeLists), together with public_headers.c for C.
#include <rcsdk/agent.hpp>
#include <rcsdk/android.hpp>
#include <rcsdk/apple.hpp>
#include <rcsdk/clipboard.h>
#include <rcsdk/file_transfer.h>
#include <rcsdk/gpu.hpp>
#include <rcsdk/host_codec.hpp>
#include <rcsdk/host_transport.hpp>
#include <rcsdk/rate_control.hpp>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/supervisor.hpp>
#include <rcsdk/tuning.hpp>
#include <rcsdk/view_geometry.hpp>

extern "C" int publicHeadersC(void);

int main() {
    rcs_capture_stats captured = RCS_CAPTURE_STATS_INIT;
    rcs_video_send_stats sent = RCS_VIDEO_SEND_STATS_INIT;
    rcs_video_receive_stats received = RCS_VIDEO_RECEIVE_STATS_INIT;
    rcs_network_stats network = RCS_NETWORK_STATS_INIT;
    rcs_input_stats inputs = RCS_INPUT_STATS_INIT;
    rcs_stream_info stream = RCS_STREAM_INFO_INIT;
    rcs_audio_send_stats audioSent = RCS_AUDIO_SEND_STATS_INIT;
    rcs_audio_receive_stats audioReceived = RCS_AUDIO_RECEIVE_STATS_INIT;
    const bool sized = captured.struct_size && sent.struct_size && received.struct_size && network.struct_size &&
                       inputs.struct_size && stream.struct_size && audioSent.struct_size && audioReceived.struct_size;
    return sized && publicHeadersC() ? 0 : 1;
}
