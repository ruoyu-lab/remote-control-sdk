// Big-endian field coding shared by the wire formats: a message is a type
// byte and its fields. The reader checks every read against the end of the
// message, so a truncated or padded message cannot be misread.
#pragma once

#include "crypto/crypto.hpp"
#include "util/api.hpp"
#include "util/bytes.hpp"
#include "util/util.hpp"

#include <span>
#include <string>

namespace rcsdk::protocol {

class Writer {
public:
    explicit Writer(uint8_t type) { bytes_.push_back(type); }
    Writer& u8(uint8_t value) {
        bytes_.push_back(value);
        return *this;
    }
    Writer& u16(uint16_t value) { return put(value, 2); }
    Writer& u32(uint32_t value) { return put(value, 4); }
    Writer& u64(uint64_t value) { return put(value, 8); }
    Writer& raw(std::span<const uint8_t> data) {
        bytes_.insert(bytes_.end(), data.begin(), data.end());
        return *this;
    }
    crypto::Bytes take() { return std::move(bytes_); }

private:
    Writer& put(uint64_t value, size_t width) {
        appendBigEndian(bytes_, value, width);
        return *this;
    }
    crypto::Bytes bytes_;
};

class Reader {
public:
    explicit Reader(std::span<const uint8_t> bytes) : bytes_(bytes) {}
    uint8_t u8() { return uint8_t(get(1)); }
    uint16_t u16() { return uint16_t(get(2)); }
    uint32_t u32() { return uint32_t(get(4)); }
    uint64_t u64() { return get(8); }
    std::span<const uint8_t> raw(size_t size) {
        need(size);
        const auto result = bytes_.subspan(offset_, size);
        offset_ += size;
        return result;
    }
    size_t remaining() const noexcept { return bytes_.size() - offset_; }
    void end() const {
        if (offset_ != bytes_.size()) {
            invalid("trailing bytes");
        }
    }
    [[noreturn]] static void invalid(const char* what) {
        fail(RCS_ERROR_PROTOCOL, std::string("Invalid message: ") + what);
    }

private:
    void need(size_t size) const {
        if (size > bytes_.size() - offset_) {
            invalid("truncated");
        }
    }
    uint64_t get(size_t width) {
        need(width);
        const uint64_t value = loadBigEndian(bytes_, offset_, width);
        offset_ += width;
        return value;
    }
    std::span<const uint8_t> bytes_;
    size_t offset_ = 0;
};

} // namespace rcsdk::protocol
