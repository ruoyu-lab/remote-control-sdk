// WebSockets (transport/websocket.hpp) and datagrams over byte streams
// (transport/framed_socket.hpp): the opening handshake both ways, and what a
// server refuses (too large, too many fields, no GET, no upgrade, another
// version, a bad key, folded lines, no host, a target that is no path); the
// client's check of the answer; frames both ways, split, fed byte by byte,
// and every frame RFC 6455 forbids or the bounds refuse; the framings a
// forwarder's TCP and WebSocket entries are reached with, a batch of
// datagrams over them (FramedSocket::sendBatch), and the pongs of a server
// that never reads them, which stay within the socket's bound.
#include "support/check.hpp"
#include "transport/framed_socket.hpp"
#include "transport/tcp_stream.hpp"
#include "transport/websocket.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace rcsdk;
using namespace rcsdk::transport;

namespace {

std::span<const uint8_t> bytesOf(const std::string& text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

std::string request(const std::string& fields, const std::string& first = "GET /rcs HTTP/1.1") {
    return first + "\r\n" + fields + "\r\n";
}

const std::string goodFields = "Host: forwarder.example\r\nUpgrade: websocket\r\nConnection: keep-alive, Upgrade\r\n"
                               "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\nSec-WebSocket-Version: 13\r\n";

// The answer a refused request gets, and why, or "" when it was not refused.
std::string refusal(const std::string& text) {
    try {
        websocket::accept(bytesOf(text));
    } catch (const websocket::Refused& refused) {
        return refused.response().substr(0, refused.response().find("\r\n"));
    }
    return {};
}

// The violation `bytes` make a decoder of `masked` frames throw (0: none).
uint16_t violation(const Bytes& bytes, bool masked = true) {
    websocket::Decoder decoder(masked);
    decoder.push(bytes);
    try {
        while (decoder.next()) {
        }
    } catch (const websocket::Violation& v) {
        return v.code();
    }
    return 0;
}

// A frame as a client would send it, with any header the test likes.
Bytes rawFrame(uint8_t first, uint64_t length, std::span<const uint8_t> payload, bool mask, int lengthBytes = -1) {
    Bytes out{first};
    const uint8_t maskBit = mask ? 0x80 : 0;
    if (lengthBytes < 0) {
        lengthBytes = length < 126 ? 0 : length < 65536 ? 2 : 8;
    }
    if (lengthBytes == 0) {
        out.push_back(uint8_t(maskBit | length));
    } else {
        out.push_back(uint8_t(maskBit | (lengthBytes == 2 ? 126 : 127)));
        appendBigEndian(out, length, size_t(lengthBytes));
    }
    if (mask) {
        out.insert(out.end(), {1, 2, 3, 4});
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        out.push_back(uint8_t(payload[i] ^ (mask ? (i % 4) + 1 : 0)));
    }
    return out;
}

void opening() {
    // RFC 6455 section 1.3's example key and answer.
    const auto accepted = websocket::accept(bytesOf(request(goodFields) + "extra"));
    CHECK(accepted && accepted->path == "/rcs" && accepted->consumed == request(goodFields).size());
    CHECK(accepted->response.find("101 Switching Protocols") != std::string::npos);
    CHECK(accepted->response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
    // Not all there yet.
    const auto whole = request(goodFields);
    for (size_t cut : {size_t(0), size_t(10), whole.size() - 1}) {
        CHECK(!websocket::accept(bytesOf(whole.substr(0, cut))));
    }
    // Refused, with the answer that says why.
    CHECK(refusal(std::string(websocket::maxHandshake, 'a')).find("431") != std::string::npos);
    std::string many = goodFields;
    for (size_t i = 0; i < websocket::maxHeaders; ++i) {
        many += "X-" + std::to_string(i) + ": 1\r\n";
    }
    CHECK(refusal(request(many)).find("431") != std::string::npos);
    CHECK(refusal(request(goodFields, "POST /rcs HTTP/1.1")).find("405") != std::string::npos);
    CHECK(refusal(request(goodFields, "GET /rcs HTTP/1.0")).find("400") != std::string::npos);
    CHECK(refusal(request(goodFields, "GET http://elsewhere/ HTTP/1.1")).find("400") != std::string::npos);
    CHECK(refusal(request(goodFields, "GET  HTTP/1.1")).find("400") != std::string::npos);
    CHECK(refusal(request("Host: f\r\n")).find("426") != std::string::npos); // A plain GET.
    const auto replaced = [&](const std::string& from, const std::string& to) {
        std::string fields = goodFields;
        fields.replace(fields.find(from), from.size(), to);
        return request(fields);
    };
    CHECK(refusal(replaced("Version: 13", "Version: 8")).find("426") != std::string::npos);
    CHECK(refusal(replaced("dGhlIHNhbXBsZSBub25jZQ==", "short")).find("400") != std::string::npos);
    CHECK(refusal(replaced("dGhlIHNhbXBsZSBub25jZQ==", "dGhlIHNhbXBsZSBub25j*Q==")).find("400") != std::string::npos);
    CHECK(refusal(replaced("Host: forwarder.example\r\n", "")).find("400") != std::string::npos);
    CHECK(refusal(replaced("Upgrade: websocket\r\n", "Upgrade: websocket\r\n folded\r\n")).find("400") !=
          std::string::npos);
    CHECK(refusal(replaced("Upgrade: websocket", "Upgrade : websocket")).find("400") != std::string::npos);
    CHECK(refusal(replaced("Upgrade: websocket", "Upgrade: web\x01socket")).find("400") != std::string::npos);
    // A browser's opening (Chromium's fields; a cookie of the site's in UTF-8, which HTTP lets a field
    // value carry): accepted, its extensions unanswered.
    const std::string browser =
        "Host: forwarder.example\r\nConnection: Upgrade\r\nPragma: no-cache\r\n"
        "Cache-Control: no-cache\r\nUser-Agent: Mozilla/5.0 (Macintosh) Chrome/152.0.0.0\r\n"
        "Upgrade: websocket\r\nOrigin: https://app.example\r\nSec-WebSocket-Version: 13\r\n"
        "Accept-Encoding: gzip, deflate, br, zstd\r\nAccept-Language: zh-CN,zh;q=0.9\r\n"
        "Cookie: name=\xe6\xa2\xa8\xe8\x8c\xb6\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Extensions: permessage-deflate; client_max_window_bits\r\n";
    const auto fromBrowser = websocket::accept(bytesOf(request(browser)));
    CHECK(fromBrowser && fromBrowser->response.find("Extensions") == std::string::npos);

    // A client's opening, the server's answer, and the client's check of it.
    const auto opening = websocket::open("forwarder.example:8443", "/rcs");
    const auto answered = websocket::accept(bytesOf(opening.request));
    CHECK(answered && answered->path == "/rcs");
    CHECK(websocket::opened(bytesOf(answered->response + "frames"), opening.key) == answered->response.size());
    CHECK(!websocket::opened(bytesOf(answered->response.substr(0, 20)), opening.key));
    CHECK(throws<Failure>([&] { websocket::opened(bytesOf(answered->response), "another key"); }));
    CHECK(throws<Failure>(
        [&] { websocket::opened(bytesOf("HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n"), opening.key); }));
    std::string extended = answered->response;
    extended.insert(extended.size() - 2, "Sec-WebSocket-Extensions: permessage-deflate\r\n");
    CHECK(throws<Failure>([&] { websocket::opened(bytesOf(extended), opening.key); }));
    CHECK(throws<Failure>([&] { websocket::opened(bytesOf(std::string(websocket::maxHandshake, 'x')), opening.key); }));
}

void frames() {
    using websocket::Decoder;
    // Both ways, every size class, fed byte by byte and whole.
    for (const size_t size : {size_t(1), size_t(125), size_t(126), size_t(1200), size_t(65535)}) {
        Bytes payload(size);
        for (size_t i = 0; i < size; ++i) {
            payload[i] = uint8_t(i * 7);
        }
        for (const bool mask : {true, false}) {
            const auto wire = websocket::frame(websocket::binary, payload, mask);
            Decoder whole(mask);
            whole.push(wire);
            const auto event = whole.next();
            CHECK(event && event->kind == websocket::binary && event->payload == payload && !whole.next());
            Decoder slow(mask);
            std::optional<Decoder::Event> got;
            for (const uint8_t byte : wire) {
                CHECK(!got);
                slow.push(std::span(&byte, 1));
                got = slow.next();
            }
            CHECK(got && got->payload == payload);
        }
    }
    // A message in three frames, a ping between them; pongs and empty messages are skipped.
    const uint8_t a[] = {1, 2}, b[] = {3}, c[] = {4, 5, 6}, p[] = {9};
    Bytes split = rawFrame(0x02, 2, a, true); // Binary, not final.
    const auto pong = rawFrame(0x8A, 0, {}, true), ping = rawFrame(0x89, 1, p, true);
    const auto middle = rawFrame(0x00, 1, b, true), last = rawFrame(0x80, 3, c, true),
               empty = rawFrame(0x82, 0, {}, true);
    for (const auto* part : {&pong, &middle, &ping, &last, &empty}) {
        split.insert(split.end(), part->begin(), part->end());
    }
    Decoder decoder(true);
    decoder.push(split);
    auto event = decoder.next();
    CHECK(event && event->kind == websocket::ping && event->payload == Bytes{9});
    event = decoder.next();
    CHECK(event && event->kind == websocket::binary && event->payload == (Bytes{1, 2, 3, 4, 5, 6}));
    CHECK(!decoder.next());
    // A close ends it: nothing after it is read.
    Decoder closing(true);
    auto closed = rawFrame(0x88, 2, std::vector<uint8_t>{0x03, 0xE8}, true);
    const auto after = rawFrame(0x82, 1, b, true);
    closed.insert(closed.end(), after.begin(), after.end());
    closing.push(closed);
    event = closing.next();
    CHECK(event && event->kind == websocket::close && closing.closed() && !closing.next());
    CHECK(websocket::closeFrame(websocket::goingAway) == (Bytes{0x88, 2, 0x03, 0xE9}));

    // What RFC 6455 forbids, and what the bounds refuse.
    using websocket::protocolError;
    const Bytes one{1};
    CHECK(violation(rawFrame(0x82, 1, one, false)) == protocolError);                  // A client's, unmasked.
    CHECK(violation(rawFrame(0x82, 1, one, true), false) == protocolError);            // A server's, masked.
    CHECK(violation(rawFrame(0xC2, 1, one, true)) == protocolError);                   // RSV1.
    CHECK(violation(rawFrame(0x92, 1, one, true)) == protocolError);                   // RSV3.
    CHECK(violation(rawFrame(0x83, 1, one, true)) == protocolError);                   // Opcode 3.
    CHECK(violation(rawFrame(0x8B, 1, one, true)) == protocolError);                   // Opcode 11.
    CHECK(violation(rawFrame(0x81, 1, one, true)) == websocket::unsupportedData);      // Text.
    CHECK(violation(rawFrame(0x09, 1, one, true)) == protocolError);                   // A split ping.
    CHECK(violation(rawFrame(0x89, 126, Bytes(126), true)) == protocolError);          // A ping of 126 bytes.
    CHECK(violation(rawFrame(0x82, 5, Bytes(5), true, 2)) == protocolError);           // 16-bit length below 126.
    CHECK(violation(rawFrame(0x82, 300, Bytes(300), true, 8)) == protocolError);       // 64-bit length below 65536.
    CHECK(violation(rawFrame(0x82, uint64_t(1) << 63, {}, true, 8)) == protocolError); // The top bit.
    CHECK(violation(rawFrame(0x82, 65536, {}, true)) == websocket::tooBig);            // One frame too large...
    Bytes parts = rawFrame(0x02, 40000, Bytes(40000), true);
    const auto more = rawFrame(0x80, 30000, {}, true);
    parts.insert(parts.end(), more.begin(), more.end());
    CHECK(violation(parts) == websocket::tooBig);                    // ...or its frames together.
    CHECK(violation(rawFrame(0x80, 1, one, true)) == protocolError); // A continuation of nothing.
    Bytes interrupted = rawFrame(0x02, 1, one, true);
    const auto another = rawFrame(0x82, 1, one, true);
    interrupted.insert(interrupted.end(), another.begin(), another.end());
    CHECK(violation(interrupted) == protocolError);                  // A message inside another.
    CHECK(violation(rawFrame(0x88, 1, one, true)) == protocolError); // A close of one byte.
    // A header that promises more is only waited for, never read past.
    Decoder waiting(true);
    waiting.push(Bytes{0x82, 0xFE, 0xFF});
    CHECK(!waiting.next());
}

void framings() {
    Bytes answer;
    // Behind their length.
    auto length = lengthFraming();
    const Bytes datagram{1, 2, 3};
    auto wire = length->wrap(datagram);
    CHECK(wire == (Bytes{0, 3, 1, 2, 3}));
    wire.insert(wire.end(), {0, 1, 9, 0});
    length->push(wire);
    CHECK(length->pop(answer) == datagram && length->pop(answer) == Bytes{9} && !length->pop(answer));
    length->push(Bytes{2}); // The length's second half (its first came above)...
    CHECK(!length->pop(answer));
    length->push(Bytes{4}); // ...and half of its datagram.
    CHECK(!length->pop(answer));
    length->push(Bytes{5});
    CHECK(length->pop(answer) == (Bytes{4, 5}));
    auto zero = lengthFraming();
    zero->push(Bytes{0, 0});
    CHECK(throws<Failure>([&] { zero->pop(answer); }));

    // As a WebSocket client: its frames are masked; a server's ping is answered, its close ends the stream.
    auto ws = webSocketFraming();
    const auto sent = ws->wrap(datagram);
    websocket::Decoder server(true);
    server.push(sent);
    const auto got = server.next();
    CHECK(got && got->payload == datagram);
    auto incoming = websocket::frame(websocket::ping, Bytes{7}, false);
    const auto message = websocket::frame(websocket::binary, datagram, false);
    incoming.insert(incoming.end(), message.begin(), message.end());
    ws->push(incoming);
    CHECK(ws->pop(answer) == datagram && !answer.empty());
    websocket::Decoder pongs(true);
    pongs.push(answer);
    CHECK(!pongs.next()); // A pong: skipped, as it should be.
    ws->push(websocket::closeFrame(websocket::goingAway));
    CHECK(throws<Failure>([&] { ws->pop(answer); }));
    auto broken = webSocketFraming();
    broken->push(websocket::frame(websocket::binary, datagram, true)); // A server's frame, masked.
    CHECK(throws<Failure>([&] { broken->pop(answer); }));
    Bytes two = websocket::frame(websocket::binary, datagram, false); // Frames appended: a batch.
    websocket::frame(two, websocket::binary, Bytes(300, 5), false);
    websocket::Decoder client(false);
    client.push(two);
    const auto first = client.next(), second = client.next();
    CHECK(first && first->payload == datagram && second && second->payload == Bytes(300, 5) && !client.next());
}

// A batch over a framed socket goes in one write, each datagram its own
// frame, in order. While the server reads nothing, a datagram that would make
// what waits more than 64 KiB is dropped whole, and later small ones that fit
// go on; the connection goes on.
void framedBatches() {
    std::atomic<bool> never{false};
    for (const bool webSocket : {false, true}) {
        TcpListenSocket listener("127.0.0.1", 0);
        const auto server = Endpoint::parse("127.0.0.1", localAddress(listener.handle()).port());
        FramedSocket socket(tcpBytes(connectStreamTcp(server, "the test's server", never, 2s, 0), "the test's server"),
                            server, webSocket ? webSocketFraming() : lengthFraming());
        const auto peer = listener.accept(never);
        const auto datagram = [](uint32_t n) {
            Bytes d(n % 2 ? 100 : 16000, uint8_t(n));
            storeBigEndian(d, 0, n, 4);
            return d;
        };
        constexpr uint32_t sent = 63 * 64; // About 32 MB.
        for (uint32_t first = 0; first < sent; first += 64) {
            std::vector<Bytes> datagrams;
            std::vector<Outgoing> batch;
            for (uint32_t n = first; n < first + 64; ++n) {
                datagrams.push_back(datagram(n));
            }
            for (const auto& d : datagrams) {
                batch.push_back({&server, d});
            }
            socket.sendBatch(batch);
        }
        // What came, while the client writes what still waits as it receives (nothing arrives).
        Bytes wire, chunk(64 * 1024);
        for (const auto until = Clock::now() + 2s; Clock::now() < until;) {
            Endpoint from;
            Bytes none(1);
            socket.receive(none, from);
            if (const size_t taken = peer->readSome(chunk)) {
                wire.insert(wire.end(), chunk.begin(), chunk.begin() + ptrdiff_t(taken));
            } else {
                std::this_thread::sleep_for(1ms);
            }
        }
        std::vector<Bytes> got;
        if (webSocket) {
            websocket::Decoder frames(true);
            frames.push(wire);
            while (auto event = frames.next()) {
                got.push_back(std::move(event->payload));
            }
        } else {
            auto lengths = lengthFraming();
            lengths->push(wire);
            Bytes none;
            while (auto d = lengths->pop(none)) {
                got.push_back(std::move(*d));
            }
        }
        bool largeDropped = false, smallAfter = false;
        for (uint32_t expected = 0; const auto& d : got) {
            const auto n = uint32_t(loadBigEndian(d, 0, 4));
            CHECK(n >= expected && n < sent && d == datagram(n)); // In order, whole.
            largeDropped = largeDropped || n > expected + expected % 2; // Missing: an even (large) one.
            smallAfter = smallAfter || (largeDropped && n % 2);
            expected = n + 1;
        }
        CHECK(got.size() < sent && smallAfter);
        CHECK(socket.send(server, datagram(sent))); // Alone, as it goes on.
        std::printf("framed batches (%s): %zu of %u datagrams arrived while the server read nothing\n",
                    webSocket ? "WebSocket" : "TCP", got.size(), sent);
    }
}

// A server that pings and never reads (this end's datagram socket over its
// WebSocket): what waits for it stays within what a datagram socket holds.
void unreadPongs() {
    struct Pinging final : ByteStream {
        Bytes pings;
        bool sent = false;      // Pings once per read of the socket's, then nothing until the next.
        size_t mostOffered = 0; // The most that waited for the server.
        size_t writeSome(std::span<const uint8_t> bytes) override {
            mostOffered = std::max(mostOffered, bytes.size());
            return 0; // It reads nothing.
        }
        size_t readSome(std::span<uint8_t> buffer) override {
            if (std::exchange(sent, !sent)) {
                return 0;
            }
            const size_t taken = std::min(buffer.size(), pings.size());
            std::copy_n(pings.begin(), taken, buffer.begin());
            return taken;
        }
        Handle handle() const noexcept override { return invalidHandle; }
        Endpoint local() const override { return Endpoint::parse("127.0.0.1", 1); }
        void close(std::span<const uint8_t>, Clock::time_point) override {}
    };
    auto stream = std::make_unique<Pinging>();
    const auto ping = websocket::frame(websocket::ping, Bytes(125, 7), false);
    for (int i = 0; i < 50; ++i) { // A read's worth.
        stream->pings.insert(stream->pings.end(), ping.begin(), ping.end());
    }
    auto& pinging = *stream;
    const auto server = Endpoint::parse("127.0.0.1", 2);
    FramedSocket socket(std::move(stream), server, webSocketFraming());
    Bytes buffer(2048);
    Endpoint from;
    for (int read = 0; read < 100; ++read) { // 5000 pings: their pongs are 655 KB.
        CHECK(!socket.receive(buffer, from));
    }
    CHECK(pinging.mostOffered && pinging.mostOffered <= 64 * 1024);
    std::printf("unread pongs: at most %zu bytes waited for the server after 5000 pings\n", pinging.mostOffered);
}

} // namespace

int main() {
    return runTest("websocket", [] {
        opening();
        frames();
        framings();
        unreadPongs();
        framedBatches();
    });
}
