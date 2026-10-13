// C ABI: validates arguments and turns exceptions into result codes.
#include "agent/agent_server.hpp"
#include "agent/host_encoder.hpp"
#include "crypto/crypto.hpp"
#include "media/gpu_frame.hpp"
#include "modules/module.hpp"
#include "modules/registry.hpp"
#include "platform/platform.hpp"
#include "seams/seams.hpp"
#include "session/path_info.hpp"
#include "session/session.hpp"
#include "session/thumbnail.hpp"
#include "transport/local_stream.hpp"
#include "transport/network_changes.hpp"
#include "util/api.hpp"
#include "util/log.hpp"

#include <rcsdk/module.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <tuple>
#include <type_traits>
#include <vector>

struct rcs_session_impl {
    rcsdk::session::Session session;
    rcs_session_impl(rcsdk::session::SessionConfig config, const rcs_session_callbacks& callbacks)
        : session(std::move(config), callbacks,
                  rcsdk::log::makeSource(RCS_LOG_SOURCE_SESSION, this, callbacks.user_data, 0)) {}
};

// An agent: the server that attaches to a session, and the host's state callback.
struct rcs_agent_impl {
    rcs_agent_callbacks callbacks;
    rcsdk::log::SourceRef log;
    rcsdk::agent::AgentServer server;
    rcs_agent_impl(const rcs_agent_config& config, const rcs_agent_callbacks& table,
                   const rcsdk::session::AgentExtensions& extensions);
    static thread_local const rcs_agent_impl* current; // The agent whose callback this thread is in.
};
thread_local const rcs_agent_impl* rcs_agent_impl::current = nullptr;

namespace {

rcsdk::agent::AgentServer::Options agentOptions(const rcs_agent_config& config,
                                                const rcsdk::session::AgentExtensions& extensions) {
    rcsdk::agent::AgentServer::Options options;
    options.hostEncoder = extensions.hostEncoder;
    options.endpoint = config.endpoint;
    std::copy(std::begin(config.key), std::end(config.key), options.key.begin());
    options.access = config.access == RCS_AGENT_ACCESS_ANY_USER ? rcsdk::transport::LocalAccess::anyUser
                                                                : rcsdk::transport::LocalAccess::privileged;
    options.reconnect = config.reconnect != 0;
    options.reconnectInterval = std::chrono::milliseconds(config.reconnect_interval_ms);
    return options;
}

} // namespace

rcs_agent_impl::rcs_agent_impl(const rcs_agent_config& config, const rcs_agent_callbacks& table,
                               const rcsdk::session::AgentExtensions& extensions)
    : callbacks(table),
      log(rcsdk::log::makeSource(RCS_LOG_SOURCE_AGENT, this, table.user_data, extensions.logLevel)),
      server(agentOptions(config, extensions), [this](rcs_state state, rcs_result result) {
          if (!callbacks.on_event) {
              return;
          }
          rcs_agent_event event{};
          event.struct_size = sizeof(event);
          event.type = RCS_AGENT_EVENT_STATE;
          event.timestamp_ns = rcsdk::monotonicNs();
          event.state.value = state;
          event.state.result = result;
          const rcs_agent_impl* previous = current;
          current = this;
          callbacks.on_event(&event, callbacks.user_data);
          current = previous;
      }) {}

namespace {

using rcsdk::copyText;
using rcsdk::Failure;

// rcs_get_last_call_error: why this thread's most recent failing call failed.
thread_local std::string callError;

// Every call starts without a reason, so a failure never shows an older one.
void beginCall() noexcept {
    callError.clear();
}

rcs_result refuse(rcs_result result, std::string reason) noexcept {
    try {
        callError = std::move(reason);
    } catch (...) {
    }
    return result;
}

template <class Function>
rcs_result guarded(Function&& function) noexcept {
    try {
        return function();
    } catch (const Failure& failure) {
        return refuse(failure.result(), failure.what());
    } catch (const std::exception& exception) {
        return refuse(RCS_ERROR_INTERNAL, exception.what());
    } catch (...) {
        return refuse(RCS_ERROR_INTERNAL, "Unexpected internal error");
    }
}

// A structure of another size is uninitialised or comes from other headers.
template <class T>
bool sized(const T* value) noexcept {
    return value && value->struct_size == sizeof(T);
}

std::string initialise(const char* structure, const char* macro) {
    return std::string("Initialise ") + structure + " with " + macro + " from the headers of this library (" +
           RCSDK_VERSION_STRING + ")";
}

template <class T>
bool validList(const T* items, uint32_t itemSize, uint32_t capacity, const uint32_t* outCount) noexcept {
    return outCount && itemSize == sizeof(T) && (items || !capacity);
}

// The two-pass list protocol.
template <class T>
rcs_result writeList(const std::vector<T>& values, T* items, uint32_t capacity, uint32_t* outCount) {
    *outCount = uint32_t(values.size());
    if (!items) {
        return RCS_OK;
    }
    if (capacity < values.size()) {
        return RCS_ERROR_BUFFER_TOO_SMALL;
    }
    std::copy(values.begin(), values.end(), items);
    return RCS_OK;
}

// A picture into the caller's buffer: its size always, its pixels when they fit.
rcs_result writePicture(const rcsdk::platform::Thumbnail& picture, rcs_picture& out) {
    out.width = picture.width;
    out.height = picture.height;
    out.stride_bytes = picture.width * 4;
    out.format = RCS_PIXEL_BGRA8;
    out.data_size = picture.pixels.size();
    if (out.capacity < out.data_size) {
        return refuse(RCS_ERROR_BUFFER_TOO_SMALL, "The picture takes " + std::to_string(out.data_size) +
                                                      " bytes; data holds " + std::to_string(out.capacity));
    }
    std::memcpy(out.data, picture.pixels.data(), picture.pixels.size());
    return RCS_OK;
}

// Lifecycle calls from the session's own callbacks would deadlock, and from
// the log callback too: it holds up every thread that logs.
bool fromCallback(rcs_session session) noexcept {
    return session->session.onCallbackThread() || rcsdk::log::writing();
}

// The same for an agent: its own state callback, or the log callback.
bool fromAgentCallback(rcs_agent agent) noexcept {
    return rcs_agent_impl::current == agent || rcsdk::log::writing();
}

rcs_result agentCallbackRefused() noexcept {
    return refuse(RCS_ERROR_CALLBACK_THREAD, "Start, stop, wait and destroy are refused from the agent's own "
                                             "callback and from the log callback; dispatch them to another thread");
}

rcs_result fromCallbackRefused() noexcept {
    return refuse(RCS_ERROR_CALLBACK_THREAD, "Start, stop and destroy are refused from the session's own callbacks "
                                             "and from the log callback; dispatch them to another thread");
}

// What a call on an object logs belongs to that object (rcs_log_record).
rcsdk::log::Scope logAs(rcs_session session) noexcept {
    return rcsdk::log::Scope(session->session.logSource());
}
rcsdk::log::Scope logAs(rcs_agent agent) noexcept {
    return rcsdk::log::Scope(agent->log);
}

// An information structure of the session (rcs_session_get_*_info).
template <class Info, class Get>
rcs_result getInfo(rcs_session session, Info* info, const char* structure, const char* macro, Get&& get) {
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    if (!sized(info)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise(structure, macro));
    }
    return guarded([&] {
        *info = get(session->session);
        return RCS_OK;
    });
}

// A permission request's answer, with the reason when it is not RCS_OK.
rcs_result permissionAnswer(rcs_result result, const char* what) {
    switch (result) {
    case RCS_OK:
        return RCS_OK;
    case RCS_ERROR_WRONG_THREAD:
        return refuse(result, std::string("Ask for ") + what + " on the application's main thread");
    case RCS_ERROR_UNSUPPORTED:
        return refuse(result, std::string("No permission for ") + what +
                                  " can be asked for here (the system has none to give this process)");
    default:
        return refuse(result, std::string("The system does not grant ") + what +
                                  " (the user decides in the system's settings)");
    }
}

rcs_result pauseSession(rcs_session session, uint32_t what, bool paused) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    if (what & ~(RCS_PAUSE_VIDEO | RCS_PAUSE_AUDIO)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "what holds a bit that is no RCS_PAUSE_*");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.pause(what, paused);
        return RCS_OK;
    });
}

} // namespace

extern "C" {

uint32_t rcs_version_number(void) {
    return RCSDK_VERSION;
}

const char* rcs_version(void) {
    return RCSDK_VERSION_STRING;
}

const char* rcs_result_message(rcs_result result) {
    switch (result) {
    case RCS_OK:
        return "Success";
    case RCS_ERROR_INVALID_ARGUMENT:
        return "Invalid argument";
    case RCS_ERROR_INVALID_STATE:
        return "Not allowed in the session's current state";
    case RCS_ERROR_UNSUPPORTED:
        return "Not supported on this system";
    case RCS_ERROR_CALLBACK_THREAD:
        return "Control the session outside its callbacks";
    case RCS_ERROR_INTERNAL:
        return "Internal error";
    case RCS_ERROR_PERMISSION_REQUIRED:
        return "Operating system permission is required";
    case RCS_ERROR_CAPTURE:
        return "Screen capture failed";
    case RCS_ERROR_CODEC:
        return "Video codec failed";
    case RCS_ERROR_CANCELLED:
        return "Operation cancelled";
    case RCS_ERROR_BUFFER_TOO_SMALL:
        return "Buffer is too small";
    case RCS_ERROR_WRONG_THREAD:
        return "Call this on the application's main thread";
    case RCS_ERROR_NETWORK:
        return "Network connection failed";
    case RCS_ERROR_AUTHENTICATION:
        return "Connection authentication failed";
    case RCS_ERROR_TIMEOUT:
        return "Operation timed out";
    case RCS_ERROR_DEVICE:
        return "A system resource is missing, held by another program or failed";
    case RCS_ERROR_DISCONNECTED:
        return "The peer closed the connection";
    case RCS_ERROR_RESTARTING:
        return "The peer closed the connection and will be back";
    case RCS_ERROR_QUEUE_FULL:
        return "The send queue is full";
    case RCS_ERROR_PROTOCOL:
        return "The peer broke the protocol";
    case RCS_ERROR_BUSY:
        return "The sharer cannot take this viewer now";
    case RCS_ERROR_REPLACED:
        return "Another agent took this one's place";
    case RCS_ERROR_REFUSED:
        return "The sharer's host refused it";
    default:
        return "Unknown result";
    }
}

rcs_result rcs_get_last_call_error(char* buffer, uint32_t capacity) {
    if (!buffer || !capacity) {
        return RCS_ERROR_INVALID_ARGUMENT; // Leaves the reason being asked for in place.
    }
    copyText(buffer, capacity, callError);
    return RCS_OK;
}

rcs_result rcs_set_last_call_error(rcs_result result, const char* reason) {
    beginCall();
    return result == RCS_OK ? RCS_OK : refuse(result, reason ? reason : "");
}

rcs_result rcs_get_capabilities(rcs_capabilities* capabilities) {
    beginCall();
    if (!sized(capabilities)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_capabilities", "RCS_CAPABILITIES_INIT"));
    }
    return guarded([&] {
        *capabilities = rcsdk::session::capabilities();
        // Android's facts come with the Java VM, which the host may hand over at any time.
        capabilities->network_facts = rcsdk::seams::networkFacts();
        capabilities->wifi_facts = rcsdk::seams::wifiFacts();
        return RCS_OK;
    });
}

rcs_result rcs_list_codec_implementations(rcs_codec_implementation* items, uint32_t item_size, uint32_t capacity,
                                          uint32_t* out_count) {
    beginCall();
    if (!validList(items, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass item_size = sizeof(rcs_codec_implementation) and out_count");
    }
    return guarded([&] {
        std::vector<rcs_codec_implementation> list;
        for (const auto& source : rcsdk::session::codecImplementations()) {
            rcs_codec_implementation& item = list.emplace_back();
            item.codec = source.codec;
            item.direction = source.direction;
            item.acceleration = source.acceleration;
            item.backend = source.backend;
            item.adapter_luid = source.adapterLuid;
            item.max_width = source.maxWidth;
            item.max_height = source.maxHeight;
            std::copy(source.maxPixelRates.begin(), source.maxPixelRates.end(), item.max_pixels_per_second);
            item.picture_formats = source.pictureFormats;
            item.ref_invalidation = source.refInvalidation ? 1 : 0;
            item.temporal_layers = source.direction == RCS_CODEC_ENCODER ? source.temporalLayers : 0;
            copyText(item.name, source.name);
        }
        return writeList(list, items, capacity, out_count);
    });
}

rcs_result rcs_list_displays(rcs_display_info* displays, uint32_t item_size, uint32_t capacity, uint32_t* out_count) {
    beginCall();
    if (!validList(displays, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass item_size = sizeof(rcs_display_info) and out_count");
    }
    return guarded([&] { return writeList(rcsdk::platform::displays(), displays, capacity, out_count); });
}

rcs_result rcs_list_windows(rcs_window_info* windows, uint32_t item_size, uint32_t capacity, uint32_t* out_count) {
    beginCall();
    if (!validList(windows, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass item_size = sizeof(rcs_window_info) and out_count");
    }
    return guarded([&] { return writeList(rcsdk::seams::windows(), windows, capacity, out_count); });
}

rcs_result rcs_list_networks(rcs_network_info* networks, uint32_t item_size, uint32_t capacity, uint32_t* out_count) {
    beginCall();
    if (!validList(networks, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass item_size = sizeof(rcs_network_info) and out_count");
    }
    return guarded([&] { return writeList(rcsdk::session::networks(), networks, capacity, out_count); });
}

rcs_result rcs_list_wifi(rcs_wifi_info* networks, uint32_t item_size, uint32_t capacity, uint32_t* out_count) {
    beginCall();
    if (!validList(networks, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass item_size = sizeof(rcs_wifi_info) and out_count");
    }
    return guarded([&] { return writeList(rcsdk::session::wifi(), networks, capacity, out_count); });
}

rcs_result rcs_notify_network_change(void) {
    beginCall();
    rcsdk::transport::networkChanged();
    return RCS_OK;
}

rcs_result rcs_generate_connection_key(uint8_t* key, uint32_t size) {
    beginCall();
    if (!key || size != 32) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "key must hold 32 bytes");
    }
    return guarded([&] {
        rcsdk::crypto::random({key, size});
        return RCS_OK;
    });
}

rcs_result rcs_generate_pin(char* out, uint32_t capacity, uint32_t digits) {
    beginCall();
    if (!out || digits < 4 || digits > 32) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass out and 4..32 digits");
    }
    if (capacity < digits + 1) {
        return refuse(RCS_ERROR_BUFFER_TOO_SMALL, "out must hold digits + 1 characters");
    }
    return guarded([&] {
        for (uint32_t i = 0; i < digits; ++i) {
            uint8_t byte = 0;
            do { // 250 of the 256 values, so that each digit is equally likely.
                rcsdk::crypto::random({&byte, 1});
            } while (byte >= 250);
            out[i] = char('0' + byte % 10);
        }
        out[digits] = '\0';
        return RCS_OK;
    });
}

rcs_result rcs_generate_device_key(uint8_t* device_key, uint32_t size) {
    beginCall();
    if (!device_key || size != RCS_DEVICE_KEY_SIZE) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "device_key must hold RCS_DEVICE_KEY_SIZE bytes");
    }
    return guarded([&] {
        const auto key = rcsdk::crypto::DeviceKey::generate();
        std::copy(key.begin(), key.end(), device_key);
        return RCS_OK;
    });
}

rcs_result rcs_device_public_key(const uint8_t* device_key, uint32_t device_key_size, uint8_t* public_key,
                                 uint32_t public_key_size) {
    beginCall();
    if (!device_key || !public_key || device_key_size != RCS_DEVICE_KEY_SIZE ||
        public_key_size != RCS_PUBLIC_KEY_SIZE) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      "Pass RCS_DEVICE_KEY_SIZE bytes of device key and room for RCS_PUBLIC_KEY_SIZE bytes");
    }
    return guarded([&] {
        const rcsdk::crypto::DeviceKey key(std::span(device_key, device_key_size));
        std::copy(key.publicKey().begin(), key.publicKey().end(), public_key);
        return RCS_OK;
    });
}

rcs_result rcs_set_log_callback(rcs_log_level max_level, rcs_log_callback callback, void* user_data) {
    beginCall();
    if (callback && (max_level < RCS_LOG_ERROR || max_level > RCS_LOG_DEBUG)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "max_level must be RCS_LOG_ERROR..RCS_LOG_DEBUG");
    }
    return rcsdk::log::setCallback(max_level, callback, user_data);
}

rcs_result rcs_log_write(const rcs_log_record* record) {
    beginCall();
    if (!sized(record) || record->level < RCS_LOG_ERROR || record->level > RCS_LOG_DEBUG || !record->module ||
        !record->message) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_log_record: struct_size sizeof(rcs_log_record), level "
                                                  "RCS_LOG_ERROR..RCS_LOG_DEBUG, a module and a message");
    }
    const rcs_log_record& r = *record;
    const int handles = (r.session != nullptr) + (r.agent != nullptr) + (r.supervisor != nullptr);
    rcs_log_level own = 0;
    bool named = false; // The source's handle, and no other.
    switch (r.source) {
    case RCS_LOG_SOURCE_PROCESS:
        named = !handles;
        break;
    case RCS_LOG_SOURCE_SESSION:
        named = r.session && handles == 1;
        own = named ? r.session->session.logSource()->level.load() : 0;
        break;
    case RCS_LOG_SOURCE_AGENT:
        named = r.agent && handles == 1;
        own = named ? r.agent->log->level.load() : 0;
        break;
    case RCS_LOG_SOURCE_SUPERVISOR:
        named = r.supervisor && handles == 1;
        break;
    default:
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_log_record: no version defines that source");
    }
    if (!named) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      "rcs_log_record: the handle of its source (session, agent or supervisor) and no other; none "
                      "for PROCESS");
    }
    rcsdk::log::write(r, own);
    return RCS_OK;
}

int rcs_log_in_callback(void) {
    return rcsdk::log::writing() ? 1 : 0;
}

rcs_permission rcs_screen_capture_permission(void) {
    return rcsdk::platform::screenPermission();
}
rcs_permission rcs_input_permission(void) {
    return rcsdk::platform::inputPermission();
}
rcs_result rcs_request_screen_capture_permission(void) {
    beginCall();
    return guarded([] { return permissionAnswer(rcsdk::platform::requestScreenPermission(), "screen capture"); });
}
rcs_result rcs_request_input_permission(void) {
    beginCall();
    return guarded([] { return permissionAnswer(rcsdk::platform::requestInputPermission(), "remote input"); });
}
rcs_permission rcs_microphone_permission(void) {
    return rcsdk::platform::microphonePermission();
}
rcs_result rcs_request_microphone_permission(void) {
    beginCall();
    return guarded([] { return permissionAnswer(rcsdk::platform::requestMicrophonePermission(), "the microphone"); });
}

rcs_result rcs_list_audio_devices(rcs_audio_device_info* devices, uint32_t item_size, uint32_t capacity,
                                  uint32_t* out_count) {
    beginCall();
    if (!validList(devices, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass item_size = sizeof(rcs_audio_device_info) and out_count");
    }
    return guarded([&] {
        std::vector<rcs_audio_device_info> list;
        for (const auto& device : rcsdk::platform::audioDevices()) {
            rcs_audio_device_info& item = list.emplace_back();
            item.device_id = device.id;
            item.input = device.input ? 1 : 0;
            item.is_default = device.isDefault ? 1 : 0;
            copyText(item.name, device.name);
        }
        return writeList(list, devices, capacity, out_count);
    });
}

rcs_result rcs_set_audio_session_config(const rcs_audio_session_config* config) {
    beginCall();
    if (!sized(config)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      initialise("rcs_audio_session_config", "RCS_AUDIO_SESSION_CONFIG_INIT"));
    }
    if (config->manager != RCS_AUDIO_SESSION_SDK && config->manager != RCS_AUDIO_SESSION_HOST) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "manager must be RCS_AUDIO_SESSION_SDK or RCS_AUDIO_SESSION_HOST");
    }
    if (config->mix_with_others > 1 || config->default_to_speaker > 1 || config->bluetooth_microphone > 1) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      "mix_with_others, default_to_speaker and bluetooth_microphone must be 0 or 1");
    }
    return guarded([&] {
        rcsdk::platform::setAudioSessionConfig(*config);
        return RCS_OK;
    });
}

rcs_result rcs_get_audio_session_config(rcs_audio_session_config* config) {
    beginCall();
    if (!sized(config)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      initialise("rcs_audio_session_config", "RCS_AUDIO_SESSION_CONFIG_INIT"));
    }
    return guarded([&] {
        *config = rcsdk::platform::audioSessionConfig();
        return RCS_OK;
    });
}

rcs_result rcs_session_create(const rcs_session_config* config, const rcs_session_callbacks* callbacks,
                              rcs_session* out_session) {
    beginCall();
    if (!out_session || !config) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "config and out_session are required");
    }
    *out_session = nullptr;
    rcs_session_callbacks table = RCS_SESSION_CALLBACKS_INIT;
    if (callbacks) {
        if (!sized(callbacks)) {
            return refuse(RCS_ERROR_INVALID_ARGUMENT,
                          initialise("rcs_session_callbacks", "RCS_SESSION_CALLBACKS_INIT"));
        }
        table = *callbacks;
        if (table.on_frame && table.on_gpu_frame) {
            return refuse(RCS_ERROR_INVALID_ARGUMENT, "Set at most one of on_frame and on_gpu_frame");
        }
    }
    return guarded([&] {
        *out_session = new rcs_session_impl(rcsdk::session::parseConfig(*config), table);
        (*out_session)->session.modules().setHandle(*out_session);
        return RCS_OK;
    });
}

rcs_result rcs_session_start(rcs_session session) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    if (fromCallback(session)) {
        return fromCallbackRefused();
    }
    const auto scope = logAs(session);
    return guarded([&] {
        const rcs_result result = session->session.start();
        return result == RCS_OK ? result : refuse(result, session->session.failure().message);
    });
}

rcs_result rcs_session_stop(rcs_session session) {
    return rcs_session_stop_with_reason(session, RCS_STOP_NORMAL);
}

rcs_result rcs_session_stop_with_reason(rcs_session session, rcs_stop_reason reason) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    if (reason != RCS_STOP_NORMAL && reason != RCS_STOP_RESTART) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Unknown stop reason " + std::to_string(reason));
    }
    if (fromCallback(session)) {
        return fromCallbackRefused();
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.stop(reason);
        return RCS_OK;
    });
}

rcs_result rcs_session_destroy(rcs_session session) {
    beginCall();
    if (!session) {
        return RCS_OK;
    }
    if (fromCallback(session)) {
        return fromCallbackRefused();
    }
    const auto source = session->session.logSource();
    const rcs_result result = guarded([&] {
        const auto scope = logAs(session);
        delete session;
        return RCS_OK;
    });
    rcsdk::log::end(source);
    return result;
}

rcs_result rcs_session_update(rcs_session session, const rcs_extension* changes) {
    beginCall();
    if (!session || !changes) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and changes are required");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.update(changes);
        return RCS_OK;
    });
}

rcs_result rcs_preset_defaults(rcs_preset preset, rcs_extension* extension) {
    beginCall();
    if (!extension) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "extension is required");
    }
    return guarded([&] {
        rcsdk::session::presetDefaults(preset, *extension);
        return RCS_OK;
    });
}

rcs_result rcs_session_get_settings(rcs_session session, rcs_extension* extension) {
    beginCall();
    if (!session || !extension) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and extension are required");
    }
    return guarded([&] {
        rcsdk::session::getSettings(session->session.settings(), *extension);
        return RCS_OK;
    });
}

rcs_result rcs_session_set_preset(rcs_session session, rcs_preset preset) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is required");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.setPreset(preset);
        return RCS_OK;
    });
}

rcs_result rcs_session_set_identity(rcs_session session, const rcs_identity* identity) {
    beginCall();
    if (!session || !identity) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and identity are required");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.setIdentity(*identity);
        return RCS_OK;
    });
}

rcs_result rcs_session_get_state(rcs_session session, rcs_state* out_state) {
    beginCall();
    if (!session || !out_state) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and out_state are required");
    }
    *out_state = session->session.state();
    return RCS_OK;
}

rcs_result rcs_session_get_failure(rcs_session session, rcs_result* out_result, char* message, uint32_t capacity) {
    beginCall();
    if (!session || !out_result || (!message && capacity)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and out_result are required; message with a capacity");
    }
    return guarded([&] {
        const auto failure = session->session.failure();
        *out_result = failure.result;
        if (capacity) {
            copyText(message, capacity, failure.message);
        }
        return RCS_OK;
    });
}

rcs_result rcs_session_get_stats(rcs_session session, rcs_stats_section section, void* stats) {
    beginCall();
    if (!session || !stats) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and stats are required");
    }
    if (section == RCS_STATS_CAPTURE || section == RCS_STATS_VIDEO_SEND || section == RCS_STATS_VIDEO_RECEIVE) {
        return rcs_session_get_stream_stats(session, 0, section, stats); // Stream 0's.
    }
    const auto copy = [&](auto member) {
        using T = std::remove_reference_t<decltype(session->session.stats().*member)>;
        auto* target = static_cast<T*>(stats);
        if (!sized(target)) {
            return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("the statistics", "the section's *_STATS_INIT"));
        }
        return guarded([&] {
            *target = session->session.stats().*member;
            return RCS_OK;
        });
    };
    using rcsdk::session::Stats;
    switch (section) {
    case RCS_STATS_AUDIO_SEND: // Sound goes both ways.
        return copy(&Stats::audioSend);
    case RCS_STATS_AUDIO_RECEIVE:
        return copy(&Stats::audioReceive);
    case RCS_STATS_NETWORK:
        return copy(&Stats::network);
    case RCS_STATS_INPUT:
        return copy(&Stats::input);
    case RCS_STATS_DATA:
        return copy(&Stats::data);
    case RCS_STATS_GROUP:
        if (session->session.config().role != RCS_ROLE_SHARE) {
            return refuse(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
        }
        return copy(&Stats::group);
    case RCS_STATS_VIEWER_VIDEO:
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "VIEWER_VIDEO is one viewer's (rcs_session_get_viewer_stats)");
    case RCS_STATS_FORWARDER:
        if (session->session.config().connect.transport != RCS_TRANSPORT_FORWARDER) {
            return refuse(RCS_ERROR_UNSUPPORTED, "Only a session through a forwarder has FORWARDER statistics");
        }
        return copy(&Stats::forwarder);
    default:
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Unknown statistics section " + std::to_string(section));
    }
}

rcs_result rcs_session_get_stream_stats(rcs_session session, uint32_t stream, rcs_stats_section section,
                                        void* stats) {
    beginCall();
    if (!session || !stats) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and stats are required");
    }
    if (stream >= RCS_MAX_STREAMS) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "stream must be below RCS_MAX_STREAMS");
    }
    const bool share = session->session.config().role == RCS_ROLE_SHARE;
    const auto copy = [&](auto member, bool applies) {
        using T = std::remove_reference_t<decltype(rcsdk::session::VideoStats{}.*member)>;
        auto* target = static_cast<T*>(stats);
        if (!sized(target)) {
            return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("the statistics", "the section's *_STATS_INIT"));
        }
        if (!applies) {
            return refuse(RCS_ERROR_UNSUPPORTED, share ? "A sharing session has no such statistics"
                                                       : "A viewing session has no such statistics");
        }
        return guarded([&] {
            *target = session->session.stats().video[stream].*member;
            return RCS_OK;
        });
    };
    using rcsdk::session::VideoStats;
    switch (section) {
    case RCS_STATS_CAPTURE:
        return copy(&VideoStats::capture, share);
    case RCS_STATS_VIDEO_SEND:
        return copy(&VideoStats::send, share);
    case RCS_STATS_VIDEO_RECEIVE:
        return copy(&VideoStats::receive, !share);
    default:
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "A stream has the sections CAPTURE, VIDEO_SEND and VIDEO_RECEIVE; "
                                                  "the rest are rcs_session_get_stats' (section " +
                                                      std::to_string(section) + ")");
    }
}

rcs_result rcs_session_get_encoding_stats(rcs_session session, uint32_t stream, uint32_t encoding,
                                          rcs_video_send_stats* stats) {
    beginCall();
    if (!session || !sized(stats)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and stats (RCS_VIDEO_SEND_STATS_INIT) are required");
    }
    if (stream >= RCS_MAX_STREAMS || encoding >= RCS_MAX_ENCODINGS) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "stream must be below RCS_MAX_STREAMS, encoding below "
                                                  "RCS_MAX_ENCODINGS");
    }
    if (session->session.config().role != RCS_ROLE_SHARE) {
        return refuse(RCS_ERROR_UNSUPPORTED, "A viewing session encodes nothing");
    }
    return guarded([&] {
        auto video = session->session.stats().video[stream];
        if (encoding && !(video.encodings & (1u << encoding))) {
            rcsdk::fail(RCS_ERROR_INVALID_STATE, "Encoding " + std::to_string(encoding) + " of stream " +
                                              std::to_string(stream) + " does not run now");
        }
        *stats = video.sendOf(encoding);
        return RCS_OK;
    });
}

rcs_result rcs_session_get_stream_info(rcs_session session, uint32_t stream, rcs_stream_info* info) {
    beginCall();
    if (stream >= RCS_MAX_STREAMS) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "stream must be below RCS_MAX_STREAMS");
    }
    return getInfo(session, info, "rcs_stream_info", "RCS_STREAM_INFO_INIT",
                   [stream](const auto& s) { return s.streamInfo(stream); });
}

rcs_result rcs_session_get_peer_info(rcs_session session, rcs_peer_info* info) {
    beginCall();
    return getInfo(session, info, "rcs_peer_info", "RCS_PEER_INFO_INIT", [](const auto& s) { return s.peerInfo(); });
}

rcs_result rcs_session_get_control_info(rcs_session session, rcs_control_info* info) {
    beginCall();
    return getInfo(session, info, "rcs_control_info", "RCS_CONTROL_INFO_INIT",
                   [](const auto& s) { return s.controlInfo(); });
}

rcs_result rcs_session_get_agent_info(rcs_session session, rcs_agent_info* info) {
    beginCall();
    if (session && session->session.config().role != RCS_ROLE_SHARE) {
        return refuse(RCS_ERROR_UNSUPPORTED, "A viewing session has no agent");
    }
    return getInfo(session, info, "rcs_agent_info", "RCS_AGENT_INFO_INIT",
                   [](const auto& s) { return s.agentInfo(); });
}

rcs_result rcs_session_get_desktop_info(rcs_session session, rcs_desktop_info* info) {
    beginCall();
    return getInfo(session, info, "rcs_desktop_info", "RCS_DESKTOP_INFO_INIT",
                   [](const auto& s) { return s.desktopInfo(); });
}

rcs_result rcs_session_list_displays(rcs_session session, rcs_display_info* displays, uint32_t item_size,
                                     uint32_t capacity, uint32_t* out_count) {
    beginCall();
    if (!session || !validList(displays, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass a session, item_size = sizeof(rcs_display_info) and out_count");
    }
    if (session->session.config().role != RCS_ROLE_SHARE) {
        return refuse(RCS_ERROR_UNSUPPORTED, "A viewing session has no displays to share");
    }
    const auto scope = logAs(session);
    return guarded([&] { return writeList(session->session.displays(), displays, capacity, out_count); });
}

rcs_result rcs_session_list_windows(rcs_session session, rcs_window_info* windows, uint32_t item_size,
                                    uint32_t capacity, uint32_t* out_count) {
    beginCall();
    if (!session || !validList(windows, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass a session, item_size = sizeof(rcs_window_info) and out_count");
    }
    if (session->session.config().role != RCS_ROLE_SHARE) {
        return refuse(RCS_ERROR_UNSUPPORTED, "A viewing session has no windows to share");
    }
    const auto scope = logAs(session);
    return guarded([&] { return writeList(session->session.windows(), windows, capacity, out_count); });
}

rcs_result rcs_session_list_viewers(rcs_session session, rcs_viewer_info* viewers, uint32_t item_size,
                                    uint32_t capacity, uint32_t* out_count) {
    beginCall();
    if (!session || !validList(viewers, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass a session, item_size = sizeof(rcs_viewer_info) and out_count");
    }
    if (session->session.config().role != RCS_ROLE_SHARE) {
        return refuse(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
    }
    return guarded([&] { return writeList(session->session.viewers(), viewers, capacity, out_count); });
}

rcs_result rcs_session_get_viewer_stats(rcs_session session, rcs_viewer_id viewer, rcs_stats_section section,
                                        void* stats) {
    beginCall();
    if (!session || !stats) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and stats are required");
    }
    if (session->session.config().role != RCS_ROLE_SHARE) {
        return refuse(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
    }
    const auto copy = [&](auto member) {
        using T = std::remove_reference_t<decltype(rcsdk::session::Stats{}.*member)>;
        auto* target = static_cast<T*>(stats);
        if (!sized(target)) {
            return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("the statistics", "the section's *_STATS_INIT"));
        }
        return guarded([&] {
            *target = session->session.viewerStats(viewer).*member;
            return RCS_OK;
        });
    };
    using rcsdk::session::Stats;
    switch (section) {
    case RCS_STATS_NETWORK:
        return copy(&Stats::network);
    case RCS_STATS_AUDIO_SEND:
        return copy(&Stats::audioSend);
    case RCS_STATS_AUDIO_RECEIVE:
        return copy(&Stats::audioReceive);
    case RCS_STATS_DATA:
        return copy(&Stats::data);
    case RCS_STATS_INPUT:
        return copy(&Stats::input);
    case RCS_STATS_VIEWER_VIDEO:
        return copy(&Stats::viewerVideo);
    default:
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      "A viewer has the sections NETWORK, AUDIO_SEND, AUDIO_RECEIVE, DATA, INPUT and VIEWER_VIDEO; the "
                      "stream's are rcs_session_get_stats' (section " +
                          std::to_string(section) + ")");
    }
}

rcs_result rcs_session_get_path_info(rcs_session session, rcs_viewer_id viewer, rcs_path_info* active,
                                     rcs_path_info* standby) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    return guarded([&] {
        const auto [now, ready] = session->session.pathInfo(viewer);
        if (active) {
            *active = now;
        }
        if (standby) {
            *standby = ready;
        }
        return RCS_OK;
    });
}

rcs_result rcs_session_disconnect_viewer(rcs_session session, rcs_viewer_id viewer) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.disconnectViewer(viewer);
        return RCS_OK;
    });
}

rcs_result rcs_session_set_viewer_control(rcs_session session, rcs_viewer_id viewer, uint32_t allowed) {
    beginCall();
    if (!session || allowed > 1) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass a session, and allowed 0 or 1");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.setViewerControl(viewer, allowed != 0);
        return RCS_OK;
    });
}

rcs_result rcs_session_set_viewer_video(rcs_session session, rcs_viewer_id viewer, const rcs_viewer_video* video) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    if (!sized(video)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_viewer_video", "RCS_VIEWER_VIDEO_INIT"));
    }
    if (video->counts_for_bitrate > 1) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_viewer_video.counts_for_bitrate must be 0 or 1");
    }
    if (video->layers > RCS_SCALABILITY_L1T3) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_viewer_video.layers must be 0..3");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.setViewerVideo(viewer, *video);
        return RCS_OK;
    });
}

rcs_result rcs_session_set_main_viewer(rcs_session session, rcs_viewer_id viewer) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.setMainViewer(viewer);
        return RCS_OK;
    });
}

rcs_result rcs_session_admit_viewer(rcs_session session, rcs_viewer_id viewer, const rcs_viewer_decision* decision) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    if (!sized(decision)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_viewer_decision", "RCS_VIEWER_DECISION_INIT"));
    }
    if (decision->admission != RCS_ADMIT && decision->admission != RCS_REFUSE) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_viewer_decision.admission must be RCS_ADMIT or RCS_REFUSE");
    }
    if (decision->control > 1) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_viewer_decision.control must be 0 or 1");
    }
    if (!std::memchr(decision->reason, 0, sizeof(decision->reason))) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_viewer_decision.reason must end within its 128 bytes");
    }
    if (!sized(&decision->video) || decision->video.counts_for_bitrate > 1 ||
        (decision->video.encoding != RCS_ENCODING_ANY && decision->video.encoding >= RCS_MAX_ENCODINGS) ||
        decision->video.layers > RCS_SCALABILITY_L1T3) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_viewer_decision.video: RCS_VIEWER_VIDEO_INIT, "
                                                  "counts_for_bitrate 0 or 1, an encoding below RCS_MAX_ENCODINGS, "
                                                  "layers 0..3");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.admitViewer(viewer, *decision);
        return RCS_OK;
    });
}

rcs_result rcs_session_signal(rcs_session session, const char* signal, uint64_t route) {
    beginCall();
    if (!session || !signal) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and signal are required");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.signal(signal, route);
        return RCS_OK;
    });
}

rcs_result rcs_session_transport_receive(rcs_session session, uint64_t route, const uint8_t* data, uint32_t size) {
    beginCall();
    if (!session || !data) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and data are required");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.transportReceive(route, std::span(data, size));
        return RCS_OK;
    });
}

rcs_result rcs_session_transport_closed(rcs_session session, uint64_t route) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is required");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.transportClosed(route);
        return RCS_OK;
    });
}

rcs_result rcs_session_transport_restarted(rcs_session session, uint64_t route) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is required");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.transportRestarted(route);
        return RCS_OK;
    });
}

rcs_result rcs_session_send_input(rcs_session session, const rcs_input_event* event) {
    beginCall();
    if (!session || !event) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and event are required");
    }
    if (!sized(event)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_input_event", "RCS_INPUT_EVENT_INIT"));
    }
    const auto scope = logAs(session);
    return guarded([&] { return session->session.sendInput(*event); });
}

rcs_result rcs_session_request_keyframe(rcs_session session) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    const auto scope = logAs(session);
    return guarded([&] { return session->session.requestKeyframe(); });
}

rcs_result rcs_session_pause(rcs_session session, uint32_t what) {
    return pauseSession(session, what, true);
}

rcs_result rcs_session_resume(rcs_session session, uint32_t what) {
    return pauseSession(session, what, false);
}

rcs_result rcs_session_push_video_frame(rcs_session session, const rcs_frame* frame) {
    beginCall();
    if (!session || !frame) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and frame are required");
    }
    if (!sized(frame)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_frame", "RCS_FRAME_INIT"));
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.pushVideoFrame(*frame);
        return RCS_OK;
    });
}

rcs_result rcs_session_push_gpu_frame(rcs_session session, const rcs_gpu_frame_info* frame) {
    beginCall();
    if (!session || !frame) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and frame are required");
    }
    if (!sized(frame)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_gpu_frame_info", "RCS_GPU_FRAME_INFO_INIT"));
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.pushGpuFrame(*frame);
        return RCS_OK;
    });
}

rcs_result rcs_session_push_encoded_video(rcs_session session, const rcs_encoded_packet* packet) {
    beginCall();
    if (!session || !packet) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and packet are required");
    }
    if (!sized(packet)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_encoded_packet: struct_size must be sizeof(rcs_encoded_packet) "
                                                  "of this library's headers");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.pushEncodedVideo(*packet);
        return RCS_OK;
    });
}

rcs_result rcs_session_video_ready(rcs_session session) {
    beginCall();
    if (!session) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    return guarded([&] {
        const rcs_result ready = session->session.videoReady();
        return ready == RCS_ERROR_QUEUE_FULL
                   ? refuse(ready, "The path or the bitrate budget is behind: hold the next packet back")
                   : ready;
    });
}

rcs_result rcs_session_push_sample_buffer(rcs_session session, const void* sample_buffer,
                                          const rcs_sample_buffer_info* info) {
    beginCall();
    if (!session || !sample_buffer || !info) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session, sample_buffer and info are required");
    }
    if (!sized(info)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_sample_buffer_info", "RCS_SAMPLE_BUFFER_INFO_INIT"));
    }
    if (info->type != RCS_SAMPLE_BUFFER_VIDEO && info->type != RCS_SAMPLE_BUFFER_AUDIO &&
        info->type != RCS_SAMPLE_BUFFER_MICROPHONE) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      "type must be RCS_SAMPLE_BUFFER_VIDEO, RCS_SAMPLE_BUFFER_AUDIO or RCS_SAMPLE_BUFFER_MICROPHONE");
    }
    if (info->type != RCS_SAMPLE_BUFFER_VIDEO && info->orientation) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      "orientation is for pictures: 0 with RCS_SAMPLE_BUFFER_AUDIO and RCS_SAMPLE_BUFFER_MICROPHONE");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.pushSampleBuffer(sample_buffer, *info);
        return RCS_OK;
    });
}

rcs_result rcs_android_initialize(void* java_vm, void* context) {
    beginCall();
    if (!java_vm || !context) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "java_vm and context are required");
    }
    return guarded([&] {
        rcsdk::platform::setJava(java_vm, context);
        return RCS_OK;
    });
}

rcs_result rcs_android_get_java(void** java_vm, void** context) {
    beginCall();
    if (!java_vm || !context) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "java_vm and context are required");
    }
    return guarded([&] {
        std::tie(*java_vm, *context) = rcsdk::platform::java();
        return RCS_OK;
    });
}

rcs_result rcs_session_push_hardware_buffer(rcs_session session, const void* hardware_buffer,
                                           const rcs_hardware_buffer_info* info) {
    beginCall();
    if (!session || !hardware_buffer || !info) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session, hardware_buffer and info are required");
    }
    if (!sized(info)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT,
                      initialise("rcs_hardware_buffer_info", "RCS_HARDWARE_BUFFER_INFO_INIT"));
    }
    if (info->rotation % 90 || info->rotation > 270) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rotation must be 0, 90, 180 or 270");
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.pushHardwareBuffer(hardware_buffer, info->rotation);
        return RCS_OK;
    });
}

rcs_result rcs_session_push_audio(rcs_session session, const rcs_audio_frame* frame) {
    beginCall();
    if (!session || !frame) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and frame are required");
    }
    if (!sized(frame)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_audio_frame", "RCS_AUDIO_FRAME_INIT"));
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.pushAudio(*frame);
        return RCS_OK;
    });
}

rcs_result rcs_session_send_data(rcs_session session, const rcs_data_message* message) {
    beginCall();
    if (!session || !message) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and message are required");
    }
    if (!sized(message)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_data_message", "RCS_DATA_MESSAGE_INIT"));
    }
    const auto scope = logAs(session);
    return guarded([&] {
        session->session.sendData(*message);
        return RCS_OK;
    });
}

rcs_result rcs_helper_wait_for_stop(uint32_t timeout_ms) {
    beginCall();
    const rcs_result result = rcsdk::platform::waitForSupervisorStop(rcsdk::platform::waitLimit(timeout_ms));
    return result == RCS_ERROR_UNSUPPORTED ? refuse(result, "This process was not started by an rcs_supervisor")
                                           : result;
}

rcs_result rcs_helper_get_key(uint8_t* key, uint32_t size) {
    beginCall();
    if (!key || size != 32) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_helper_get_key takes a buffer of 32 bytes");
    }
    return guarded([&] {
        const auto value = rcsdk::platform::helperKey();
        std::copy(value.begin(), value.end(), key);
        return RCS_OK;
    });
}

// ---- The agent ----------------------------------------------------------------------------

rcs_result rcs_agent_create(const rcs_agent_config* config, const rcs_agent_callbacks* callbacks,
                            rcs_agent* out_agent) {
    beginCall();
    if (!config || !out_agent) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "config and out_agent are required");
    }
    *out_agent = nullptr;
    if (config->struct_size != sizeof(rcs_agent_config) || !config->version) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_agent_config", "RCS_AGENT_CONFIG_INIT"));
    }
    if (config->version != RCSDK_VERSION) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_agent_config: built with other headers than this library (" +
                                                      std::string(RCSDK_VERSION_STRING) +
                                                      "); use the headers of the library you ship");
    }
    rcs_agent_callbacks table = RCS_AGENT_CALLBACKS_INIT;
    if (callbacks) {
        if (!sized(callbacks)) {
            return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_agent_callbacks", "RCS_AGENT_CALLBACKS_INIT"));
        }
        table = *callbacks;
    }
    if (!std::memchr(config->endpoint, 0, sizeof(config->endpoint))) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_agent_config: endpoint is not NUL-terminated");
    }
    if (const auto problem = rcsdk::transport::endpointProblem(config->endpoint); !problem.empty()) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_agent_config: endpoint: " + problem);
    }
    if (std::all_of(std::begin(config->key), std::end(config->key), [](uint8_t b) { return b == 0; })) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_agent_config: key is empty; it is the session's "
                                                  "rcs_agent_link_config key");
    }
    if (config->access > RCS_AGENT_ACCESS_ANY_USER) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_agent_config: unknown access");
    }
    if (config->reconnect > 1 || config->reconnect_interval_ms < 100 || config->reconnect_interval_ms > 60'000) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_agent_config: reconnect is 0 or 1, reconnect_interval_ms "
                                                  "100..60 000");
    }
    return guarded([&] {
        const auto extensions = rcsdk::session::parseAgentExtensions(config->extensions);
        *out_agent = new rcs_agent_impl(*config, table, extensions);
        return RCS_OK;
    });
}

rcs_result rcs_agent_start(rcs_agent agent) {
    beginCall();
    if (!agent) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "agent is NULL");
    }
    if (fromAgentCallback(agent)) {
        return agentCallbackRefused();
    }
    const auto scope = logAs(agent);
    return guarded([&] {
        return agent->server.start() ? RCS_OK : refuse(RCS_ERROR_INVALID_STATE, "The agent runs already; stop it first");
    });
}

rcs_result rcs_agent_stop(rcs_agent agent) {
    beginCall();
    if (!agent) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "agent is NULL");
    }
    if (fromAgentCallback(agent)) {
        return agentCallbackRefused();
    }
    const auto scope = logAs(agent);
    return guarded([&] {
        agent->server.stop();
        return RCS_OK;
    });
}

rcs_result rcs_agent_destroy(rcs_agent agent) {
    beginCall();
    if (!agent) {
        return RCS_OK;
    }
    if (fromAgentCallback(agent)) {
        return agentCallbackRefused();
    }
    const auto source = agent->log;
    const rcs_result result = guarded([&] {
        const auto scope = logAs(agent);
        delete agent;
        return RCS_OK;
    });
    rcsdk::log::end(source);
    return result;
}

rcs_result rcs_agent_get_state(rcs_agent agent, rcs_state* out_state) {
    beginCall();
    if (!agent || !out_state) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "agent and out_state are required");
    }
    *out_state = agent->server.state();
    return RCS_OK;
}

rcs_result rcs_agent_get_failure(rcs_agent agent, rcs_result* out_result, char* message, uint32_t capacity) {
    beginCall();
    if (!agent || !out_result || (!message && capacity)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "agent and out_result are required; message with a capacity");
    }
    return guarded([&] {
        const auto [result, text] = agent->server.failure();
        *out_result = result;
        if (capacity) {
            copyText(message, capacity, text);
        }
        return RCS_OK;
    });
}

rcs_result rcs_agent_wait(rcs_agent agent, uint32_t timeout_ms) {
    beginCall();
    if (!agent) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "agent is NULL");
    }
    if (fromAgentCallback(agent)) {
        return agentCallbackRefused();
    }
    const auto scope = logAs(agent);
    return guarded([&]() -> rcs_result {
        if (agent->server.state() == RCS_STATE_IDLE) {
            return refuse(RCS_ERROR_INVALID_STATE, "Start the agent first");
        }
        return agent->server.wait(rcsdk::platform::waitLimit(timeout_ms))
                   ? RCS_OK
                   : refuse(RCS_ERROR_TIMEOUT, "The agent did not end within timeout_ms");
    });
}

rcs_result rcs_encoder_output_push(rcs_encoder_output output, const rcs_encoded_packet* packet) {
    beginCall();
    if (!output || !packet) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "output and packet are required");
    }
    if (!sized(packet)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_encoded_packet: struct_size must be sizeof(rcs_encoded_packet) "
                                                  "of this library's headers");
    }
    return guarded([&] {
        output->encoder->push(*packet);
        return RCS_OK;
    });
}

rcs_result rcs_gpu_frame_retain(rcs_gpu_frame frame) {
    beginCall();
    if (!frame) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "frame is NULL");
    }
    frame->references.fetch_add(1, std::memory_order_relaxed);
    return RCS_OK;
}

void rcs_gpu_frame_release(rcs_gpu_frame frame) {
    if (frame && frame->references.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        delete frame;
    }
}

rcs_result rcs_gpu_frame_get_info(rcs_gpu_frame frame, rcs_gpu_frame_info* info) {
    beginCall();
    if (!frame) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "frame is NULL");
    }
    if (!sized(info)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_gpu_frame_info", "RCS_GPU_FRAME_INFO_INIT"));
    }
    *info = frame->info;
    return RCS_OK;
}

rcs_result rcs_gpu_frame_wait(rcs_gpu_frame frame, uint32_t timeout_ms) {
    beginCall();
    if (!frame) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "frame is NULL");
    }
    return guarded([&] { return frame->frame.waitGpu ? frame->frame.waitGpu(timeout_ms) : RCS_OK; });
}

// ---- Modules ------------------------------------------------------------------------------

rcs_result rcs_register_module(const rcs_module_definition* definition) {
    beginCall();
    if (!definition) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "definition is NULL");
    }
    return guarded([&] {
        rcsdk::modules::registerModule(*definition);
        return RCS_OK;
    });
}

rcs_result rcs_list_modules(rcs_module_info* modules, uint32_t item_size, uint32_t capacity, uint32_t* out_count) {
    beginCall();
    if (!validList(modules, item_size, capacity, out_count)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "Pass item_size = sizeof(rcs_module_info) and out_count");
    }
    return guarded([&] {
        std::vector<rcs_module_info> list;
        for (const auto* definition : rcsdk::modules::registered()) {
            rcs_module_info info{};
            copyText(info.id, definition->id);
            info.version = definition->table.version;
            info.flags = definition->table.flags;
            info.roles = definition->table.roles;
            info.max_message_bytes = definition->table.max_message_bytes;
            list.push_back(info);
        }
        return writeList(list, modules, capacity, out_count);
    });
}

rcs_result rcs_module_send(rcs_module module, rcs_viewer_id peer, const uint8_t* data, uint32_t size,
                           rcs_module_message_flags flags, uint64_t tag) {
    beginCall();
    if (!module || (!data && size)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "module is required, and data for size bytes");
    }
    if (!module->set) {
        return refuse(RCS_ERROR_INVALID_STATE, "A machine's half sends to its session's half only "
                                               "(rcs_module_send_machine)");
    }
    return guarded([&] {
        module->set->send(*module, peer, {data, size}, flags, tag);
        return RCS_OK;
    });
}

rcs_result rcs_module_set_hello(rcs_module module, const uint8_t* hello, uint32_t size) {
    beginCall();
    if (!module || (!hello && size)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "module is required, and hello for size bytes");
    }
    if (!module->set) {
        return refuse(RCS_ERROR_INVALID_STATE, "A machine's half has no hello");
    }
    return guarded([&] {
        module->set->setHello(*module, {hello, size});
        return RCS_OK;
    });
}

rcs_result rcs_module_send_machine(rcs_module module, const uint8_t* data, uint32_t size) {
    beginCall();
    if (!module || !data || !size) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "module and 1.. bytes of data are required");
    }
    return guarded([&]() -> rcs_result {
        if (module->host) {
            if (size > module->definition.table.max_message_bytes) {
                return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_module_send_machine: more than max_message_bytes");
            }
            module->host->toSession(*module, {data, size});
        } else {
            module->set->sendMachine(*module, {data, size});
        }
        return RCS_OK;
    });
}

rcs_result rcs_module_get_shared(rcs_module module, rcs_module_shared* out) {
    beginCall();
    if (!module) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "module is NULL");
    }
    if (!sized(out)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_module_shared", "RCS_MODULE_SHARED_INIT"));
    }
    if (!module->host) {
        return refuse(RCS_ERROR_INVALID_STATE, "A session's half shares nothing: its machine's half asks where it is");
    }
    return guarded([&]() -> rcs_result {
        const auto target = module->host->shared();
        if (!target) {
            return refuse(RCS_ERROR_INVALID_STATE, "Nothing is shared on this machine (yet)");
        }
        const auto area = rcsdk::platform::sharedArea(*target);
        if (!area) {
            return refuse(RCS_ERROR_INVALID_STATE, "What is shared does not show now");
        }
        out->source = target->source;
        out->display_id = target->displayId;
        out->window_id = target->windowId;
        out->process_id = area->processId;
        out->left = area->left;
        out->top = area->top;
        out->right = area->right;
        out->bottom = area->bottom;
        return RCS_OK;
    });
}

rcs_result rcs_module_emit(rcs_module module, rcs_viewer_id peer, uint32_t kind, const void* payload, uint32_t size) {
    beginCall();
    if (!module || (!payload && size)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "module is required, and payload for size bytes");
    }
    if (!module->set) {
        return refuse(RCS_ERROR_INVALID_STATE, "A machine's half has no host to tell: its session's half tells it");
    }
    return guarded([&] {
        module->set->emit(*module, peer, kind, {static_cast<const uint8_t*>(payload), size});
        return RCS_OK;
    });
}

rcs_session rcs_module_session(rcs_module module) {
    return module && module->set ? module->set->handle() : nullptr;
}

rcs_result rcs_module_find(rcs_session session, const char* id, rcs_module* out) {
    beginCall();
    if (!session || !id || !out) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session, id and out are required");
    }
    *out = session->session.modules().find(id);
    return *out ? RCS_OK
                : refuse(RCS_ERROR_INVALID_STATE,
                         std::string("The session does not use module \"") + id + "\" (rcs_module_config)");
}

rcs_result rcs_module_run(rcs_module module, rcs_result (*task)(rcs_module module, void* argument), void* argument) {
    beginCall();
    if (!module || !task) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "module and task are required");
    }
    rcs_result result = RCS_ERROR_INTERNAL; // What a task that throws leaves.
    rcsdk::modules::call(*module, [&] { result = task(module, argument); });
    return result;
}

void rcs_module_set_state(rcs_module module, void* state, void (*release)(void* state)) {
    if (!module) {
        return;
    }
    void* old = nullptr;
    void (*oldRelease)(void*) = nullptr;
    {
        std::lock_guard lock(module->stateMutex);
        old = std::exchange(module->state, state);
        oldRelease = std::exchange(module->release, release);
    }
    if (oldRelease && old != state) {
        oldRelease(old);
    }
}

void* rcs_module_state(rcs_module module) {
    if (!module) {
        return nullptr;
    }
    std::lock_guard lock(module->stateMutex);
    return module->state;
}

void rcs_module_log(rcs_module module, rcs_log_level level, const char* message) {
    if (!module || !message || level < RCS_LOG_ERROR || level > RCS_LOG_DEBUG) {
        return;
    }
    const rcsdk::log::Scope scope(module->logSource);
    rcsdk::log::write(level, module->definition.id.c_str(), message);
}

rcs_result rcs_capture_picture(const rcs_capture_config* what, uint32_t max_width, uint32_t max_height,
                               rcs_picture* out) {
    beginCall();
    if (!sized(out)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_picture", "RCS_PICTURE_INIT"));
    }
    if (!out->data && out->capacity) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcs_picture: data is NULL, capacity is not 0");
    }
    return guarded([&] {
        using namespace rcsdk::session;
        return writePicture(
            takeThumbnail(what, max_width, max_height, captureMachine, rcsdk::seams::captureThumbnail), *out);
    });
}

rcs_result rcs_session_get_module_stats(rcs_session session, rcs_viewer_id viewer, const char* module,
                                        rcs_module_stats* stats) {
    beginCall();
    if (!session || !module) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "session and module are required");
    }
    if (!sized(stats)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, initialise("rcs_module_stats", "RCS_MODULE_STATS_INIT"));
    }
    if (viewer && session->session.config().role != RCS_ROLE_SHARE) {
        return refuse(RCS_ERROR_UNSUPPORTED, "A viewing session has no viewers");
    }
    const int index = session->session.modules().indexOf(module);
    if (index < 0) {
        return refuse(RCS_ERROR_INVALID_STATE,
                      std::string("The session does not use module \"") + module + "\" (rcs_module_config)");
    }
    return guarded([&] {
        *stats = session->session.moduleStats(viewer, index);
        return RCS_OK;
    });
}

} // extern "C"
