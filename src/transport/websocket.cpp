#include "transport/websocket.hpp"

#include "crypto/crypto.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <string_view>

namespace rcsdk::transport::websocket {
namespace {

constexpr std::string_view guid = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
constexpr std::string_view endOfHead = "\r\n\r\n";

std::string base64(std::span<const uint8_t> bytes) {
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string text;
    for (size_t i = 0; i < bytes.size(); i += 3) {
        const uint32_t chunk = uint32_t(bytes[i]) << 16 | (i + 1 < bytes.size() ? uint32_t(bytes[i + 1]) << 8 : 0) |
                               (i + 2 < bytes.size() ? bytes[i + 2] : 0);
        text += alphabet[chunk >> 18 & 63];
        text += alphabet[chunk >> 12 & 63];
        text += i + 1 < bytes.size() ? alphabet[chunk >> 6 & 63] : '=';
        text += i + 2 < bytes.size() ? alphabet[chunk & 63] : '=';
    }
    return text;
}

// The key's answer: base64(SHA-1(key || GUID)).
std::string acceptOf(std::string_view key) {
    std::string joined(key);
    joined += guid;
    return base64(crypto::sha1(std::span(reinterpret_cast<const uint8_t*>(joined.data()), joined.size())));
}

// 16 bytes in base64: 22 characters of its alphabet, then "==".
bool validKey(std::string_view key) {
    return key.size() == 24 && key.substr(22) == "==" && std::all_of(key.begin(), key.begin() + 22, [](char c) {
               return std::isalnum(static_cast<unsigned char>(c)) || c == '+' || c == '/';
           });
}

std::string lower(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
        text.remove_suffix(1);
    }
    return text;
}

// Whether a comma-separated header value holds `token` (case aside).
bool hasToken(std::string_view value, std::string_view token) {
    while (!value.empty()) {
        const auto comma = value.find(',');
        if (lower(trim(value.substr(0, comma))) == token) {
            return true;
        }
        value = comma == std::string_view::npos ? std::string_view() : value.substr(comma + 1);
    }
    return false;
}

// An opening's head: its first line and its header fields (names in lower
// case), or why it is none; none while it has not all arrived.
struct Head {
    size_t consumed = 0;
    std::string first;
    std::vector<std::pair<std::string, std::string>> fields;
    std::optional<std::string> field(std::string_view name) const {
        for (const auto& [n, value] : fields) {
            if (n == name) {
                return value;
            }
        }
        return std::nullopt;
    }
};
enum class HeadProblem { none, tooLarge, malformed };
std::optional<Head> headOf(std::span<const uint8_t> bytes, HeadProblem& problem) {
    problem = HeadProblem::none;
    const std::string_view text(reinterpret_cast<const char*>(bytes.data()), std::min(bytes.size(), maxHandshake));
    const auto end = text.find(endOfHead);
    if (end == std::string_view::npos) {
        if (bytes.size() >= maxHandshake) {
            problem = HeadProblem::tooLarge;
        }
        return std::nullopt;
    }
    Head head;
    head.consumed = end + endOfHead.size();
    std::string_view rest = text.substr(0, end + 2);
    const auto line = [&] {
        const auto at = rest.find("\r\n");
        const auto taken = rest.substr(0, at);
        rest.remove_prefix(at + 2);
        return taken;
    };
    head.first = std::string(line());
    while (!rest.empty()) {
        const auto field = line();
        const auto colon = field.find(':');
        if (head.fields.size() == maxHeaders) {
            problem = HeadProblem::tooLarge;
            return std::nullopt;
        }
        // No folded lines, no empty or spaced names, no control characters
        // (bytes from 0x80 are text: a value may carry them, e.g. a cookie in UTF-8).
        if (colon == std::string_view::npos || !colon || field.front() == ' ' || field.front() == '\t' ||
            field.substr(0, colon).find_first_of(" \t") != std::string_view::npos ||
            std::any_of(field.begin(), field.end(), [](char c) {
                const auto b = static_cast<unsigned char>(c);
                return (b < ' ' && b != '\t') || b == 127;
            })) {
            problem = HeadProblem::malformed;
            return std::nullopt;
        }
        head.fields.emplace_back(lower(field.substr(0, colon)), std::string(trim(field.substr(colon + 1))));
    }
    return head;
}

std::string answer(std::string_view status, std::string_view extra = {}) {
    return "HTTP/1.1 " + std::string(status) + "\r\n" + std::string(extra) +
           "Content-Length: 0\r\nConnection: close\r\n\r\n";
}

[[noreturn]] void refuse(std::string_view status, const std::string& why, std::string_view extra = {}) {
    throw Refused(answer(status, extra), why);
}

} // namespace

// ---- The opening handshake ---------------------------------------------------------------

std::optional<Accepted> accept(std::span<const uint8_t> bytes) {
    HeadProblem problem;
    const auto head = headOf(bytes, problem);
    if (problem == HeadProblem::tooLarge) {
        refuse("431 Request Header Fields Too Large", "The opening request is larger than " +
                                                          std::to_string(maxHandshake) + " bytes or has more than " +
                                                          std::to_string(maxHeaders) + " header fields");
    }
    if (problem == HeadProblem::malformed) {
        refuse("400 Bad Request", "The opening request has a malformed header field");
    }
    if (!head) {
        return std::nullopt;
    }
    // "GET /path HTTP/1.1": origin form, HTTP/1.1 (RFC 6455 section 4.1).
    const std::string_view first(head->first);
    const auto space = first.find(' '), last = first.rfind(' ');
    if (space == std::string_view::npos || last == space) {
        refuse("400 Bad Request", "The opening request's first line is malformed");
    }
    const auto method = first.substr(0, space), target = first.substr(space + 1, last - space - 1),
               version = first.substr(last + 1);
    if (version != "HTTP/1.1") {
        refuse("400 Bad Request", "The opening request is not HTTP/1.1");
    }
    if (method != "GET") {
        refuse("405 Method Not Allowed", "The opening request is no GET", "Allow: GET\r\n");
    }
    if (target.empty() || target.front() != '/' || target.find_first_of(" \t") != std::string_view::npos) {
        refuse("400 Bad Request", "The opening request's target is not a path");
    }
    if (!head->field("host")) {
        refuse("400 Bad Request", "The opening request has no Host");
    }
    if (!hasToken(head->field("upgrade").value_or(""), "websocket") ||
        !hasToken(head->field("connection").value_or(""), "upgrade")) {
        refuse("426 Upgrade Required", "The request asks for no WebSocket",
               "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n");
    }
    if (head->field("sec-websocket-version") != "13") {
        refuse("426 Upgrade Required", "The opening asks for another WebSocket version than 13",
               "Upgrade: websocket\r\nSec-WebSocket-Version: 13\r\n");
    }
    const auto key = head->field("sec-websocket-key");
    if (!key || !validKey(*key)) {
        refuse("400 Bad Request", "The opening request's Sec-WebSocket-Key is no 16 bytes in base64");
    }
    Accepted accepted;
    accepted.consumed = head->consumed;
    accepted.path = std::string(target);
    accepted.response = "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                        "Sec-WebSocket-Accept: " +
                        acceptOf(*key) + "\r\n\r\n";
    return accepted;
}

Url parseUrl(std::string_view text) {
    const auto bad = [&](const std::string& why) -> Url {
        fail(RCS_ERROR_INVALID_ARGUMENT, "\"" + std::string(text) + "\" is no WebSocket URL: " + why);
    };
    if (text.size() > 255) {
        return bad("longer than 255 bytes");
    }
    if (std::any_of(text.begin(), text.end(), [](char c) { return c <= ' ' || c == 127 || c == '#'; })) {
        return bad("it has a space, a control character or a fragment");
    }
    Url url;
    std::string_view rest;
    if (text.substr(0, 5) == "ws://") {
        rest = text.substr(5);
    } else if (text.substr(0, 6) == "wss://") {
        url.tls = true;
        rest = text.substr(6);
    } else {
        return bad("it starts with neither ws:// nor wss://");
    }
    const auto slash = rest.find_first_of("/?");
    auto authority = rest.substr(0, slash);
    url.target = slash == std::string_view::npos ? "/" : std::string(rest.substr(slash));
    if (url.target.front() == '?') {
        url.target.insert(url.target.begin(), '/');
    }
    if (authority.find('@') != std::string_view::npos) {
        return bad("it names a user");
    }
    url.hostHeader = std::string(authority);
    std::string_view port;
    if (!authority.empty() && authority.front() == '[') {
        const auto close = authority.find(']');
        if (close == std::string_view::npos) {
            return bad("its IPv6 address has no ']'");
        }
        url.host = std::string(authority.substr(1, close - 1));
        const auto after = authority.substr(close + 1);
        if (!after.empty() && after.front() != ':') {
            return bad("something follows its IPv6 address");
        }
        port = after.empty() ? after : after.substr(1);
        if (url.host.empty()) {
            return bad("it names no host");
        }
    } else {
        const auto colon = authority.find(':');
        url.host = std::string(authority.substr(0, colon));
        port = colon == std::string_view::npos ? std::string_view() : authority.substr(colon + 1);
    }
    if (url.host.empty()) {
        return bad("it names no host");
    }
    url.port = url.tls ? 443 : 80;
    if (!port.empty() || authority.back() == ':') {
        if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string_view::npos ||
            std::stoul(std::string(port)) == 0 || std::stoul(std::string(port)) > 65535) {
            return bad("its port is not 1..65535");
        }
        url.port = uint16_t(std::stoul(std::string(port)));
    }
    return url;
}

Opening open(const std::string& host, const std::string& path) {
    std::array<uint8_t, 16> nonce;
    crypto::random(nonce);
    Opening opening;
    opening.key = base64(nonce);
    opening.request = "GET " + path + " HTTP/1.1\r\nHost: " + host +
                      "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Key: " + opening.key +
                      "\r\nSec-WebSocket-Version: 13\r\n\r\n";
    return opening;
}

std::optional<size_t> opened(std::span<const uint8_t> bytes, const std::string& key) {
    HeadProblem problem;
    const auto head = headOf(bytes, problem);
    if (problem != HeadProblem::none) {
        fail(RCS_ERROR_NETWORK,
             "The WebSocket server's answer is malformed or larger than " + std::to_string(maxHandshake) + " bytes");
    }
    if (!head) {
        return std::nullopt;
    }
    const auto status = head->first.substr(0, std::min<size_t>(head->first.size(), 80));
    if (head->first.rfind("HTTP/1.1 101", 0) != 0) {
        fail(RCS_ERROR_NETWORK, "The WebSocket server answered \"" + status + "\"");
    }
    if (!hasToken(head->field("upgrade").value_or(""), "websocket") ||
        !hasToken(head->field("connection").value_or(""), "upgrade") ||
        head->field("sec-websocket-accept") != acceptOf(key)) {
        fail(RCS_ERROR_NETWORK, "The WebSocket server's answer does not accept this end's opening");
    }
    if (head->field("sec-websocket-extensions") || head->field("sec-websocket-protocol")) {
        fail(RCS_ERROR_NETWORK, "The WebSocket server agreed to an extension or subprotocol this end did not ask for");
    }
    return head->consumed;
}

// ---- Frames ------------------------------------------------------------------------------

Bytes frame(Opcode opcode, std::span<const uint8_t> payload, bool mask) {
    Bytes out;
    frame(out, opcode, payload, mask);
    return out;
}

void frame(Bytes& out, Opcode opcode, std::span<const uint8_t> payload, bool mask) {
    out.push_back(uint8_t(0x80 | opcode));
    const uint8_t maskBit = mask ? 0x80 : 0;
    if (payload.size() < 126) {
        out.push_back(uint8_t(maskBit | payload.size()));
    } else if (payload.size() < 65536) {
        out.push_back(uint8_t(maskBit | 126));
        appendBigEndian(out, payload.size(), 2);
    } else {
        out.push_back(uint8_t(maskBit | 127));
        appendBigEndian(out, payload.size(), 8);
    }
    const size_t start = out.size() + (mask ? 4 : 0);
    std::array<uint8_t, 4> key{};
    if (mask) {
        crypto::random(key);
        out.insert(out.end(), key.begin(), key.end());
    }
    out.insert(out.end(), payload.begin(), payload.end());
    for (size_t i = 0; mask && i < payload.size(); ++i) {
        out[start + i] ^= key[i % 4];
    }
}

Bytes closeFrame(CloseCode code) {
    const uint8_t payload[2] = {uint8_t(code >> 8), uint8_t(code)};
    return frame(close, payload, false);
}

void Decoder::push(std::span<const uint8_t> bytes) {
    if (begin_) {
        buffer_.erase(buffer_.begin(), buffer_.begin() + ptrdiff_t(begin_));
        begin_ = 0;
    }
    buffer_.insert(buffer_.end(), bytes.begin(), bytes.end());
}

std::optional<Decoder::Event> Decoder::next() {
    while (!closed_) {
        const std::span<const uint8_t> waiting = std::span<const uint8_t>(buffer_).subspan(begin_);
        if (waiting.size() < 2) {
            return std::nullopt;
        }
        const bool fin = waiting[0] & 0x80, masked = waiting[1] & 0x80;
        const auto opcode = Opcode(waiting[0] & 0x0F);
        if (waiting[0] & 0x70) {
            throw Violation(protocolError, "A WebSocket frame sets reserved bits (no extension was agreed)");
        }
        if (opcode != continuation && opcode != text && opcode != binary && opcode != close && opcode != ping &&
            opcode != pong) {
            throw Violation(protocolError, "A WebSocket frame of unknown opcode " + std::to_string(opcode));
        }
        if (masked != masked_) {
            throw Violation(protocolError, masked_ ? "A client's WebSocket frame is not masked"
                                                   : "A server's WebSocket frame is masked");
        }
        size_t header = 2;
        uint64_t length = waiting[1] & 0x7F;
        if (length == 126) {
            if (waiting.size() < 4) {
                return std::nullopt;
            }
            length = loadBigEndian(waiting, 2, 2);
            header = 4;
            if (length < 126) {
                throw Violation(protocolError, "A WebSocket frame's length is not in its shortest form");
            }
        } else if (length == 127) {
            if (waiting.size() < 10) {
                return std::nullopt;
            }
            length = loadBigEndian(waiting, 2, 8);
            header = 10;
            if (length >> 63 || length < 65536) {
                throw Violation(protocolError, "A WebSocket frame's length is not in its shortest form");
            }
        }
        const bool control = opcode >= close;
        if (control && (!fin || length > maxControl)) {
            throw Violation(protocolError, "A WebSocket control frame is split or longer than 125 bytes");
        }
        if (!control) {
            if (opcode == continuation ? !partial_ : bool(partial_)) {
                throw Violation(protocolError, partial_ ? "A WebSocket message starts inside another"
                                                        : "A WebSocket continuation continues nothing");
            }
            if (length + (partial_ ? partial_->size() : 0) > maxMessage) {
                throw Violation(tooBig, "A WebSocket message is longer than " + std::to_string(maxMessage) + " bytes");
            }
        }
        if (opcode == text) {
            throw Violation(unsupportedData, "A WebSocket text message (this end takes binary ones)");
        }
        header += masked ? 4 : 0;
        if (waiting.size() < header + length) {
            return std::nullopt;
        }
        Bytes payload(waiting.begin() + ptrdiff_t(header), waiting.begin() + ptrdiff_t(header + length));
        for (size_t i = 0; masked && i < payload.size(); ++i) {
            payload[i] ^= waiting[header - 4 + i % 4];
        }
        begin_ += header + size_t(length);
        switch (opcode) {
        case ping:
            return Event{ping, std::move(payload)};
        case pong:
            continue;
        case close:
            if (payload.size() == 1) {
                throw Violation(protocolError, "A WebSocket close frame of one byte");
            }
            closed_ = true;
            return Event{close, std::move(payload)};
        default: // binary, continuation
            if (!partial_) {
                partial_.emplace();
            }
            partial_->insert(partial_->end(), payload.begin(), payload.end());
            if (!fin) {
                continue;
            }
            auto message = std::move(*partial_);
            partial_.reset();
            if (message.empty()) {
                continue; // No datagram.
            }
            return Event{binary, std::move(message)};
        }
    }
    return std::nullopt;
}

} // namespace rcsdk::transport::websocket
