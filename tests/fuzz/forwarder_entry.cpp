// libFuzzer: the forwarder's entries reading a connection's bytes
// (server/forwarder/src/entry.hpp, EntryReader: the TCP entry's lengths; the
// WebSocket entry's opening, then its frames), and an end's side of them
// (transport/framed_socket.hpp: the server's answer to its opening, then the
// server's frames; the TCP entry's lengths): what a peer's bytes do to the
// readers' state and buffers over many reads.
//
// The input's first byte picks the reader, and whether the stream starts
// with a valid opening (the WebSocket entry: a request; an end: the answer
// to its own request), so that the frames are reached. Then steps, by their
// first byte, append to the stream: raw bytes; a WebSocket frame whose
// header, length (or one around the edges: 125, 126, 65 535, 65 536, past a
// message's most, past 2^63, in a longer form than it needs) and masking the
// input gives; a datagram behind a length the input gives; or hand the
// stream over in a read of a size the input gives (what is left goes at the
// end). A reader goes on until it says the connection closes, or throws
// Failure (as the entry or the end closes the connection); anything else is
// a finding. Checked as it goes: each datagram 1..65 535 bytes, what goes
// back to the peer no more than what came plus an answer, nothing taken once
// closed.
#include "../../server/forwarder/src/entry.hpp"
#include "transport/framed_socket.hpp"
#include "transport/websocket.hpp"
#include "util/bytes.hpp"

#include <cstdlib>

using namespace rcsdk;
namespace ws = rcsdk::transport::websocket;

namespace {

// The input, read from its start; zeros once it ran out.
class Steps {
public:
    Steps(const uint8_t* data, size_t size) : data_(data), size_(size) {}
    bool more() const noexcept { return at_ < size_; }
    uint8_t byte() noexcept { return at_ < size_ ? data_[at_++] : 0; }
    uint16_t u16() noexcept { return uint16_t(byte() << 8 | byte()); }
    Bytes bytes(size_t count) {
        count = std::min(count, size_ - at_);
        Bytes out(data_ + at_, data_ + at_ + count);
        at_ += count;
        return out;
    }

private:
    const uint8_t* data_;
    size_t size_, at_ = 0;
};

void check(bool ok) {
    if (!ok) {
        std::abort();
    }
}

// A frame as the input spells it: FIN and the opcode (the reserved bits
// when it asks), a length (or one around the edges, in a longer form than
// it needs, or past 2^63), masked as `masking` says unless it asks
// otherwise; its payload the input's bytes, zeros beyond them.
Bytes frameOf(Steps& in, bool masking) {
    static constexpr uint64_t edges[] = {0, 1, 2, 125, 126, 127, 65535, 65536, 70000};
    const uint8_t head = in.byte(), flags = in.byte(), size = in.byte();
    Bytes out{uint8_t(head & (flags & 1 ? 0xFF : 0x8F))};
    const bool masked = masking != bool(flags & 2);
    const uint64_t length = flags & 4 ? edges[size % std::size(edges)] : size & 0x7F;
    const uint8_t maskBit = masked ? 0x80 : 0;
    if (flags & 32) {
        out.push_back(uint8_t(maskBit | 127));
        appendBigEndian(out, 0x8000000000000000ull | size, 8);
        return out;
    }
    if (length < 126 && !(flags & 8)) {
        out.push_back(uint8_t(maskBit | length));
    } else if (length < 65536 && !(flags & 16)) {
        out.push_back(uint8_t(maskBit | 126));
        appendBigEndian(out, length, 2);
    } else {
        out.push_back(uint8_t(maskBit | 127));
        appendBigEndian(out, length, 8);
    }
    if (masked) {
        auto key = in.bytes(4);
        key.resize(4);
        out.insert(out.end(), key.begin(), key.end());
    }
    auto payload = in.bytes(size_t(std::min<uint64_t>(length, 64)));
    payload.resize(size_t(length));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// One step that adds to the stream (false: a read, which the caller does).
bool append(Steps& in, uint8_t step, bool masking, Bytes& stream) {
    switch (step % 4) {
    case 0: { // Raw bytes.
        const auto raw = in.bytes(in.byte());
        stream.insert(stream.end(), raw.begin(), raw.end());
        return true;
    }
    case 1: {
        const auto frame = frameOf(in, masking);
        stream.insert(stream.end(), frame.begin(), frame.end());
        return true;
    }
    case 2: { // A datagram behind a length.
        const uint16_t length = in.u16();
        appendBigEndian(stream, length, 2);
        auto payload = in.bytes(std::min<size_t>(length, 256));
        payload.resize(length);
        stream.insert(stream.end(), payload.begin(), payload.end());
        return true;
    }
    default:
        return false;
    }
}

// The forwarder's side: an entry's reader (a client's frames are masked).
void entry(forwarder::EntryKind kind, bool opening, Steps& in) {
    forwarder::EntryReader reader(kind);
    Bytes stream;
    if (opening) {
        const auto request = ws::open("forwarder.example", "/rcs").request;
        stream.assign(request.begin(), request.end());
    }
    bool open = true;
    const auto read = [&](size_t size) {
        size = std::min(size, stream.size());
        if (!size) {
            return;
        }
        std::vector<Bytes> datagrams;
        Bytes outbound;
        const bool was = open;
        open = reader.take(std::span<const uint8_t>(stream).first(size), datagrams, outbound);
        check(was || (!open && datagrams.empty() && outbound.empty()));
        for (const auto& datagram : datagrams) {
            check(!datagram.empty() && datagram.size() <= ws::maxMessage);
        }
        check(outbound.size() <= size + 512 && (!reader.refused() || !open));
        stream.erase(stream.begin(), stream.begin() + ptrdiff_t(size));
    };
    while (in.more() && open) {
        if (const uint8_t step = in.byte(); !append(in, step, true, stream)) {
            read(size_t(in.u16()) + 1);
        }
    }
    while (open && !stream.empty()) {
        read(stream.size());
    }
}

// An end's side: the server's answer to its opening, then the server's frames
// (or lengths), as a framed socket reads them (a server's are not masked).
void end(bool webSocket, bool answered, Steps& in) {
    const auto opening = ws::open("forwarder.example", "/rcs");
    Bytes stream;
    if (answered) { // The answer a server gives this very request.
        const auto accepted = ws::accept(
            std::span(reinterpret_cast<const uint8_t*>(opening.request.data()), opening.request.size()));
        check(accepted.has_value());
        stream.assign(accepted->response.begin(), accepted->response.end());
    }
    auto framing = webSocket ? transport::webSocketFraming() : transport::lengthFraming();
    bool upgraded = !webSocket;
    Bytes head; // The answer as far as it came.
    const auto read = [&](size_t size) {
        size = std::min(size, stream.size());
        std::span<const uint8_t> bytes = std::span<const uint8_t>(stream).first(size);
        Bytes rest;
        if (!upgraded) {
            head.insert(head.end(), bytes.begin(), bytes.end());
            const auto consumed = ws::opened(head, opening.key);
            if (!consumed) {
                stream.erase(stream.begin(), stream.begin() + ptrdiff_t(size));
                return;
            }
            check(*consumed <= head.size());
            rest.assign(head.begin() + ptrdiff_t(*consumed), head.end());
            bytes = rest;
            upgraded = true;
        }
        framing->push(bytes);
        Bytes answer;
        while (auto datagram = framing->pop(answer)) {
            check(!datagram->empty() && datagram->size() <= ws::maxMessage);
        }
        check(answer.size() <= bytes.size() + 512);
        stream.erase(stream.begin(), stream.begin() + ptrdiff_t(size));
    };
    while (in.more()) {
        if (const uint8_t step = in.byte(); !append(in, step, false, stream)) {
            read(size_t(in.u16()) + 1);
        }
    }
    read(stream.size());
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    Steps in(data, size);
    const uint8_t mode = in.byte();
    try {
        switch (mode % 4) {
        case 0: entry(forwarder::EntryKind::tcp, false, in); break;
        case 1: entry(forwarder::EntryKind::websocket, mode & 4, in); break;
        case 2: end(true, mode & 4, in); break;
        default: end(false, false, in); break;
        }
    } catch (const Failure&) {
        // The connection closes, as it would.
    }
    return 0;
}
