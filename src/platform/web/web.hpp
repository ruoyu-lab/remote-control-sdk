// The browser's platform layer: what the page found out before the first
// session (web/rcsdk.mjs probes WebCodecs when it loads, then hands it in),
// and the threads the page's JavaScript runs on.
#pragma once

#include "platform/platform.hpp"

#include <functional>
#include <pthread.h>

namespace rcsdk::platform {

// The picture formats the browser decodes `codec` in, as (1u << rcs_picture_format)
// bits, in hardware or in software; 0: not at all.
uint32_t webDecoderFormats(rcs_codec codec, bool hardware) noexcept;
// "WebCodecs VP9 (hardware)": a decoder's name, as the implementations list it.
std::string webDecoderName(rcs_codec codec, bool hardware);

// The SDK's media thread: a worker of the page's that keeps its event loop.
// The browser's decoders run there, the views' canvases are drawn there and a
// session's WebSocket lives there, so that pictures go from the network to
// the screen without the page's main thread. Started from the page
// (rcsdk_web_media_thread, when rcsdk.mjs loads).
pthread_t mediaThread();

// Runs `work` later, in order, on the page's main thread (what needs the
// document: AudioContext, getUserMedia, events) or on the media thread.
void onPage(std::function<void()> work);
void onMedia(std::function<void()> work);

} // namespace rcsdk::platform

// The media thread's number, for the page (Module.rcsdk finds its worker)
// and the package's glue.
extern "C" pthread_t rcsdk_web_media_thread(void);
