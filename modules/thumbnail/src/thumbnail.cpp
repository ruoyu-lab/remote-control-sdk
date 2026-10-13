// "rcsdk.thumbnail": the session's half asks for a picture and waits for it;
// the machine's half, where the session's agent runs, takes it there with
// rcs_capture_picture on a thread of its own, one at a time, and answers.
//
// Their messages, little-endian:
//   ask     u32 serial, u32 max_width, u32 max_height, then the capture's
//           fields: u32 source, display_id, region_x, region_y, region_width,
//           region_height, u64 window_id, u32 process_id,
//           exclude_current_process, secure_desktop
//   answer  u32 serial, i32 result, u32 width, u32 height, u32 reason size,
//           the reason (UTF-8), then the pixels (width * height * 4 bytes)
#include <rcsdk/thumbnail.h>

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

namespace rcsdk::thumbnail {
namespace {

using namespace std::chrono_literals;

constexpr uint32_t moduleVersion = 1;
constexpr uint32_t maxSide = 8192;
constexpr uint32_t maxReason = 1024;
constexpr uint32_t answerHeader = 20;
constexpr uint32_t maxMessageBytes = answerHeader + maxReason + maxSide * maxSide * 4;
constexpr auto answerWait = 5s;

using Bytes = std::vector<uint8_t>;

class Writer {
public:
    Writer& u32(uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            bytes_.push_back(uint8_t(value >> (8 * i)));
        }
        return *this;
    }
    Writer& u64(uint64_t value) { return u32(uint32_t(value)).u32(uint32_t(value >> 32)); }
    Writer& raw(std::span<const uint8_t> data) {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
        return *this;
    }
    Bytes take() { return std::move(bytes_); }

private:
    Bytes bytes_;
};

// Reads what a message holds; ok() is false once it held less than was read.
class Reader {
public:
    explicit Reader(std::span<const uint8_t> data) : data_(data) {}
    uint32_t u32() {
        if (data_.size() - at_ < 4 || failed_) {
            failed_ = true;
            return 0;
        }
        uint32_t value = 0;
        for (int i = 0; i < 4; ++i) {
            value |= uint32_t(data_[at_ + size_t(i)]) << (8 * i);
        }
        at_ += 4;
        return value;
    }
    uint64_t u64() {
        const uint64_t low = u32();
        return low | uint64_t(u32()) << 32;
    }
    std::span<const uint8_t> raw(size_t size) {
        if (data_.size() - at_ < size || failed_) {
            failed_ = true;
            return {};
        }
        const auto part = data_.subspan(at_, size);
        at_ += size;
        return part;
    }
    size_t remaining() const { return failed_ ? 0 : data_.size() - at_; }
    bool ok() const { return !failed_; }

private:
    std::span<const uint8_t> data_;
    size_t at_ = 0;
    bool failed_ = false;
};

struct Ask {
    uint32_t serial = 0, maxWidth = 0, maxHeight = 0;
    rcs_capture_config what = RCS_CAPTURE_CONFIG_INIT;
};

Bytes encode(const Ask& a) {
    const auto& c = a.what;
    return Writer()
        .u32(a.serial)
        .u32(a.maxWidth)
        .u32(a.maxHeight)
        .u32(c.source)
        .u32(c.display_id)
        .u32(c.region_x)
        .u32(c.region_y)
        .u32(c.region_width)
        .u32(c.region_height)
        .u64(c.window_id)
        .u32(c.process_id)
        .u32(c.exclude_current_process)
        .u32(c.secure_desktop)
        .take();
}

bool decode(std::span<const uint8_t> data, Ask& a) {
    Reader r(data);
    auto& c = a.what;
    a.serial = r.u32();
    a.maxWidth = r.u32();
    a.maxHeight = r.u32();
    c.source = r.u32();
    c.display_id = r.u32();
    c.region_x = r.u32();
    c.region_y = r.u32();
    c.region_width = r.u32();
    c.region_height = r.u32();
    c.window_id = r.u64();
    c.process_id = r.u32();
    c.exclude_current_process = r.u32();
    c.secure_desktop = r.u32();
    return r.ok() && !r.remaining();
}

// An answer as the session's half keeps it.
struct Answer {
    rcs_result result = RCS_OK;
    std::string reason;
    uint32_t width = 0, height = 0;
    Bytes pixels;
};

Bytes encode(uint32_t serial, const Answer& a) {
    const std::string reason = a.reason.substr(0, maxReason);
    return Writer()
        .u32(serial)
        .u32(uint32_t(a.result))
        .u32(a.width)
        .u32(a.height)
        .u32(uint32_t(reason.size()))
        .raw({reinterpret_cast<const uint8_t*>(reason.data()), reason.size()})
        .raw(a.pixels)
        .take();
}

bool decode(std::span<const uint8_t> data, uint32_t& serial, Answer& a) {
    Reader r(data);
    serial = r.u32();
    a.result = rcs_result(r.u32());
    a.width = r.u32();
    a.height = r.u32();
    const uint32_t size = r.u32();
    if (!r.ok() || size > maxReason) {
        return false;
    }
    const auto reason = r.raw(size);
    a.reason.assign(reason.begin(), reason.end());
    const auto pixels = r.raw(r.remaining());
    a.pixels.assign(pixels.begin(), pixels.end());
    return r.ok();
}

// ---- The session's half -------------------------------------------------------------------

// Asks waiting for their answers; the module's calls and the askers' threads
// both use it.
class Asker {
public:
    // What is asked, and its answer once it came.
    struct Waiting {
        uint32_t maxWidth = 0, maxHeight = 0;
        std::optional<Answer> answer;
    };

    void start() {
        std::lock_guard lock(mutex_);
        running_ = true;
    }
    void machine(bool up) {
        std::lock_guard lock(mutex_);
        machineUp_ = up;
        if (!up) {
            endAll(RCS_ERROR_INVALID_STATE, "The machine that takes the picture left before it answered");
        }
    }
    void stop() {
        std::lock_guard lock(mutex_);
        running_ = false;
        machineUp_ = false;
        endAll(RCS_ERROR_INVALID_STATE, "The session stopped before the picture was taken");
    }

    // Inside one of the module's calls: sends `ask` (its serial set) once
    // the machine's half runs.
    rcs_result send(rcs_module module, Ask& ask, std::shared_ptr<Waiting>& waiting) {
        std::lock_guard lock(mutex_);
        if (!running_) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE, "The session is not running");
        }
        if (!machineUp_) {
            return rcs_set_last_call_error(RCS_ERROR_INVALID_STATE,
                                           "The module does not run on the machine the session captures: no agent "
                                           "is attached, or its process did not register rcsdk.thumbnail");
        }
        ask.serial = ++serial_;
        const Bytes bytes = encode(ask);
        if (const rcs_result sent = rcs_module_send_machine(module, bytes.data(), uint32_t(bytes.size()));
            sent != RCS_OK) {
            return sent;
        }
        waiting = std::make_shared<Waiting>();
        waiting->maxWidth = ask.maxWidth;
        waiting->maxHeight = ask.maxHeight;
        waiting_[ask.serial] = waiting;
        return RCS_OK;
    }

    // An answer from the machine's half; false for one that is malformed or
    // larger than asked for.
    bool answered(rcs_module module, std::span<const uint8_t> data) {
        uint32_t serial = 0;
        Answer answer;
        if (!decode(data, serial, answer)) {
            return false;
        }
        std::lock_guard lock(mutex_);
        const auto found = waiting_.find(serial);
        if (found == waiting_.end()) {
            return true; // Its asker gave up.
        }
        auto& waiting = *found->second;
        const bool sized = answer.width && answer.height && answer.width <= waiting.maxWidth &&
                           answer.height <= waiting.maxHeight &&
                           answer.pixels.size() == uint64_t(answer.width) * answer.height * 4;
        if (answer.result == RCS_OK ? !sized : !answer.pixels.empty()) {
            rcs_module_log(module, RCS_LOG_WARNING, "The machine's half answered with a picture not as asked");
            answer = {RCS_ERROR_PROTOCOL, "The machine's half answered with a picture not as asked", 0, 0, {}};
        }
        waiting.answer = std::move(answer);
        waiting_.erase(found);
        answered_.notify_all();
        return true;
    }

    // Outside the module's calls: the answer, or why none came in time.
    Answer wait(const std::shared_ptr<Waiting>& waiting) {
        std::unique_lock lock(mutex_);
        if (!answered_.wait_for(lock, answerWait, [&] { return waiting->answer.has_value(); })) {
            for (auto at = waiting_.begin(); at != waiting_.end(); ++at) {
                if (at->second == waiting) {
                    waiting_.erase(at);
                    break;
                }
            }
            return {RCS_ERROR_TIMEOUT, "The machine did not take the picture within five seconds", 0, 0, {}};
        }
        return std::move(*waiting->answer);
    }

private:
    void endAll(rcs_result result, const char* why) { // Holding mutex_.
        for (auto& [serial, waiting] : waiting_) {
            waiting->answer = Answer{result, why, 0, 0, {}};
        }
        waiting_.clear();
        answered_.notify_all();
    }

    std::mutex mutex_;
    std::condition_variable answered_;
    bool running_ = false, machineUp_ = false;
    uint32_t serial_ = 0;
    std::map<uint32_t, std::shared_ptr<Waiting>> waiting_;
};

// The session's half's state, made at its first use, kept for the session's life.
Asker& asker(rcs_module module) {
    if (auto* made = static_cast<Asker*>(rcs_module_state(module))) {
        return *made;
    }
    auto* made = new Asker();
    rcs_module_set_state(module, made, [](void* state) { delete static_cast<Asker*>(state); });
    return *made;
}

// ---- The machine's half -------------------------------------------------------------------

// Takes the pictures asked for, one at a time, on its own thread: a capture
// may wait for the system for seconds, and the host's other modules go on.
class Taker {
public:
    explicit Taker(rcs_module module) : module_(module), thread_([this] { run(); }) {}
    ~Taker() {
        {
            std::lock_guard lock(mutex_);
            quit_ = true;
        }
        wake_.notify_all();
        thread_.join();
    }

    void ask(std::span<const uint8_t> data) {
        Ask ask;
        if (!decode(data, ask)) {
            rcs_module_log(module_, RCS_LOG_WARNING, "The session's half asked for a picture in a malformed message");
            return;
        }
        {
            std::lock_guard lock(mutex_);
            asks_.push_back(ask);
        }
        wake_.notify_all();
    }

private:
    void run() {
        std::unique_lock lock(mutex_);
        for (;;) {
            wake_.wait(lock, [&] { return quit_ || !asks_.empty(); });
            if (quit_) {
                return;
            }
            const Ask ask = asks_.front();
            asks_.pop_front();
            lock.unlock();
            const Bytes answer = encode(ask.serial, take(ask));
            rcs_module_send_machine(module_, answer.data(), uint32_t(answer.size()));
            lock.lock();
        }
    }

    static Answer take(const Ask& ask) {
        Answer answer;
        rcs_picture picture = RCS_PICTURE_INIT;
        const bool sized = ask.maxWidth && ask.maxHeight && ask.maxWidth <= maxSide && ask.maxHeight <= maxSide;
        Bytes pixels(sized ? size_t(ask.maxWidth) * ask.maxHeight * 4 : 0);
        picture.data = pixels.data();
        picture.capacity = pixels.size();
        answer.result = rcs_capture_picture(&ask.what, ask.maxWidth, ask.maxHeight, &picture);
        if (answer.result != RCS_OK) {
            char reason[maxReason] = {};
            rcs_get_last_call_error(reason, sizeof(reason));
            answer.reason = reason;
            return answer;
        }
        answer.width = picture.width;
        answer.height = picture.height;
        pixels.resize(size_t(picture.data_size));
        answer.pixels = std::move(pixels);
        return answer;
    }

    const rcs_module module_;
    std::mutex mutex_;
    std::condition_variable wake_;
    std::deque<Ask> asks_;
    bool quit_ = false;
    std::thread thread_; // Last: it starts once the rest exists.
};

// What a call does, its failures logged: they never reach the SDK.
template <class Call>
void guarded(rcs_module module, Call&& call) noexcept {
    try {
        call();
    } catch (const std::exception& error) {
        rcs_module_log(module, RCS_LOG_ERROR, error.what());
    }
}

rcs_module_definition makeDefinition() {
    rcs_module_definition d{};
    d.struct_size = sizeof(d);
    d.id = RCS_THUMBNAIL_MODULE;
    d.version = moduleVersion;
    d.flags = RCS_MODULE_MACHINE;
    d.roles = 1u << RCS_ROLE_SHARE;
    d.max_message_bytes = maxMessageBytes;
    d.check = [](rcs_role, const void* settings, uint32_t size, char* reason, uint32_t capacity,
                 void*) -> rcs_result {
        if (!settings && !size) {
            return RCS_OK;
        }
        if (capacity) {
            std::snprintf(reason, capacity, "%s", "rcsdk.thumbnail takes no settings (settings NULL, size 0)");
        }
        return RCS_ERROR_INVALID_ARGUMENT;
    };
    d.start = [](rcs_module module, rcs_role, const void*, uint32_t, void*) {
        guarded(module, [&] { asker(module).start(); });
    };
    d.machine_joined = [](rcs_module module, void*) { guarded(module, [&] { asker(module).machine(true); }); };
    d.machine_left = [](rcs_module module, void*) { guarded(module, [&] { asker(module).machine(false); }); };
    d.from_machine = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] {
            if (!asker(module).answered(module, {data, size})) {
                rcs_module_log(module, RCS_LOG_WARNING, "The machine's half answered in a malformed message");
            }
        });
    };
    d.stop = [](rcs_module module, void*) { guarded(module, [&] { asker(module).stop(); }); };
    d.machine_start = [](rcs_module module, rcs_role, void*) {
        guarded(module, [&] {
            rcs_module_set_state(module, new Taker(module), [](void* state) { delete static_cast<Taker*>(state); });
        });
    };
    d.from_session = [](rcs_module module, const uint8_t* data, uint32_t size, void*) {
        guarded(module, [&] {
            if (auto* taker = static_cast<Taker*>(rcs_module_state(module))) {
                taker->ask({data, size});
            }
        });
    };
    d.machine_stop = [](rcs_module module, void*) { rcs_module_set_state(module, nullptr, nullptr); };
    return d;
}

} // namespace
} // namespace rcsdk::thumbnail

using namespace rcsdk::thumbnail;

extern "C" {

const rcs_module_definition* rcs_thumbnail_module(void) {
    static const rcs_module_definition definition = makeDefinition();
    return &definition;
}

rcs_result rcs_thumbnail_capture(rcs_session session, const rcs_capture_config* what, uint32_t max_width,
                                 uint32_t max_height, rcs_picture* out) {
    if (!session) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "session is NULL");
    }
    if (!out || out->struct_size != sizeof(*out)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "Initialise rcs_picture with RCS_PICTURE_INIT from this library's headers");
    }
    if (!out->data && out->capacity) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "rcs_picture: data is NULL, capacity is not 0");
    }
    if (!what || what->header.type != RCS_EXT_CAPTURE || what->header.struct_size != sizeof(*what)) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "what: an RCS_EXT_CAPTURE extension, initialised with "
                                       "RCS_CAPTURE_CONFIG_INIT from this library's headers");
    }
    if (what->header.next) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT,
                                       "what: next must be NULL: a picture takes the capture's settings alone");
    }
    if (!max_width || !max_height || max_width > maxSide || max_height > maxSide) {
        return rcs_set_last_call_error(RCS_ERROR_INVALID_ARGUMENT, "max_width and max_height must be 1..8192");
    }
    rcs_module module = nullptr;
    if (const rcs_result found = rcs_module_find(session, RCS_THUMBNAIL_MODULE, &module); found != RCS_OK) {
        return found;
    }
    struct Asking {
        Ask ask;
        std::shared_ptr<Asker::Waiting> waiting;
    } asking;
    asking.ask.maxWidth = max_width;
    asking.ask.maxHeight = max_height;
    asking.ask.what = *what;
    const rcs_result sent = rcs_module_run(
        module,
        [](rcs_module m, void* argument) -> rcs_result {
            auto& a = *static_cast<Asking*>(argument);
            try {
                return asker(m).send(m, a.ask, a.waiting);
            } catch (const std::exception& error) {
                return rcs_set_last_call_error(RCS_ERROR_INTERNAL, error.what());
            }
        },
        &asking);
    if (sent != RCS_OK) {
        return sent;
    }
    const Answer answer = asker(module).wait(asking.waiting);
    if (answer.result != RCS_OK) {
        return rcs_set_last_call_error(answer.result, answer.reason.c_str());
    }
    out->width = answer.width;
    out->height = answer.height;
    out->stride_bytes = answer.width * 4;
    out->format = RCS_PIXEL_BGRA8;
    out->data_size = answer.pixels.size();
    if (out->capacity < out->data_size) {
        return rcs_set_last_call_error(RCS_ERROR_BUFFER_TOO_SMALL,
                                       ("The picture takes " + std::to_string(out->data_size) +
                                        " bytes; data holds " + std::to_string(out->capacity))
                                           .c_str());
    }
    std::memcpy(out->data, answer.pixels.data(), answer.pixels.size());
    return rcs_set_last_call_error(RCS_OK, nullptr);
}

} // extern "C"
