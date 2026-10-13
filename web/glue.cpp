// The browser package's C side: what web/rcsdk.mjs calls through the module
// (rcsw_*), and the session callbacks that hand the page its events and
// pictures. Three threads of the page's own carry it:
//   the page's main thread  events, the calls that return at once (input,
//                           data, the modules' functions);
//   the media thread        (src/platform/web/web.hpp) the decoders, the
//                           views' canvases and a session's WebSocket: the
//                           pictures never wait on the page;
//   the files thread        the file transfers' reads and writes (File,
//                           FileSystemDirectoryHandle, memory).
// The calls that wait (start, stop, destroy, update) run on an API thread, so
// that the page never waits on the SDK (docs/guide/23-browser.md).
#include "json.hpp"

#include <rcsdk/clipboard.h>
#include <rcsdk/file_transfer.h>
#include <rcsdk/gpu.h>
#include <rcsdk/host_transport.h>
#include <rcsdk/rcsdk.h>

#include <emscripten.h>
#include <emscripten/proxying.h>
#include <emscripten/threading.h>

#include <atomic>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <pthread.h>
#include <shared_mutex>
#include <string>
#include <vector>

extern "C" pthread_t rcsdk_web_media_thread(void); // src/platform/web/platform.cpp

namespace {

using web::Json;
using web::JsonWriter;

// The page's side (web/pre.js, Module.rcsdk of the page).
EM_JS(void, jsEvent, (int id, const char* json), { Module.rcsdk.event(id, UTF8ToString(json)); });
EM_JS(int, jsSendPage, (int id, const uint8_t* data, int size),
      { return Module.rcsdk.send(id, HEAPU8.slice(data, data + size)); });
EM_JS(void, jsDone, (int call, int result, const char* message), { Module.rcsdk.done(call, result, UTF8ToString(message)); });
EM_JS(void, jsCursorShape,
      (int id, int shape, int type, int width, int height, int hotspotX, int hotspotY, const uint8_t* pixels, int stride),
      { Module.rcsdk.cursorShape(id, shape, type, width, height, hotspotX, hotspotY, pixels, stride); });
EM_JS(void, jsData, (int id, int channel, int flags, const uint8_t* data, int size),
      { Module.rcsdk.data(id, channel, flags, HEAPU8.slice(data, data + size)); });
EM_JS(void, jsClipboard, (int id, const char* json, const uint8_t* data, int size),
      { Module.rcsdk.clipboard(id, UTF8ToString(json), HEAPU8.slice(data, data + size)); });
// Answers through rcsw_token_done(call, token or 0).
EM_JS(void, jsRefreshToken, (void* call, int id), { Module.rcsdk.refreshToken(call, id); });

// The media thread's side (Module.rcsdk of its worker).
EM_JS(void, jsFrame, (int id, int stream, int frame, int width, int height, double timestampUs),
      { Module.rcsdk.frame(id, stream, frame, width, height, timestampUs); });
EM_JS(int, jsSendSocket, (int id, const uint8_t* data, int size),
      { return Module.rcsdk.socketSend(id, HEAPU8.slice(data, data + size)); });

// The files thread's side: each call answers through rcsw_files_done(call, ...).
EM_JS(void, jsFileRead, (void* call, int id, double transfer, int item, double offset, uint8_t* data, int size),
      { Module.rcsdk.fileRead(call, id, transfer, item, offset, data, size); });
EM_JS(void, jsFileWrite,
      (void* call, int id, double transfer, int item, const char* name, double itemSize, double modifiedMs,
       double offset, const uint8_t* data, int size),
      {
          Module.rcsdk.fileWrite(call, id, transfer, item, UTF8ToString(name), itemSize, modifiedMs, offset,
                                 HEAPU8.slice(data, data + size));
      });
EM_JS(void, jsFileResume, (void* call, int id, double transfer, int item, const char* name, double itemSize, const char* token),
      { Module.rcsdk.fileResume(call, id, transfer, item, UTF8ToString(name), itemSize, UTF8ToString(token)); });
EM_JS(void, jsFileEvent, (int id, const char* json), { Module.rcsdk.fileEvent(id, UTF8ToString(json)); });

// Runs `work` on `thread` later (the runtime's queue, in order).
void on(pthread_t thread, std::function<void()> work) {
    auto* job = new std::function<void()>(std::move(work));
    if (!emscripten_proxy_async(
            emscripten_proxy_get_system_queue(), thread,
            [](void* argument) {
                std::unique_ptr<std::function<void()>> owned(static_cast<std::function<void()>*>(argument));
                (*owned)();
            },
            job)) {
        delete job;
    }
}
void onPage(std::function<void()> work) {
    on(emscripten_main_runtime_thread_id(), std::move(work));
}
void onMedia(std::function<void()> work) {
    on(rcsdk_web_media_thread(), std::move(work));
}

// A thread that returns to its event loop and runs what is handed to it.
pthread_t liveThread() {
    pthread_t t{};
    pthread_create(&t, nullptr, [](void*) -> void* {
        emscripten_exit_with_live_runtime();
        return nullptr;
    }, nullptr);
    return t;
}
pthread_t apiThread() {
    static const pthread_t thread = liveThread();
    return thread;
}
pthread_t filesThread() {
    static const pthread_t thread = liveThread();
    return thread;
}

// A session's channel as its sends see it: shared with the sends queued on
// the media or page thread, which may run after the session is gone (its
// goodbyes leave as it is destroyed).
struct WebChannel {
    int id = 0;
    // Bytes handed to the channel and not sent yet, and what it still
    // buffers (its side says): beyond 8 MiB the SDK waits.
    std::atomic<int64_t> pending{0}, buffered{0};
    // The media thread's WebSocket (connectWebSocket), not the page's own.
    std::atomic<bool> socket{false};
};

struct WebSession {
    int id = 0;
    rcs_session session = nullptr;
    std::shared_ptr<WebChannel> channel = std::make_shared<WebChannel>();
    // Through a forwarder (RCS_FORWARDER_HOST over the channel).
    bool forwarder = false;
    // Held (shared) by each call into the session, so that its destroy waits
    // for the calls under way: the threads call in on their own (the media
    // thread's WebSocket, the page's input).
    std::shared_mutex use;
};

std::mutex sessionsMutex;
std::map<int, std::shared_ptr<WebSession>> sessions;
int nextSession = 1;

// A session for the length of one call; empty once its destroy began (the
// session is out of the map then, so a call never waits on a destroy).
struct Lease {
    std::shared_ptr<WebSession> session;
    std::shared_lock<std::shared_mutex> lock;
    explicit operator bool() const { return session != nullptr; }
    WebSession* operator->() const { return session.get(); }
};

Lease find(int id) {
    std::lock_guard lock(sessionsMutex);
    const auto found = sessions.find(id);
    if (found == sessions.end()) {
        return {};
    }
    return {found->second, std::shared_lock(found->second->use)};
}

// Why the last call on this thread failed.
std::string lastError() {
    char text[512] = {};
    rcs_get_last_call_error(text, sizeof(text));
    return text;
}

std::string failureOf(rcs_session session) {
    rcs_result result = RCS_OK;
    char message[512] = {};
    rcs_session_get_failure(session, &result, message, sizeof(message));
    return message;
}

std::string streamInfoJson(const rcs_stream_info& i) {
    JsonWriter color, content, display, desktop;
    color.add("primaries", i.color.primaries).add("transfer", i.color.transfer).add("matrix", i.color.matrix)
        .add("range", i.color.range);
    content.add("x", i.content_x).add("y", i.content_y).add("width", i.content_width).add("height", i.content_height);
    display.add("width", i.display_width).add("height", i.display_height).add("refreshHz", i.display_refresh_hz);
    desktop.add("x", i.desktop_x).add("y", i.desktop_y).add("width", i.desktop_width).add("height", i.desktop_height);
    JsonWriter j;
    j.add("stream", i.stream).add("encoding", i.encoding).add("encodings", i.encodings).add("layers", i.layers)
        .add("receivedLayers", i.received_layers).add("codec", i.codec)
        .add("format", i.format).raw("color", color.str())
        .add("encoderAcceleration", i.encoder_acceleration).add("width", i.width).add("height", i.height)
        .add("framesPerSecond", i.frames_per_second).add("bitrateBps", i.bitrate_bps).raw("content", content.str())
        .raw("display", display.str()).raw("desktop", desktop.str()).add("cursor", i.cursor)
        .add("audioCodec", i.audio_codec).add("audioBitrateBps", i.audio_bitrate_bps);
    return j.str();
}

std::string fileTransferJson(const rcs_file_transfer_info& t) {
    JsonWriter j;
    j.add("viewerId", t.viewer_id).add("direction", t.direction).add("target", t.target).add("state", t.state)
        .add("paused", t.paused).add("resuming", t.resuming).hex("token", t.token, RCS_FILE_TOKEN_SIZE)
        .add("items", t.items).add("itemsCompleted", t.items_completed).add("itemsSkipped", t.items_skipped)
        .add("itemsFailed", t.items_failed).add("item", t.item).add("totalBytes", t.total_bytes)
        .add("doneBytes", t.done_bytes).add("resumedBytes", t.resumed_bytes)
        .add("transferredBytes", t.transferred_bytes).add("bytesPerSecond", t.bytes_per_second).add("end", t.end)
        .add("byPeer", t.by_peer).add("result", t.result).add("partialKept", t.partial_kept)
        .add("message", t.message);
    return j.str();
}

std::string fileItemJson(const rcs_file_item_info& i) {
    JsonWriter j;
    j.add("index", i.index).add("kind", i.kind).add("state", i.state).add("result", i.result).add("size", i.size)
        .add("modifiedMs", int64_t(i.modified_ns / 1'000'000)).add("bytes", i.bytes).add("path", i.path)
        .add("message", i.message);
    return j.str();
}

// A module's event: the SDK's kinds, and the clipboard's and file transfer's own.
void moduleFields(WebSession* s, const rcs_module_event& m, JsonWriter& j) {
    j.add("module", m.module).add("kind", m.kind).add("result", m.result).add("message", m.message);
    const std::string id = m.module ? m.module : "";
    if (id == RCS_CLIPBOARD_MODULE && m.kind == RCS_CLIPBOARD_COPY && m.size >= sizeof(rcs_clipboard_event)) {
        const auto& c = *static_cast<const rcs_clipboard_event*>(m.payload);
        j.add("flow", c.flow).add("copyResult", c.result).add("formats", c.formats).add("leftOut", c.left_out)
            .add("bytes", c.bytes).add("copyMessage", c.message);
    } else if (id == RCS_CLIPBOARD_MODULE && m.kind == RCS_CLIPBOARD_PEER && m.size >= sizeof(rcs_clipboard_peer)) {
        const auto& p = *static_cast<const rcs_clipboard_peer*>(m.payload);
        j.add("formats", p.formats).add("maxBytes", p.max_bytes);
    } else if (id == RCS_FILE_TRANSFER_MODULE && m.payload) {
        switch (m.kind) {
        case RCS_FILE_OFFER: {
            const auto& o = *static_cast<const rcs_file_offer_event*>(m.payload);
            j.add("transfer", o.transfer).add("items", o.items).add("resuming", o.resuming)
                .add("totalBytes", o.total_bytes).hex("token", o.token, RCS_FILE_TOKEN_SIZE);
            break;
        }
        case RCS_FILE_PROGRESS: {
            const auto& p = *static_cast<const rcs_file_progress_event*>(m.payload);
            j.add("transfer", p.transfer).add("item", p.item).add("itemsFinished", p.items_finished)
                .add("doneBytes", p.done_bytes).add("totalBytes", p.total_bytes)
                .add("bytesPerSecond", p.bytes_per_second);
            break;
        }
        case RCS_FILE_ITEM_DONE: {
            const auto& e = *static_cast<const rcs_file_item_event*>(m.payload);
            j.add("transfer", e.transfer).add("item", e.item).add("state", e.state).add("itemResult", e.result)
                .add("bytes", e.bytes).add("itemMessage", e.message);
            // The item as the files thread needs it (an empty file or a
            // directory gets no write), and the page.
            rcs_file_item_info info{};
            uint32_t count = 0;
            const rcs_result listed = rcs_file_get_items(s->session, e.transfer, e.item, &info, sizeof(info), 1, &count);
            if ((listed == RCS_OK || listed == RCS_ERROR_BUFFER_TOO_SMALL) && count >= 1) { // Filled either way.
                j.add("itemKind", info.kind).add("path", info.path).add("size", info.size)
                    .add("modifiedMs", int64_t(info.modified_ns / 1'000'000));
            }
            break;
        }
        case RCS_FILE_DONE: {
            const auto& d = *static_cast<const rcs_file_done_event*>(m.payload);
            j.add("transfer", d.transfer).add("direction", d.direction).add("end", d.end).add("byPeer", d.by_peer)
                .add("doneResult", d.result).add("itemsCompleted", d.items_completed)
                .add("itemsSkipped", d.items_skipped).add("itemsFailed", d.items_failed)
                .add("partialKept", d.partial_kept).add("doneBytes", d.done_bytes).add("doneMessage", d.message);
            break;
        }
        case RCS_FILE_STATE: {
            const auto& e = *static_cast<const rcs_file_state_event*>(m.payload);
            j.add("transfer", e.transfer).add("state", e.state).add("paused", e.paused);
            break;
        }
        case RCS_FILE_PEER: {
            const auto& p = *static_cast<const rcs_file_peer*>(m.payload);
            j.add("flow", p.flow).add("maxItems", p.max_items).add("maxOfferBytes", p.max_offer_bytes);
            break;
        }
        default:
            break;
        }
    }
}

// Events as JSON for the page: every field of the kind, as rcs_event has it.
void onEvent(const rcs_event* event, void* user) {
    auto* s = static_cast<WebSession*>(user);
    JsonWriter j;
    j.add("type", event->type).add("viewerId", event->viewer_id).add("stream", event->stream_index)
        .add("timestampNs", event->timestamp_ns);
    switch (event->type) {
    case RCS_EVENT_STATE:
        j.add("value", event->state.value).add("result", event->state.result);
        if (event->state.value == RCS_STATE_FAILED) {
            j.add("message", failureOf(s->session));
        }
        break;
    case RCS_EVENT_PEER: {
        const auto& p = event->peer;
        std::string limits = "[";
        for (const auto& limit : p.codec_limits) {
            if (limit.codec) {
                JsonWriter l;
                l.add("codec", limit.codec).add("maxWidth", limit.max_width).add("maxHeight", limit.max_height)
                    .add("pictureFormats", limit.picture_formats);
                limits += (limits.size() > 1 ? "," : "") + l.str();
            }
        }
        j.add("platform", p.platform).add("codecs", p.codecs).raw("codecLimits", limits + "]")
            .add("sendAudioCodecs", p.send_audio_codecs).add("receiveAudioCodecs", p.receive_audio_codecs)
            .add("sendsAudio", p.sends_audio).add("receivesAudio", p.receives_audio).add("inputTypes", p.input_types);
        if (p.has_public_key) {
            j.hex("publicKey", p.public_key, sizeof(p.public_key));
        }
        break;
    }
    case RCS_EVENT_PIN:
        j.add("failures", event->pin.failures).add("attemptsLeft", event->pin.attempts_left);
        break;
    case RCS_EVENT_ADMISSION:
        j.add("state", event->admission.state).add("timeoutMs", event->admission.timeout_ms)
            .add("result", event->admission.result).add("message", event->admission.message);
        break;
    case RCS_EVENT_STREAM:
        j.add("changes", event->stream.changes).raw("info", streamInfoJson(event->stream.info));
        break;
    case RCS_EVENT_CODEC: {
        const auto& c = event->codec;
        j.add("direction", c.direction).add("codec", c.codec).add("acceleration", c.acceleration)
            .add("backend", c.backend).add("lowLatency", c.low_latency).add("refInvalidation", c.ref_invalidation)
            .add("name", c.name);
        break;
    }
    case RCS_EVENT_CAPTURE:
        j.add("paused", event->capture.paused);
        break;
    case RCS_EVENT_DISPLAYS:
        j.add("count", event->displays.count);
        break;
    case RCS_EVENT_RECOVERY:
        j.add("reason", event->recovery.reason).add("method", event->recovery.method)
            .add("frozenNs", event->recovery.frozen_ns);
        break;
    case RCS_EVENT_KEYFRAME:
        j.add("reason", event->keyframe.reason);
        break;
    case RCS_EVENT_RATE: {
        const auto& r = event->rate;
        j.add("bitrateBps", r.bitrate_bps).add("lossPermille", r.loss_permille).add("receivedBps", r.received_bps)
            .add("queueDelayMs", r.queue_delay_ms).add("roundTripMs", r.round_trip_ms);
        break;
    }
    case RCS_EVENT_CURSOR: {
        const auto& c = event->cursor;
        j.add("visible", c.visible).add("cursorStream", c.stream).add("x", c.x).add("y", c.y).add("shapeId", c.shape_id);
        if (const rcs_cursor_shape* shape = c.shape) { // Its image first, copied for the page.
            std::vector<uint8_t> pixels(shape->pixels, shape->pixels + size_t(shape->stride_bytes) * shape->height);
            onPage([id = s->id, shape = *shape, pixels = std::move(pixels)] {
                jsCursorShape(id, int(shape.id), int(shape.type), int(shape.width), int(shape.height),
                              int(shape.hotspot_x), int(shape.hotspot_y), pixels.data(), int(shape.stride_bytes));
            });
        }
        break;
    }
    case RCS_EVENT_AUDIO: {
        const auto& a = event->audio;
        j.add("direction", a.direction).add("active", a.active).add("codec", a.codec).add("result", a.result)
            .add("message", a.message).add("voiceProcessing", a.voice_processing);
        break;
    }
    case RCS_EVENT_CONTROL:
        j.add("allowed", event->control.allowed).add("granted", event->control.granted);
        break;
    case RCS_EVENT_DATA:
        j.add("queuedBytes", event->data.queued_bytes);
        break;
    case RCS_EVENT_DESKTOP:
        j.add("kind", event->desktop.kind).add("userLoggedOn", event->desktop.user_logged_on);
        break;
    case RCS_EVENT_MODULE:
        moduleFields(s, event->module, j);
        break;
    case RCS_EVENT_PAUSE: {
        const auto& p = event->pause;
        j.add("paused", p.paused).add("byHost", p.by_host).add("byBackground", p.by_background).add("byPeer", p.by_peer);
        break;
    }
    case RCS_EVENT_PATH: { // Over the host's channel: its replacements (rcs_session_transport_restarted).
        const auto& p = event->path;
        const auto end = [](const rcs_path_info& info) {
            JsonWriter e;
            e.add("route", info.route).add("relayTransport", info.relay_transport).add("network", info.network)
                .add("networkFlags", info.network_flags).add("interfaceName", info.interface_name)
                .add("localAddress", info.local_address).add("remoteAddress", info.remote_address);
            return e.str();
        };
        j.add("change", p.change).add("cause", p.cause).raw("from", end(p.from)).raw("to", end(p.to))
            .add("interruptionNs", p.interruption_ns).add("signaled", p.signaled).add("result", p.result)
            .add("message", p.message);
        break;
    }
    case RCS_EVENT_STALLS: { // No Wi-Fi of its own in a browser: `wifi` stays empty.
        const auto& t = event->stalls;
        j.add("fromPeer", t.from_peer).add("periodMs", t.period_ms).add("stallMs", t.stall_ms).add("stalls", t.stalls);
        break;
    }
    default:
        break;
    }
    const std::string json = j.str();
    // What ends a transfer's items reaches the files thread too (it closes and finishes files there).
    if (event->type == RCS_EVENT_MODULE && event->module.module &&
        std::strcmp(event->module.module, RCS_FILE_TRANSFER_MODULE) == 0 &&
        (event->module.kind == RCS_FILE_ITEM_DONE || event->module.kind == RCS_FILE_DONE)) {
        on(filesThread(), [id = s->id, json] { jsFileEvent(id, json.c_str()); });
    }
    onPage([id = s->id, json] { jsEvent(id, json.c_str()); });
}

// The sharer's data messages (rcs_session_send_data on its side).
void onData(const rcs_data_message* message, void* user) {
    auto* s = static_cast<WebSession*>(user);
    onPage([id = s->id, channel = message->channel, flags = message->flags,
            bytes = std::vector<uint8_t>(message->data, message->data + message->size)] {
        jsData(id, int(channel), int(flags), bytes.data(), int(bytes.size()));
    });
}

// A picture, on the media thread where its VideoFrame is: drawn on the
// stream's views there, or handed to the page; the SDK's reference goes after.
void onGpuFrame(rcs_gpu_frame frame, void* user) {
    auto* s = static_cast<WebSession*>(user);
    rcs_gpu_frame_info info = RCS_GPU_FRAME_INFO_INIT;
    if (rcs_gpu_frame_get_info(frame, &info) != RCS_OK || info.resource_type != RCS_GPU_WEB_VIDEO_FRAME ||
        rcs_gpu_frame_retain(frame) != RCS_OK) {
        return;
    }
    onMedia([id = s->id, frame, info] {
        jsFrame(id, int(info.stream), int(reinterpret_cast<intptr_t>(info.resource)), int(info.width), int(info.height),
                double(info.timestamp_ns / 1000));
        rcs_gpu_frame_release(frame);
    });
}

// The SDK's bytes for the channel (the media thread's WebSocket, or the
// page's own); 0 (wait) while 8 MiB are on their way.
int32_t onSend(uint64_t, const uint8_t* data, uint32_t size, void* user) { // A viewer's route is 0.
    const auto c = static_cast<WebSession*>(user)->channel;
    if (c->pending + c->buffered > (8 << 20)) {
        return 0;
    }
    c->pending += size;
    auto send = [c, bytes = std::vector<uint8_t>(data, data + size)](bool socket) {
        c->buffered = socket ? jsSendSocket(c->id, bytes.data(), int(bytes.size()))
                             : jsSendPage(c->id, bytes.data(), int(bytes.size()));
        c->pending -= int64_t(bytes.size());
    };
    if (c->socket) {
        onMedia([send] { send(true); });
    } else {
        onPage([send] { send(false); });
    }
    return 1;
}

// ---- The clipboard (target HOST): the page reads and writes its own -------

// A copy as the page has it: {"viewerId","flow","concealed","items":[{"format","size"}]}
// and the items' bytes one after another.
int32_t onClipboard(const rcs_clipboard_content* content, void* user) {
    auto* s = static_cast<WebSession*>(user);
    if (content->flow != RCS_CLIPBOARD_RECEIVE) {
        return 1; // The page's own copies, going: let them.
    }
    std::string items = "[";
    std::vector<uint8_t> bytes;
    for (uint32_t i = 0; i < content->count; ++i) {
        const auto* item = reinterpret_cast<const rcs_clipboard_item*>(
            reinterpret_cast<const uint8_t*>(content->items) + size_t(i) * content->item_size);
        JsonWriter one;
        one.add("format", item->format).add("size", item->size);
        items += (i ? "," : "") + one.str();
        bytes.insert(bytes.end(), item->data, item->data + item->size);
    }
    JsonWriter j;
    j.add("viewerId", content->viewer_id).add("flow", content->flow).add("concealed", content->concealed)
        .raw("items", items + "]");
    onPage([id = s->id, json = j.str(), bytes = std::move(bytes)] {
        jsClipboard(id, json.c_str(), bytes.data(), int(bytes.size()));
    });
    return 1;
}

// ---- File transfer (target HOST): the files thread reads and writes -------

// One call of the module's that the files thread answers (rcsw_files_done);
// the module's thread waits for it.
struct FileCall {
    em_proxying_ctx* ctx = nullptr;
    std::function<void(FileCall*)> start;
    rcs_result result = RCS_ERROR_DEVICE;
    uint64_t value = 0;
    std::string message = "The page's files did not answer";
};

rcs_result fileCall(FileCall& call) {
    if (!emscripten_proxy_sync_with_ctx(
            emscripten_proxy_get_system_queue(), filesThread(),
            [](em_proxying_ctx* ctx, void* argument) {
                auto* c = static_cast<FileCall*>(argument);
                c->ctx = ctx;
                c->start(c);
            },
            &call)) {
        return rcs_set_last_call_error(RCS_ERROR_DEVICE, "The files thread is gone");
    }
    return call.result == RCS_OK ? RCS_OK : rcs_set_last_call_error(call.result, call.message.c_str());
}

int sessionOf(void* user) {
    return static_cast<WebSession*>(user)->id;
}

rcs_result onFileRead(const rcs_file_chunk* chunk, void* user) {
    FileCall call;
    const int id = sessionOf(user);
    call.start = [&](FileCall* c) {
        jsFileRead(c, id, double(chunk->transfer), int(chunk->item), double(chunk->offset), chunk->data,
                   int(chunk->size));
    };
    return fileCall(call);
}

rcs_result onFileWrite(const rcs_file_chunk* chunk, void* user) {
    FileCall call;
    const int id = sessionOf(user);
    call.start = [&](FileCall* c) {
        jsFileWrite(c, id, double(chunk->transfer), int(chunk->item), chunk->name, double(chunk->item_size),
                    double(chunk->modified_ns / 1'000'000), double(chunk->offset), chunk->data, int(chunk->size));
    };
    return fileCall(call);
}

uint64_t onFileResume(const rcs_file_chunk* chunk, void* user) {
    FileCall call;
    const int id = sessionOf(user);
    const std::string hex = web::hex(chunk->token, RCS_FILE_TOKEN_SIZE);
    call.start = [&](FileCall* c) {
        jsFileResume(c, id, double(chunk->transfer), int(chunk->item), chunk->name, double(chunk->item_size),
                     hex.c_str());
    };
    return fileCall(call) == RCS_OK ? std::min<uint64_t>(call.value, chunk->item_size) : 0;
}

uint32_t bits(const Json& names, std::initializer_list<std::pair<const char*, uint32_t>> table) {
    uint32_t value = 0;
    for (const auto& name : names.items()) {
        for (const auto& [text, bit] : table) {
            if (name.text() == text) {
                value |= 1u << bit;
            }
        }
    }
    return value;
}

bool hexBytes(const std::string& text, uint8_t* out, size_t size) {
    if (text.size() != size * 2) {
        return false;
    }
    for (size_t i = 0; i < size; ++i) {
        char digits[3] = {text[i * 2], text[i * 2 + 1], 0};
        char* end = nullptr;
        out[i] = uint8_t(std::strtoul(digits, &end, 16));
        if (*end) {
            return false;
        }
    }
    return true;
}

rcs_result invalid(const char* why) {
    return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, why);
}

// Zeroes what held a secret; volatile, so that the compiler keeps the stores
// to memory nothing reads again.
void wipe(void* data, size_t size) {
    auto* bytes = static_cast<volatile uint8_t*>(data);
    for (size_t i = 0; i < size; ++i) {
        bytes[i] = 0;
    }
}

// ---- The forwarder's token: the page asks its server for a new one -------

// One refresh_token call the page answers (rcsw_token_done); the run's thread waits for it.
struct TokenCall {
    em_proxying_ctx* ctx = nullptr;
    int id = 0;
    bool given = false;
    std::string token;
};

// Before each connection to the forwarder but the session's first: the
// page's refreshToken (rcsdk.mjs) gives the token, or none (the one in use
// stays). One that does not fit goes as none at all, which fails the run
// with the reason (INVALID_ARGUMENT), as a C host's would.
int32_t onRefreshToken(char* token, uint32_t capacity, void* user) {
    TokenCall call;
    call.id = static_cast<WebSession*>(user)->id;
    if (!emscripten_proxy_sync_with_ctx(
            emscripten_proxy_get_system_queue(), emscripten_main_runtime_thread_id(),
            [](em_proxying_ctx* ctx, void* argument) {
                auto* c = static_cast<TokenCall*>(argument);
                c->ctx = ctx;
                jsRefreshToken(c, c->id);
            },
            &call) ||
        !call.given) {
        return 0;
    }
    if (call.token.size() < capacity) {
        std::memcpy(token, call.token.c_str(), call.token.size() + 1);
    } else {
        token[0] = 0;
    }
    wipe(call.token.data(), call.token.size());
    return 1;
}

// Why `what` (JSON from rcsdk.mjs) is refused: it read as null.
std::string notJson(const char* what) {
    return std::string(what) + " must be a JSON object nested at most " + std::to_string(Json::maxDepth) +
           " levels deep";
}

// The options' viewRequest (rcsdk.mjs, viewRequestJson).
rcs_view_request_config viewRequest(const Json& r) {
    rcs_view_request_config request = RCS_VIEW_REQUEST_CONFIG_INIT;
    request.width = r["width"].u32();
    request.height = r["height"].u32();
    request.frames_per_second = r["framesPerSecond"].u32();
    request.bitrate_bps = r["bitrateBps"].u32();
    request.codec = r["codec"].u32();
    request.format = r["format"].u32();
    request.streams = r["streams"].u32();
    request.audio_codec = r["audioCodec"].u32();
    request.audio_bitrate_bps = r["audioBitrateBps"].u32();
    request.cursor = r["cursor"].u32();
    return request;
}

} // namespace

extern "C" {

// A viewer over a channel of the page's (a WebSocket on the media thread, or
// the page's own), to the sharer or, with "forwarder" ({keySha256 in hex,
// token, refresh}), to a forwarder (RCS_FORWARDER_HOST), with the options
// rcsdk.mjs's createSession takes, as JSON (web/rcsdk.d.ts, SessionOptions;
// identity: "key", "pin" or "password"), and
// the secret apart as `secretSize` bytes (the key, or the PIN or password in
// UTF-8), which the page wipes after the call: the session's number, or a
// negative rcs_result (rcs_get_last_call_error says why). The copy here is
// wiped too; the session keeps its own while it lives (it reconnects with it).
EMSCRIPTEN_KEEPALIVE int rcsw_create(const char* text, const uint8_t* secret, uint32_t secretSize) {
    static const bool registered = [] { // The official modules the package carries, once.
        rcs_register_module(rcs_clipboard_module());
        rcs_register_module(rcs_file_transfer_module());
        return true;
    }();
    (void)registered;
    apiThread(); // Started from the page's thread, before anything waits on them.
    filesThread();
    const Json options = Json::parse(text);
    if (options.kind() != Json::Object) {
        return invalid(notJson("The options").c_str());
    }
    auto s = std::make_shared<WebSession>();
    rcs_session_config config = RCS_SESSION_CONFIG_INIT;
    const std::unique_ptr<rcs_identity, void (*)(rcs_identity*)> wiped(
        &config.identity, [](rcs_identity* identity) { wipe(identity, sizeof(*identity)); });
    config.role = RCS_ROLE_VIEW;
    config.connect.transport = RCS_TRANSPORT_HOST;
    const std::string& kind = options["identity"].text();
    if (kind == "key") {
        if (secretSize != sizeof(config.identity.key)) {
            return invalid("identity.key must be 32 bytes");
        }
        config.identity.secret = RCS_SECRET_KEY;
        std::memcpy(config.identity.key, secret, secretSize);
    } else if (kind == "pin" || kind == "password") {
        if (secretSize >= sizeof(config.identity.password)) {
            return invalid("The PIN or password is too long");
        }
        if (std::memchr(secret, 0, secretSize)) {
            return invalid("The PIN or password holds a NUL character");
        }
        config.identity.secret = kind == "pin" ? RCS_SECRET_PIN : RCS_SECRET_PASSWORD;
        std::memcpy(config.identity.password, secret, secretSize);
    } else {
        return invalid("identity needs key, pin or password");
    }
    std::vector<rcs_extension*> chain;
    rcs_host_transport_config transport = RCS_HOST_TRANSPORT_CONFIG_INIT;
    transport.host_channel = options["channel"].text() == "datagram" ? RCS_HOST_CHANNEL_DATAGRAM : RCS_HOST_CHANNEL_STREAM;
    transport.user_data = s.get();
    transport.on_transport_send = onSend;
    chain.push_back(&transport.header);
    // Through a forwarder: the channel leads to it (its WebSocket entry, or the page's own way there).
    rcs_forwarder_config forwarder = RCS_FORWARDER_CONFIG_INIT;
    const std::unique_ptr<char, void (*)(char*)> wipedToken(forwarder.token,
                                                            [](char* t) { wipe(t, RCS_FORWARDER_TOKEN_SIZE); });
    if (const Json& f = options["forwarder"]; f.kind() == Json::Object) {
        if (!hexBytes(f["keySha256"].text(), forwarder.forwarder_key_sha256, sizeof(forwarder.forwarder_key_sha256))) {
            return invalid("forwarder.keySha256 must be 32 bytes: the SHA-256 of the forwarder's public key");
        }
        const std::string& token = f["token"].text();
        if (token.size() >= sizeof(forwarder.token)) {
            return invalid("forwarder.token is longer than 1023 bytes");
        }
        std::memcpy(forwarder.token, token.c_str(), token.size() + 1);
        forwarder.link = RCS_FORWARDER_HOST;
        if (f["refresh"].boolean()) {
            forwarder.refresh_token = onRefreshToken;
            forwarder.user_data = s.get();
        }
        config.connect.transport = RCS_TRANSPORT_FORWARDER;
        config.connect.address[0] = 0; // The channel leads to it.
        config.connect.port = 0;
        s->forwarder = true;
        chain.push_back(&forwarder.header);
    }
    rcs_decoder_config decoder = RCS_DECODER_CONFIG_INIT;
    const Json& d = options["decoder"];
    for (size_t i = 0; i < d["codecs"].items().size() && i < RCS_MAX_CODECS; ++i) {
        decoder.codecs[i] = d["codecs"].items()[i].u32();
    }
    for (size_t i = 0; i < d["formats"].items().size() && i < RCS_MAX_PICTURE_FORMATS; ++i) {
        decoder.formats[i] = d["formats"].items()[i].u32();
    }
    decoder.acceleration = d["acceleration"].u32(decoder.acceleration);
    chain.push_back(&decoder.header);
    rcs_view_request_config request = viewRequest(options["viewRequest"]);
    chain.push_back(&request.header);
    // The page's microphone (rcsdk_audio.js's recorder), with or without the browser's voice processing.
    rcs_send_audio_config sound = RCS_SEND_AUDIO_CONFIG_INIT;
    if (options["sendAudio"]["microphone"].boolean()) {
        sound.enabled = 1;
        sound.source = RCS_AUDIO_MICROPHONE;
        sound.voice_processing = options["sendAudio"]["voiceProcessing"].boolean(true);
        chain.push_back(&sound.header);
    }
    rcs_receive_audio_config hearing = RCS_RECEIVE_AUDIO_CONFIG_INIT;
    if (options.has("receiveAudio")) {
        hearing.enabled = options["receiveAudio"]["enabled"].boolean(true);
        chain.push_back(&hearing.header);
    }
    rcs_reconnect_config reconnect = RCS_RECONNECT_CONFIG_INIT;
    if (options.has("reconnect")) {
        reconnect.reconnect = options["reconnect"].boolean();
        chain.push_back(&reconnect.header);
    }
    // The clipboard: the page reads and writes its own (target HOST).
    rcs_clipboard_settings clipboard = RCS_CLIPBOARD_SETTINGS_INIT;
    rcs_module_config clipboardUse = RCS_MODULE_CONFIG_INIT;
    if (const Json& c = options["clipboard"]; c.kind() == Json::Object) {
        clipboard.flow = (c["send"].boolean() ? RCS_CLIPBOARD_SEND : 0) | (c["receive"].boolean() ? RCS_CLIPBOARD_RECEIVE : 0);
        clipboard.target = RCS_CLIPBOARD_HOST;
        clipboard.formats = c["formats"].u32();
        clipboard.max_bytes = c["maxBytes"].u32(clipboard.max_bytes);
        clipboard.send_existing = c["sendExisting"].boolean();
        clipboard.on_clipboard = onClipboard;
        clipboard.user_data = s.get();
        clipboardUse.module = RCS_CLIPBOARD_MODULE;
        clipboardUse.settings = &clipboard;
        clipboardUse.size = sizeof(clipboard);
        chain.push_back(&clipboardUse.header);
    }
    // File transfer: the page's files and storage (target HOST).
    rcs_file_transfer_settings files = RCS_FILE_TRANSFER_SETTINGS_INIT;
    rcs_module_config filesUse = RCS_MODULE_CONFIG_INIT;
    if (const Json& f = options["files"]; f.kind() == Json::Object) {
        files.flow = (f["send"].boolean() ? RCS_FILE_SEND : 0) | (f["receive"].boolean() ? RCS_FILE_RECEIVE : 0);
        files.target = RCS_FILE_HOST;
        files.max_items = f["maxItems"].u32(files.max_items);
        files.max_offer_bytes = f["maxOfferBytes"].u64(files.max_offer_bytes);
        files.offer_timeout_ms = f["offerTimeoutMs"].u32(files.offer_timeout_ms);
        files.reconnect_wait_ms = f["reconnectWaitMs"].u32(files.reconnect_wait_ms);
        files.max_bytes_per_second = f["maxBytesPerSecond"].u64();
        files.read = onFileRead;
        files.write = onFileWrite;
        files.resume = onFileResume;
        files.user_data = s.get();
        filesUse.module = RCS_FILE_TRANSFER_MODULE;
        filesUse.settings = &files;
        filesUse.size = sizeof(files);
        chain.push_back(&filesUse.header);
    }
    for (size_t i = 0; i + 1 < chain.size(); ++i) {
        chain[i]->next = chain[i + 1];
    }
    config.extensions = chain.front();
    rcs_session_callbacks callbacks = RCS_SESSION_CALLBACKS_INIT;
    callbacks.user_data = s.get();
    callbacks.on_event = onEvent;
    callbacks.on_gpu_frame = onGpuFrame;
    callbacks.on_data = onData;
    const rcs_result result = rcs_session_create(&config, &callbacks, &s->session);
    if (result != RCS_OK) {
        return result;
    }
    std::lock_guard lock(sessionsMutex);
    s->id = s->channel->id = nextSession++;
    sessions[s->id] = s;
    return s->id;
}

// Started from the page when rcsdk.mjs loads: the threads its workers are.
EMSCRIPTEN_KEEPALIVE pthread_t rcsw_files_thread() {
    return filesThread();
}

// start, stop and destroy: on the API thread; the page hears jsDone(call, result, why).
EMSCRIPTEN_KEEPALIVE void rcsw_start(int id, int call) {
    on(apiThread(), [id, call] {
        const auto s = find(id);
        const rcs_result result = s ? rcs_session_start(s->session) : RCS_ERROR_INVALID_ARGUMENT;
        const std::string why = result == RCS_OK ? "" : lastError();
        onPage([call, result, why] { jsDone(call, result, why.c_str()); });
    });
}

EMSCRIPTEN_KEEPALIVE void rcsw_stop(int id, int call) {
    on(apiThread(), [id, call] {
        const auto s = find(id);
        const rcs_result result = s ? rcs_session_stop(s->session) : RCS_ERROR_INVALID_ARGUMENT;
        onPage([call, result] { jsDone(call, result, ""); });
    });
}

EMSCRIPTEN_KEEPALIVE void rcsw_destroy(int id, int call) {
    std::shared_ptr<WebSession> s;
    {
        std::lock_guard lock(sessionsMutex);
        const auto found = sessions.find(id);
        if (found != sessions.end()) {
            s = found->second;
            sessions.erase(found);
        }
    }
    on(apiThread(), [s, call] {
        rcs_result result = RCS_ERROR_INVALID_ARGUMENT;
        if (s) {
            std::unique_lock calls(s->use); // The calls under way end first.
            result = rcs_session_destroy(s->session);
        }
        // What the threads still have queued for it ran before these (the same order).
        onMedia([s, call, result] { onPage([s, call, result] { jsDone(call, result, ""); }); });
    });
}

// Where the session's channel is: the media thread's WebSocket (1) or the page's own (0).
EMSCRIPTEN_KEEPALIVE int rcsw_socket(int id, int socket) {
    const auto s = find(id);
    if (!s) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    s->channel->socket = socket != 0;
    return RCS_OK;
}

// What arrived on the channel, and its end (on the thread the channel is on).
EMSCRIPTEN_KEEPALIVE int rcsw_receive(int id, const uint8_t* data, uint32_t size) {
    const auto s = find(id);
    return s ? rcs_session_transport_receive(s->session, 0, data, size) : RCS_ERROR_INVALID_ARGUMENT;
}

EMSCRIPTEN_KEEPALIVE int rcsw_closed(int id) {
    const auto s = find(id);
    return s ? rcs_session_transport_closed(s->session, 0) : RCS_ERROR_INVALID_ARGUMENT;
}

// The page's answer to jsRefreshToken: the new token, or 0 for none.
EMSCRIPTEN_KEEPALIVE void rcsw_token_done(void* call, const char* token) {
    auto* c = static_cast<TokenCall*>(call);
    c->given = token != nullptr;
    c->token = token ? token : "";
    emscripten_proxy_finish(c->ctx);
}

// The page replaced its channel with one to the same sharer (rcs_session_transport_restarted).
EMSCRIPTEN_KEEPALIVE int rcsw_restarted(int id) {
    const auto s = find(id);
    return s ? rcs_session_transport_restarted(s->session, 0) : RCS_ERROR_INVALID_ARGUMENT;
}

// One input event (rcs_input_event's fields the page sets).
EMSCRIPTEN_KEEPALIVE int rcsw_input(int id, uint32_t type, uint32_t x, uint32_t y, uint32_t stream, uint32_t code,
                                    uint32_t down, int32_t wheelX, int32_t wheelY, uint32_t repeat, int32_t dx,
                                    int32_t dy, uint32_t pressure, int32_t tiltX, int32_t tiltY, uint32_t rotation,
                                    uint32_t pen) {
    const auto s = find(id);
    if (!s) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    rcs_input_event event = RCS_INPUT_EVENT_INIT;
    event.type = type;
    event.x = x;
    event.y = y;
    event.stream = stream;
    event.code = code;
    event.down = down;
    event.wheel_x = wheelX;
    event.wheel_y = wheelY;
    event.repeat = repeat;
    event.dx = dx;
    event.dy = dy;
    event.pressure = pressure;
    event.tilt_x = tiltX;
    event.tilt_y = tiltY;
    event.rotation = rotation;
    event.pen = pen;
    return rcs_session_send_input(s->session, &event);
}

// A data message to the sharer (RCS_DATA_UNRELIABLE in flags or not).
EMSCRIPTEN_KEEPALIVE int rcsw_send_data(int id, uint32_t channel, const uint8_t* data, uint32_t size, uint32_t flags) {
    const auto s = find(id);
    if (!s) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    rcs_data_message message = RCS_DATA_MESSAGE_INIT;
    message.channel = channel;
    message.flags = flags;
    message.size = size;
    message.data = data;
    return rcs_session_send_data(s->session, &message);
}

EMSCRIPTEN_KEEPALIVE int rcsw_pause(int id, uint32_t what, int resume) {
    const auto s = find(id);
    return !s ? RCS_ERROR_INVALID_ARGUMENT
              : resume ? rcs_session_resume(s->session, what) : rcs_session_pause(s->session, what);
}

EMSCRIPTEN_KEEPALIVE int rcsw_request_keyframe(int id) {
    const auto s = find(id);
    return s ? rcs_session_request_keyframe(s->session) : RCS_ERROR_INVALID_ARGUMENT;
}

// What the viewer asks for now (rcs_session_update with RCS_EXT_VIEW_REQUEST;
// the options' viewRequest as JSON), on the API thread.
EMSCRIPTEN_KEEPALIVE void rcsw_update_request(int id, int call, const char* text) {
    on(apiThread(), [id, call, json = std::string(text)] {
        const auto s = find(id);
        const Json r = Json::parse(json.c_str());
        if (r.kind() != Json::Object) {
            onPage([call] { jsDone(call, RCS_ERROR_INVALID_ARGUMENT, notJson("The view request").c_str()); });
            return;
        }
        rcs_view_request_config request = viewRequest(r);
        const rcs_result result = s ? rcs_session_update(s->session, &request.header) : RCS_ERROR_INVALID_ARGUMENT;
        const std::string why = result == RCS_OK ? "" : lastError();
        onPage([call, result, why] { jsDone(call, result, why.c_str()); });
    });
}

// The session's statistics as JSON, its sections by name, the video's of
// `stream` (valid until the next call on this thread).
EMSCRIPTEN_KEEPALIVE const char* rcsw_stats(int id, uint32_t stream) {
    static thread_local std::string json;
    const auto s = find(id);
    if (!s) {
        return "{}";
    }
    rcs_video_receive_stats v = RCS_VIDEO_RECEIVE_STATS_INIT;
    rcs_session_get_stream_stats(s->session, stream, RCS_STATS_VIDEO_RECEIVE, &v);
    rcs_network_stats n = RCS_NETWORK_STATS_INIT;
    rcs_session_get_stats(s->session, RCS_STATS_NETWORK, &n);
    rcs_audio_receive_stats ar = RCS_AUDIO_RECEIVE_STATS_INIT;
    rcs_session_get_stats(s->session, RCS_STATS_AUDIO_RECEIVE, &ar);
    rcs_audio_send_stats as = RCS_AUDIO_SEND_STATS_INIT;
    rcs_session_get_stats(s->session, RCS_STATS_AUDIO_SEND, &as);
    rcs_input_stats in = RCS_INPUT_STATS_INIT;
    rcs_session_get_stats(s->session, RCS_STATS_INPUT, &in);
    rcs_data_stats da = RCS_DATA_STATS_INIT;
    rcs_session_get_stats(s->session, RCS_STATS_DATA, &da);
    JsonWriter video, network, audioReceive, audioSend, input, data;
    video.add("decoder", v.decoder).add("receivedPackets", v.received_packets).add("queuedPackets", v.queued_packets)
        .add("droppedPackets", v.dropped_packets).add("recoveryRequests", v.recovery_requests)
        .add("decoderResets", v.decoder_resets).add("decoderRecoveries", v.decoder_recoveries)
        .add("decodedFrames", v.decoded_frames).add("deliveredFrames", v.delivered_frames)
        .add("lastQueueNs", v.last_queue_ns).add("lastDecodeNs", v.last_decode_ns)
        .add("lastPipelineNs", v.last_pipeline_ns);
    network.add("path", n.path).add("roundTripNs", n.round_trip_ns).add("fecRecovered", n.fec_recovered)
        .add("resendRecovered", n.resend_recovered).add("framesLost", n.frames_lost)
        .add("soundWaitUs", n.sound_wait_us).add("soundQueueUs", n.sound_queue_us).add("bytes", n.bytes)
        .add("connections", n.connections).add("lastReceiveNs", n.last_receive_ns)
        .add("lastDecryptNs", n.last_decrypt_ns).add("hostRefused", n.host_refused).add("route", n.route)
        .add("pathLost", n.path_lost).add("pathChanges", n.path_changes)
        .add("lastInterruptionNs", n.last_interruption_ns).add("stalls", n.stalls).add("stallMs", n.stall_ms)
        .add("longestStallMs", n.longest_stall_ms).add("stallPeriodMs", n.stall_period_ms)
        .add("peerStallPeriodMs", n.peer_stall_period_ms).add("peerStallMs", n.peer_stall_ms);
    audioReceive.add("active", ar.active).add("codec", ar.codec).add("decoder", ar.decoder).add("device", ar.device)
        .add("playbackPeriodFrames", ar.playback_period_frames).add("playbackQueueFrames", ar.playback_queue_frames)
        .add("bufferMs", ar.buffer_ms).add("targetMs", ar.target_ms).add("receivedPackets", ar.received_packets)
        .add("lostPackets", ar.lost_packets).add("recoveredPackets", ar.recovered_packets)
        .add("concealedMs", ar.concealed_ms).add("droppedMs", ar.dropped_ms).add("underruns", ar.underruns)
        .add("deviceUnderruns", ar.device_underruns).add("playedFrames", ar.played_frames);
    audioSend.add("active", as.active).add("codec", as.codec).add("encoder", as.encoder)
        .add("bitrateBps", as.bitrate_bps).add("capturePeriodFrames", as.capture_period_frames)
        .add("capturedFrames", as.captured_frames).add("encodedPackets", as.encoded_packets)
        .add("encodedBytes", as.encoded_bytes).add("interruptions", as.interruptions)
        .add("lastEncodeNs", as.last_encode_ns);
    input.add("sent", in.sent).add("lastQueueNs", in.last_queue_ns).add("lastSendNs", in.last_send_ns);
    data.add("queuedBytes", da.queued_bytes).add("sentMessages", da.sent_messages).add("sentBytes", da.sent_bytes)
        .add("receivedMessages", da.received_messages).add("receivedBytes", da.received_bytes);
    JsonWriter all;
    all.raw("videoReceive", video.str()).raw("network", network.str()).raw("audioReceive", audioReceive.str())
        .raw("audioSend", audioSend.str()).raw("input", input.str()).raw("data", data.str());
    if (s->forwarder) {
        rcs_forwarder_stats f = RCS_FORWARDER_STATS_INIT;
        rcs_session_get_stats(s->session, RCS_STATS_FORWARDER, &f);
        JsonWriter forwarder;
        forwarder.add("peer", f.peer).add("generation", f.generation).add("step", f.step)
            .add("mediaFrames", f.media_frames).add("mediaBytes", f.media_bytes)
            .add("mediaRejected", f.media_rejected).add("mediaReplayed", f.media_replayed)
            .add("mediaUnkeyed", f.media_unkeyed).add("hopRoundTripNs", f.hop_round_trip_ns)
            .add("hopBytes", f.hop_bytes).add("soundPackets", f.sound_packets).add("link", f.link);
        all.raw("forwarder", forwarder.str());
    }
    json = all.str();
    return json.c_str();
}

// What the session knows now, as JSON: a stream as last announced, the peer, control, the desktop.
EMSCRIPTEN_KEEPALIVE const char* rcsw_info(int id, uint32_t stream) {
    static thread_local std::string json;
    const auto s = find(id);
    if (!s) {
        return "{}";
    }
    rcs_stream_info info = RCS_STREAM_INFO_INIT;
    rcs_session_get_stream_info(s->session, stream, &info);
    rcs_control_info control = RCS_CONTROL_INFO_INIT;
    rcs_session_get_control_info(s->session, &control);
    rcs_desktop_info desktop = RCS_DESKTOP_INFO_INIT;
    rcs_session_get_desktop_info(s->session, &desktop);
    rcs_peer_info peer = RCS_PEER_INFO_INIT;
    const bool connected = rcs_session_get_peer_info(s->session, &peer) == RCS_OK;
    JsonWriter c, k, p;
    c.add("allowed", control.allowed).add("granted", control.granted);
    k.add("kind", desktop.kind).add("userLoggedOn", desktop.user_logged_on);
    p.add("platform", connected ? peer.platform : 0).add("codecs", peer.codecs).add("inputTypes", peer.input_types)
        .add("sendsAudio", peer.sends_audio).add("receivesAudio", peer.receives_audio);
    JsonWriter all;
    all.raw("stream", streamInfoJson(info)).raw("control", c.str()).raw("desktop", k.str()).raw("peer", p.str());
    json = all.str();
    return json.c_str();
}

EMSCRIPTEN_KEEPALIVE const char* rcsw_last_error() {
    static thread_local std::string text;
    text = lastError();
    return text.c_str();
}

// ---- The clipboard --------------------------------------------------------

// A copy of the page's: {"viewerId","items":[{"format","size"}]} and the items' bytes one after another.
EMSCRIPTEN_KEEPALIVE int rcsw_clipboard_send(int id, const char* text, const uint8_t* bytes, uint32_t size) {
    const auto s = find(id);
    if (!s) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    const Json copy = Json::parse(text);
    if (copy.kind() != Json::Object) {
        return invalid(notJson("A clipboard copy").c_str());
    }
    std::vector<rcs_clipboard_item> items;
    uint64_t at = 0;
    for (const Json& item : copy["items"].items()) {
        rcs_clipboard_item one{};
        one.format = item["format"].u32();
        one.size = item["size"].u32();
        if (at + one.size > size) {
            return invalid("The clipboard items are larger than their bytes");
        }
        one.data = bytes + at;
        at += one.size;
        items.push_back(one);
    }
    rcs_clipboard_content content = RCS_CLIPBOARD_CONTENT_INIT;
    content.viewer_id = copy["viewerId"].u32();
    content.flow = RCS_CLIPBOARD_SEND;
    content.count = uint32_t(items.size());
    content.items = items.data();
    return rcs_clipboard_send(s->session, &content);
}

// What the sharer takes (peer), and the run's copies (stats), as JSON; "" with rcs_get_last_call_error.
EMSCRIPTEN_KEEPALIVE const char* rcsw_clipboard_info(int id) {
    static thread_local std::string json;
    const auto s = find(id);
    rcs_clipboard_peer peer = RCS_CLIPBOARD_PEER_INIT;
    rcs_clipboard_stats stats = RCS_CLIPBOARD_STATS_INIT;
    if (!s || rcs_clipboard_get_stats(s->session, 0, &stats) != RCS_OK) {
        return "";
    }
    const bool active = rcs_clipboard_get_peer(s->session, 0, &peer) == RCS_OK;
    JsonWriter p, t;
    p.add("formats", peer.formats).add("maxBytes", peer.max_bytes);
    t.add("sent", stats.sent).add("sentBytes", stats.sent_bytes).add("received", stats.received)
        .add("receivedBytes", stats.received_bytes).add("refused", stats.refused).add("failed", stats.failed);
    JsonWriter all;
    all.add("active", uint32_t(active)).raw("peer", p.str()).raw("stats", t.str());
    json = all.str();
    return json.c_str();
}

// ---- File transfer --------------------------------------------------------

// An offer: {"viewerId","items":[{"name","kind","size","modifiedMs"}],"resumeToken":hex}: the
// transfer's number, or a negative rcs_result.
EMSCRIPTEN_KEEPALIVE double rcsw_file_send(int id, const char* text) {
    const auto s = find(id);
    if (!s) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    const Json offer = Json::parse(text);
    if (offer.kind() != Json::Object) {
        return invalid(notJson("An offer").c_str());
    }
    std::vector<rcs_file_item> items;
    for (const Json& item : offer["items"].items()) {
        rcs_file_item one{};
        one.name = item["name"].text().c_str(); // The tree lives through the call.
        one.kind = item["kind"].u32(RCS_FILE_REGULAR);
        one.size = item["size"].u64();
        one.modified_ns = Json::to<int64_t>(item["modifiedMs"].number() * 1e6);
        items.push_back(one);
    }
    rcs_file_offer o = RCS_FILE_OFFER_INIT;
    o.viewer_id = offer["viewerId"].u32();
    o.count = uint32_t(items.size());
    o.items = items.data();
    if (offer.has("resumeToken") && !hexBytes(offer["resumeToken"].text(), o.resume_token, RCS_FILE_TOKEN_SIZE)) {
        return invalid("resumeToken must be 16 bytes");
    }
    uint64_t transfer = 0;
    const rcs_result result = rcs_file_send(s->session, &o, &transfer);
    return result == RCS_OK ? double(transfer) : double(result);
}

// 0 accept (conflict: rcs_file_conflict, the page's store's business), 1 reject (reason), 2 cancel, 3 pause, 4 go on.
EMSCRIPTEN_KEEPALIVE int rcsw_file_decide(int id, double transfer, int what, int conflict, const char* reason) {
    const auto s = find(id);
    if (!s) {
        return RCS_ERROR_INVALID_ARGUMENT;
    }
    const auto t = Json::to<uint64_t>(transfer);
    switch (what) {
    case 0: return rcs_file_accept(s->session, t, nullptr, rcs_file_conflict(conflict));
    case 1: return rcs_file_reject(s->session, t, reason);
    case 2: return rcs_file_cancel(s->session, t);
    case 3: return rcs_file_pause(s->session, t, 1);
    case 4: return rcs_file_pause(s->session, t, 0);
    default: return RCS_ERROR_INVALID_ARGUMENT;
    }
}

// A transfer as it is now with its items from `first` (at most `count`), or
// what the sharer allows (transfer 0), as JSON; "" with rcs_get_last_call_error.
EMSCRIPTEN_KEEPALIVE const char* rcsw_file_info(int id, double transfer, uint32_t first, uint32_t count) {
    static thread_local std::string json;
    const auto s = find(id);
    if (!s) {
        return "";
    }
    if (transfer == 0) {
        rcs_file_peer peer = RCS_FILE_PEER_INIT;
        if (rcs_file_get_peer(s->session, 0, &peer) != RCS_OK) {
            return "";
        }
        JsonWriter p;
        p.add("flow", peer.flow).add("maxItems", peer.max_items).add("maxOfferBytes", peer.max_offer_bytes);
        json = p.str();
        return json.c_str();
    }
    rcs_file_transfer_info info = RCS_FILE_TRANSFER_INFO_INIT;
    if (rcs_file_get_transfer(s->session, Json::to<uint64_t>(transfer), &info) != RCS_OK) {
        return "";
    }
    std::vector<rcs_file_item_info> items(count);
    uint32_t got = 0;
    const rcs_result listed = rcs_file_get_items(s->session, Json::to<uint64_t>(transfer), first, items.data(),
                                                 sizeof(rcs_file_item_info), count, &got);
    if (listed != RCS_OK && listed != RCS_ERROR_BUFFER_TOO_SMALL) {
        return "";
    }
    std::string list = "[";
    for (uint32_t i = 0; i < std::min(got, count); ++i) {
        list += (i ? "," : "") + fileItemJson(items[i]);
    }
    JsonWriter all;
    all.raw("transfer", fileTransferJson(info)).raw("items", list + "]");
    json = all.str();
    return json.c_str();
}

// The files thread's answer to a call of the module's (jsFileRead, jsFileWrite, jsFileResume).
EMSCRIPTEN_KEEPALIVE void rcsw_files_done(void* call, int result, double value, const char* message) {
    auto* c = static_cast<FileCall*>(call);
    c->result = result;
    c->value = Json::to<uint64_t>(value);
    c->message = message ? message : "";
    emscripten_proxy_finish(c->ctx);
}

} // extern "C"
