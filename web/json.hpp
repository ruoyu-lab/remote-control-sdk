// The browser package's glue: JSON as rcsdk.mjs writes it (JSON.stringify of
// a session's options, a file offer), read into a tree, and written for the
// page's events. Malformed text reads as null, which the glue refuses, and so
// does text that nests arrays and objects deeper than maxDepth (the options
// are 3 deep): no text runs the parser out of stack.
#pragma once

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <utility>
#include <vector>

namespace web {

class Json {
public:
    enum Kind { Null, Bool, Number, String, Array, Object };
    static constexpr int maxDepth = 32;

    static Json parse(const char* text) {
        const char* p = text;
        Json value;
        if (!read(p, value, maxDepth)) {
            return {};
        }
        skip(p);
        return *p ? Json{} : value;
    }

    Kind kind() const { return kind_; }
    bool has(const char* name) const { return (*this)[name].kind_ != Null; }
    // A field, or null when this is not an object or has no such field.
    const Json& operator[](const char* name) const {
        static const Json none;
        for (const auto& [key, value] : fields_) {
            if (key == name) {
                return value;
            }
        }
        return none;
    }
    const std::vector<Json>& items() const { return items_; }
    const std::string& text() const { return text_; }
    double number(double fallback = 0) const { return kind_ == Number ? number_ : kind_ == Bool ? bool_ : fallback; }
    // The number as an integer type: one below its range is its least, one
    // above its most (which the SDK refuses where that is no valid value).
    uint32_t u32(uint32_t fallback = 0) const {
        return kind_ == Number || kind_ == Bool ? to<uint32_t>(number()) : fallback;
    }
    uint64_t u64(uint64_t fallback = 0) const {
        return kind_ == Number || kind_ == Bool ? to<uint64_t>(number()) : fallback;
    }
    template <class T>
    static T to(double value) {
        using Limits = std::numeric_limits<T>;
        // T's most as a double rounds up to 2^digits, which T does not hold; NaN is the least.
        return !(value > double(Limits::lowest()))      ? Limits::lowest()
               : value >= std::ldexp(1.0, Limits::digits) ? Limits::max()
                                                          : T(value);
    }
    bool boolean(bool fallback = false) const { return kind_ == Bool ? bool_ : kind_ == Number ? number_ != 0 : fallback; }

private:
    static void skip(const char*& p) {
        while (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t') {
            ++p;
        }
    }

    static bool word(const char*& p, const char* w) {
        size_t n = 0;
        while (w[n]) {
            if (p[n] != w[n]) {
                return false;
            }
            ++n;
        }
        p += n;
        return true;
    }

    static void utf8(std::string& out, uint32_t c) {
        if (c < 0x80) {
            out += char(c);
        } else if (c < 0x800) {
            out += char(0xC0 | (c >> 6));
            out += char(0x80 | (c & 0x3F));
        } else if (c < 0x10000) {
            out += char(0xE0 | (c >> 12));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        } else {
            out += char(0xF0 | (c >> 18));
            out += char(0x80 | ((c >> 12) & 0x3F));
            out += char(0x80 | ((c >> 6) & 0x3F));
            out += char(0x80 | (c & 0x3F));
        }
    }

    static bool string(const char*& p, std::string& out) {
        if (*p++ != '"') {
            return false;
        }
        while (*p && *p != '"') {
            if (*p != '\\') {
                out += *p++;
                continue;
            }
            ++p;
            const char c = *p++;
            switch (c) {
            case 'n': out += '\n'; break;
            case 't': out += '\t'; break;
            case 'r': out += '\r'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'u': {
                auto hex4 = [&](uint32_t& v) {
                    v = 0;
                    for (int i = 0; i < 4; ++i, ++p) {
                        const char d = *p;
                        const int digit = d >= '0' && d <= '9'   ? d - '0'
                                          : d >= 'a' && d <= 'f' ? d - 'a' + 10
                                          : d >= 'A' && d <= 'F' ? d - 'A' + 10
                                                                 : -1;
                        if (digit < 0) {
                            return false;
                        }
                        v = v << 4 | uint32_t(digit);
                    }
                    return true;
                };
                uint32_t code = 0;
                if (!hex4(code)) {
                    return false;
                }
                if (code >= 0xD800 && code < 0xE000) { // A surrogate pair, or text UTF-8 cannot carry.
                    uint32_t low = 0;
                    if (code >= 0xDC00 || p[0] != '\\' || p[1] != 'u' || (p += 2, !hex4(low)) || low < 0xDC00 ||
                        low > 0xDFFF) {
                        return false;
                    }
                    code = 0x10000 + ((code - 0xD800) << 10) + (low - 0xDC00);
                }
                utf8(out, code);
                break;
            }
            case 0: return false;
            default: out += c; break; // \" \\ \/
            }
        }
        if (*p != '"') {
            return false;
        }
        ++p;
        return true;
    }

    static bool digits(const char*& p) {
        const char* start = p;
        while (*p >= '0' && *p <= '9') {
            ++p;
        }
        return p != start;
    }

    // A number as JSON writes it (strtod would also take "inf", "nan" and
    // hexadecimal), within a double's range.
    static bool number(const char*& p, double& value) {
        const char* start = p;
        p += *p == '-';
        if (!digits(p) || (*p == '.' && (++p, !digits(p)))) {
            return false;
        }
        if (*p == 'e' || *p == 'E') {
            ++p;
            p += *p == '+' || *p == '-';
            if (!digits(p)) {
                return false;
            }
        }
        value = std::strtod(start, nullptr);
        return std::isfinite(value);
    }

    // One value at p, arrays and objects in it at most `depth` deep; false for
    // text that is not one.
    static bool read(const char*& p, Json& value, int depth) {
        skip(p);
        if ((*p == '{' || *p == '[') && depth-- == 0) {
            return false;
        }
        if (*p == '{') {
            ++p;
            value.kind_ = Object;
            skip(p);
            if (*p == '}') {
                ++p;
                return true;
            }
            for (;;) {
                skip(p);
                std::string key;
                Json field;
                if (!string(p, key)) {
                    return false;
                }
                skip(p);
                if (*p++ != ':' || !read(p, field, depth)) {
                    return false;
                }
                value.fields_.emplace_back(std::move(key), std::move(field));
                skip(p);
                if (*p == ',') {
                    ++p;
                } else if (*p == '}') {
                    ++p;
                    return true;
                } else {
                    return false;
                }
            }
        }
        if (*p == '[') {
            ++p;
            value.kind_ = Array;
            skip(p);
            if (*p == ']') {
                ++p;
                return true;
            }
            for (;;) {
                Json item;
                if (!read(p, item, depth)) {
                    return false;
                }
                value.items_.push_back(std::move(item));
                skip(p);
                if (*p == ',') {
                    ++p;
                } else if (*p == ']') {
                    ++p;
                    return true;
                } else {
                    return false;
                }
            }
        }
        if (*p == '"') {
            value.kind_ = String;
            return string(p, value.text_);
        }
        if (word(p, "true") || word(p, "false")) {
            value.kind_ = Bool;
            value.bool_ = p[-1] == 'e' && p[-2] == 'u';
            value.number_ = value.bool_;
            return true;
        }
        if (word(p, "null")) {
            return true;
        }
        value.kind_ = Number;
        return number(p, value.number_);
    }

    Kind kind_ = Null;
    bool bool_ = false;
    double number_ = 0;
    std::string text_;
    std::vector<Json> items_;
    std::vector<std::pair<std::string, Json>> fields_;
};

inline std::string hex(const uint8_t* bytes, size_t size) {
    static const char digits[] = "0123456789abcdef";
    std::string text;
    for (size_t i = 0; i < size; ++i) {
        text += digits[bytes[i] >> 4];
        text += digits[bytes[i] & 15];
    }
    return text;
}

// An event or a result for the page: {"name":value,...} built field by field.
class JsonWriter {
public:
    JsonWriter& add(const char* name, int64_t value) { return raw(name, std::to_string(value)); }
    JsonWriter& add(const char* name, uint64_t value) { return raw(name, std::to_string(value)); }
    JsonWriter& add(const char* name, uint32_t value) { return raw(name, std::to_string(value)); }
    JsonWriter& add(const char* name, int32_t value) { return raw(name, std::to_string(value)); }
    JsonWriter& add(const char* name, const std::string& value) { return raw(name, quoted(value)); }
    JsonWriter& add(const char* name, const char* value) { return raw(name, quoted(value ? value : "")); }
    JsonWriter& hex(const char* name, const uint8_t* bytes, size_t size) { return add(name, web::hex(bytes, size)); }
    // A value already written as JSON (an object, an array).
    JsonWriter& raw(const char* name, const std::string& json) {
        text_ += text_.size() > 1 ? "," : "";
        text_ += quoted(name) + ":" + json;
        return *this;
    }
    std::string str() const { return text_ + "}"; }

    static std::string quoted(const std::string& text) {
        std::string out = "\"";
        for (const char c : text) {
            if (c == '"' || c == '\\') {
                out += '\\';
                out += c;
            } else if (uint8_t(c) < 0x20) {
                char code[8];
                std::snprintf(code, sizeof(code), "\\u%04x", unsigned(uint8_t(c)));
                out += code;
            } else {
                out += c;
            }
        }
        return out + "\"";
    }

private:
    std::string text_ = "{";
};

} // namespace web
