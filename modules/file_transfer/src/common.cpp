#include "common.hpp"

namespace rcsdk::file_transfer {

std::string hex(const Token& token) {
    static const char digits[] = "0123456789abcdef";
    std::string out;
    for (const uint8_t byte : token) {
        out.push_back(digits[byte >> 4]);
        out.push_back(digits[byte & 15]);
    }
    return out;
}

Writer& Writer::text(std::string_view value, size_t most) {
    size_t size = std::min(value.size(), most);
    while (size > 0 && size < value.size() && (uint8_t(value[size]) & 0xC0) == 0x80) {
        --size;
    }
    u16(uint16_t(size));
    bytes_.insert(bytes_.end(), value.begin(), value.begin() + ptrdiff_t(size));
    return *this;
}

std::span<const uint8_t> Reader::take(size_t size) {
    if (data_.size() - at_ < size) {
        invalid("it ends early");
    }
    const auto part = data_.subspan(at_, size);
    at_ += size;
    return part;
}

void Reader::invalid(const std::string& why) const {
    fail(RCS_ERROR_PROTOCOL, std::string("Invalid file transfer message (") + what_ + "): " + why);
}

bool Reader::flag() {
    const uint8_t value = u8();
    if (value > 1) {
        invalid("a flag is neither 0 nor 1");
    }
    return value == 1;
}

std::string Reader::text(size_t most) {
    const size_t size = u16();
    if (size > most) {
        invalid("its text is too long");
    }
    const auto bytes = take(size);
    std::string value(bytes.begin(), bytes.end());
    if (!utf8(value)) {
        invalid("its text is not UTF-8");
    }
    return value;
}

rcs_result Reader::result(bool orOk) {
    const rcs_result value = i32();
    if ((value == RCS_OK && !orOk) || value > RCS_OK || value < RCS_ERROR_REFUSED) {
        invalid("a result no version defines");
    }
    return value;
}

Token Reader::token() {
    Token value{};
    const auto bytes = take(value.size());
    std::copy(bytes.begin(), bytes.end(), value.begin());
    return value;
}

void Reader::end() {
    if (at_ != data_.size()) {
        invalid("trailing bytes");
    }
}

bool utf8(std::string_view text) noexcept {
    size_t i = 0;
    while (i < text.size()) {
        const auto c = uint8_t(text[i]);
        size_t more = 0;
        uint32_t code = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            more = 1;
            code = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            more = 2;
            code = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            more = 3;
            code = c & 0x07;
        } else {
            return false;
        }
        if (text.size() - i <= more) {
            return false;
        }
        for (size_t k = 1; k <= more; ++k) {
            const auto next = uint8_t(text[i + k]);
            if ((next & 0xC0) != 0x80) {
                return false;
            }
            code = (code << 6) | (next & 0x3F);
        }
        static constexpr uint32_t least[] = {0, 0x80, 0x800, 0x10000};
        if (code < least[more] || code > 0x10FFFF || (code >= 0xD800 && code <= 0xDFFF)) {
            return false;
        }
        i += more + 1;
    }
    return true;
}

} // namespace rcsdk::file_transfer
