// A cancellable byte stream between two ends: a TCP connection, or a local
// endpoint of this machine (transport/local_stream.hpp). The secure channel
// runs on any of them.
#pragma once

#include "util/util.hpp"

#include <chrono>
#include <cstdint>
#include <span>

namespace rcsdk::transport {

// What every implementation guarantees (SecureChannel builds whole records on it):
//  - read and write transfer exactly the whole buffer or throw: DISCONNECTED
//    when the peer closed, CANCELLED, or NETWORK for the rest.
//  - `timeout` bounds the time without progress, not the whole transfer: a
//    large record on a slow path takes as long as it needs while bytes move.
//  - CANCELLED means that not one byte of the buffer was transferred. A
//    transfer under way when the stream's token is set is finished first;
//    the implementation may bound that time (TCP: 250 ms) and then fails with
//    NETWORK. After any failure part-way the stream carries nothing more.
class Stream {
public:
    virtual ~Stream() = default;

    virtual void read(std::span<uint8_t> buffer, std::chrono::milliseconds timeout) = 0;
    virtual void write(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) = 0;
    // The last words of a connection: like write, but cancellation does not end
    // it, and it waits (for `timeout` at most, in all) until the peer has read
    // them and closed its end, so that they are not lost to a reset when this
    // end closes with data of the peer's still unread.
    virtual void writeFinal(std::span<const uint8_t> buffer, std::chrono::milliseconds timeout) = 0;

    virtual uint64_t bytesTransferred() const noexcept = 0;
};

} // namespace rcsdk::transport
