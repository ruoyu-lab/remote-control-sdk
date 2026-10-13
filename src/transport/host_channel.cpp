#include "transport/host_channel.hpp"

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <fcntl.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cstring>
#include <iterator>

namespace rcsdk::transport {

namespace {
constexpr size_t frameHeader = 2;
constexpr size_t maxHostDatagram = 65535;
} // namespace

HostChannel::HostChannel(HostKind kind, Send send) : kind_(kind), send_(std::move(send)) {}

void HostChannel::receive(std::span<const uint8_t> bytes) {
    if (bytes.empty()) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "No bytes: pass what arrived over the host's channel");
    }
    std::lock_guard lock(mutex_);
    if (kind_ == HostKind::datagram) {
        if (bytes.size() > maxHostDatagram) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "A datagram of " + std::to_string(bytes.size()) +
                                                 " bytes: the other end sends at most RCS_HOST_MAX_DATAGRAM "
                                                 "(pass each message as it came)");
        }
        if (queuedBytes_ + bytes.size() > maxQueued) {
            fail(RCS_ERROR_QUEUE_FULL, "The session fell behind what the host's channel delivers; the datagram "
                                       "is dropped");
        }
        queue(std::vector<uint8_t>(bytes.begin(), bytes.end()));
        return;
    }
    if (queuedBytes_ + partial_.size() + bytes.size() > maxQueued) {
        queue_.clear();
        queuedBytes_ = 0;
        partial_.clear();
        breakConnection();
        fail(RCS_ERROR_QUEUE_FULL, "The session fell behind the host's stream, which lost bytes; the connection "
                                   "over it ends");
    }
    partial_.insert(partial_.end(), bytes.begin(), bytes.end());
    size_t at = 0;
    while (partial_.size() - at >= frameHeader) {
        const size_t length = size_t(partial_[at]) << 8 | partial_[at + 1];
        if (partial_.size() - at - frameHeader < length) {
            break;
        }
        const auto start = partial_.begin() + ptrdiff_t(at + frameHeader);
        if (length) {
            queue(std::vector<uint8_t>(start, start + ptrdiff_t(length)));
        }
        at += frameHeader + length;
    }
    partial_.erase(partial_.begin(), partial_.begin() + ptrdiff_t(at));
}

void HostChannel::closed() {
    std::lock_guard lock(mutex_);
    if (reader_) { // Read before it fails: the other end's last words (its goodbye) may be among them.
        auto& before = reader_->beforeClose_;
        before.insert(before.end(), std::make_move_iterator(queue_.begin()), std::make_move_iterator(queue_.end()));
    }
    queue_.clear();
    queuedBytes_ = 0;
    partial_.clear();
    breakConnection();
}

void HostChannel::restarted() {
    std::lock_guard lock(mutex_);
    queue_.clear();
    queuedBytes_ = 0;
    partial_.clear();
    ++restarts_;
    if (reader_) {
        reader_->wake();
    }
}

void HostChannel::queue(std::vector<uint8_t> datagram) {
    queuedBytes_ += datagram.size();
    queue_.push_back(std::move(datagram));
    if (reader_) {
        reader_->wake();
    }
}

void HostChannel::breakConnection() {
    if (reader_) {
        reader_->broken_ = true;
        reader_->wake();
    }
}

// ---- HostSocket -----------------------------------------------------------------------

HostSocket::HostSocket(std::shared_ptr<HostChannel> channel) : channel_(std::move(channel)) {
#if defined(_WIN32)
    watchable();
#elif !defined(__EMSCRIPTEN__)
    if (::pipe(pipe_) != 0) {
        fail(RCS_ERROR_INTERNAL, "Could not create the host channel's wake pipe");
    }
    for (const int end : pipe_) {
        fcntl(end, F_SETFL, fcntl(end, F_GETFL) | O_NONBLOCK);
        fcntl(end, F_SETFD, FD_CLOEXEC);
    }
#endif
    std::lock_guard lock(channel_->mutex_);
    channel_->reader_ = this;
    if (!channel_->queue_.empty()) {
        wake();
    }
}

HostSocket::~HostSocket() {
    {
        std::lock_guard lock(channel_->mutex_);
        if (channel_->reader_ == this) {
            channel_->reader_ = nullptr;
        }
    }
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    for (const int end : pipe_) {
        ::close(end);
    }
#endif
}

Endpoint HostSocket::placeholder() {
    static const Endpoint value = Endpoint::parse("0.0.0.0", 0);
    return value;
}

Handle HostSocket::handle() const noexcept {
#if defined(_WIN32) || defined(__EMSCRIPTEN__)
    return invalidHandle; // Waiters use the event (readable()), or are woken (attachWaiter).
#else
    return pipe_[0];
#endif
}

bool HostSocket::send(const Endpoint&, std::span<const uint8_t> datagram) {
    bool taken;
    if (channel_->kind() == HostKind::stream) {
        if (datagram.size() > maxHostDatagram) {
            fail(RCS_ERROR_INTERNAL, "A datagram too large for the host's stream");
        }
        std::vector<uint8_t> frame(frameHeader + datagram.size());
        frame[0] = uint8_t(datagram.size() >> 8);
        frame[1] = uint8_t(datagram.size());
        std::memcpy(frame.data() + frameHeader, datagram.data(), datagram.size());
        taken = channel_->send_(frame);
    } else {
        taken = channel_->send_(datagram);
    }
    if (!taken) {
        ++channel_->refused_;
    }
    return taken;
}

std::optional<size_t> HostSocket::receive(std::span<uint8_t> buffer, Endpoint& from) {
    std::unique_lock lock(channel_->mutex_);
    const auto take = [&](std::deque<std::vector<uint8_t>>& queue, size_t* queuedBytes) -> std::optional<size_t> {
        while (!queue.empty()) {
            auto datagram = std::move(queue.front());
            queue.pop_front();
            if (queuedBytes) {
                *queuedBytes -= datagram.size();
            }
            if (datagram.size() > buffer.size()) {
                continue; // As a socket drops a datagram larger than the buffer.
            }
            std::copy(datagram.begin(), datagram.end(), buffer.begin());
            from = placeholder();
            return datagram.size();
        }
        return std::nullopt;
    };
    if (const auto size = take(beforeClose_, nullptr)) {
        return size;
    }
    if (broken_) {
        broken_ = false;
        drain();
        lock.unlock();
        fail(RCS_ERROR_NETWORK, "The host's channel to the other end closed");
    }
    if (const auto size = take(channel_->queue_, &channel_->queuedBytes_)) {
        return size;
    }
    drain();
    return std::nullopt;
}

bool HostSocket::buffered() const noexcept {
    std::lock_guard lock(channel_->mutex_);
    return broken_ || !beforeClose_.empty() || !channel_->queue_.empty();
}

#ifdef __EMSCRIPTEN__
bool HostSocket::attachWaiter(Waiter& waiter) const noexcept {
    std::lock_guard lock(channel_->mutex_);
    waiters_.push_back(&waiter);
    return true;
}

void HostSocket::detachWaiter(Waiter& waiter) const noexcept {
    std::lock_guard lock(channel_->mutex_);
    std::erase(waiters_, &waiter);
}
#endif

// Holding the channel's mutex.
void HostSocket::wake() noexcept {
#if defined(_WIN32)
    signal();
#elif defined(__EMSCRIPTEN__)
    for (auto* waiter : waiters_) {
        waiter->wake();
    }
#else
    const uint8_t byte = 1;
    [[maybe_unused]] const auto written = ::write(pipe_[1], &byte, 1); // Full: it is readable already.
#endif
}

void HostSocket::drain() noexcept {
#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
    uint8_t bytes[64];
    while (::read(pipe_[0], bytes, sizeof(bytes)) > 0) {
    }
#endif
}

} // namespace rcsdk::transport
