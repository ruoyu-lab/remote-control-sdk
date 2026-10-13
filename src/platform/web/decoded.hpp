// What a browser's decoder hands back (decoder.cpp), apart from the browser.
// Each packet goes to WebCodecs as a chunk its decoder numbers (the chunk's
// timestamp, which its picture carries), and the SDK's decoding thread waits
// for that chunk's picture: a picture of another chunk (one whose wait gave
// up; a second picture of one chunk, as an AV1 temporal unit that shows two
// frames gives) is not kept, and its VideoFrame is closed at once. A picture
// must be of the size its packet says (the stream's, checked as it arrived):
// a canvas takes the size of what it draws, and the sharer's bitstream is not
// to choose it.
#pragma once

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>

namespace rcsdk::platform {

// A picture (the media thread's VideoFrame by number, its size, its chunk) or
// an error (frame 0, the browser's message).
struct Decoded {
    int frame = 0;
    uint32_t width = 0, height = 0;
    double chunk = 0;
    std::string error;
};

// One decoder's: what came, and the chunk whose picture is waited for (0: none).
struct DecodedSlot {
    std::mutex mutex;
    std::condition_variable arrived;
    std::deque<Decoded> outcomes;
    double waiting = 0;

    // The media thread: keeps `outcome` for the wait; false for a picture no
    // wait is for (not kept: its frame is to be closed). Errors are kept.
    bool keep(Decoded outcome) {
        {
            std::lock_guard lock(mutex);
            if (outcome.frame &&
                (outcome.chunk != waiting ||
                 std::any_of(outcomes.begin(), outcomes.end(), [](const Decoded& o) { return o.frame != 0; }))) {
                return false;
            }
            outcomes.push_back(std::move(outcome));
        }
        arrived.notify_one();
        return true;
    }
};

// Why a picture of `width` x `height` is not shown for a packet of a stream
// of `streamWidth` x `streamHeight`; empty where it is.
inline std::string sizeProblem(uint32_t width, uint32_t height, uint32_t streamWidth, uint32_t streamHeight) {
    if (width == streamWidth && height == streamHeight) {
        return {};
    }
    return "The browser decoded a picture of " + std::to_string(width) + "x" + std::to_string(height) +
           " where the stream is " + std::to_string(streamWidth) + "x" + std::to_string(streamHeight) + ": not shown";
}

} // namespace rcsdk::platform
