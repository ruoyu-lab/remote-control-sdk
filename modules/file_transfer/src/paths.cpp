#include "paths.hpp"

namespace rcsdk::file_transfer {
namespace {

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? char(c - 'A' + 'a') : c;
}

bool equalNoCase(std::string_view a, std::string_view b) noexcept {
    return a.size() == b.size() &&
           std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return lower(x) == lower(y); });
}

// Windows' reserved device names: the part before the first dot, its
// trailing spaces left out, in any case; COM and LPT with a digit 0..9 or
// a superscript 1, 2 or 3.
bool reserved(std::string_view component) noexcept {
    std::string_view base = component.substr(0, component.find('.'));
    while (!base.empty() && base.back() == ' ') {
        base.remove_suffix(1);
    }
    for (const char* name : {"con", "prn", "aux", "nul", "conin$", "conout$"}) {
        if (equalNoCase(base, name)) {
            return true;
        }
    }
    if (base.size() < 4 || !(equalNoCase(base.substr(0, 3), "com") || equalNoCase(base.substr(0, 3), "lpt"))) {
        return false;
    }
    const std::string_view number = base.substr(3);
    return (number.size() == 1 && number[0] >= '0' && number[0] <= '9') || number == "\xC2\xB9" ||
           number == "\xC2\xB2" || number == "\xC2\xB3";
}

std::optional<std::string> componentProblem(std::string_view c) {
    const std::string quoted = "\"" + std::string(c) + "\"";
    if (c.empty()) {
        return std::string("an empty component (a leading, trailing or double '/')");
    }
    if (c.size() > maxComponent) {
        return "a component longer than 255 bytes";
    }
    if (c == "." || c == "..") {
        return quoted + " (\".\" and \"..\" are not taken)";
    }
    for (const char ch : c) {
        if (uint8_t(ch) < 0x20 || ch == 0x7F) {
            return quoted + " holds a control character";
        }
        if (std::string_view("\\:*?\"<>|").find(ch) != std::string_view::npos) {
            return quoted + " holds one of \\ : * ? \" < > | (Windows takes none of them)";
        }
    }
    if (c.back() == '.' || c.back() == ' ') {
        return quoted + " ends in a dot or a space (Windows drops them)";
    }
    if (reserved(c)) {
        return quoted + " is a name Windows reserves for a device";
    }
    if (c.size() >= partSuffix.size() && c.substr(c.size() - partSuffix.size()) == partSuffix) {
        return quoted + " ends in .rcsdk-part, the module's own partial files";
    }
    if (c.substr(0, recordPrefix.size()) == recordPrefix) {
        return quoted + " starts with .rcsdk-resume-, the module's own records";
    }
    return std::nullopt;
}

} // namespace

std::optional<std::string> pathProblem(std::string_view path) {
    if (path.empty()) {
        return std::string("it is empty");
    }
    if (path.size() >= RCS_FILE_PATH_MAX) {
        return "it is longer than " + std::to_string(RCS_FILE_PATH_MAX - 1) + " bytes";
    }
    if (!utf8(path)) {
        return std::string("it is not UTF-8");
    }
    size_t start = 0;
    for (;;) {
        const size_t slash = path.find('/', start);
        const auto component = path.substr(start, slash == std::string_view::npos ? path.npos : slash - start);
        if (auto problem = componentProblem(component)) {
            return problem;
        }
        if (slash == std::string_view::npos) {
            return std::nullopt;
        }
        start = slash + 1;
    }
}

std::vector<std::string> components(std::string_view path) {
    std::vector<std::string> parts;
    size_t start = 0;
    for (;;) {
        const size_t slash = path.find('/', start);
        parts.emplace_back(path.substr(start, slash == std::string_view::npos ? path.npos : slash - start));
        if (slash == std::string_view::npos) {
            return parts;
        }
        start = slash + 1;
    }
}

std::string lastComponent(std::string_view path) {
    const size_t slash = path.rfind('/');
    return std::string(slash == std::string_view::npos ? path : path.substr(slash + 1));
}

std::string parentOf(std::string_view path) {
    const size_t slash = path.rfind('/');
    return slash == std::string_view::npos ? std::string() : std::string(path.substr(0, slash));
}

std::string numbered(std::string_view path, uint32_t n) {
    const std::string parent = parentOf(path);
    const std::string name = lastComponent(path);
    // "archive.tar" -> "archive (1).tar"; ".profile" and "README" -> "... (1)".
    const size_t dot = name.rfind('.');
    const bool extension = dot != std::string::npos && dot > 0;
    std::string stem = extension ? name.substr(0, dot) : name;
    const std::string ext = extension ? name.substr(dot) : std::string();
    const std::string mark = " (" + std::to_string(n) + ")";
    while (!stem.empty() && stem.size() + mark.size() + ext.size() > maxComponent) {
        size_t last = stem.size() - 1; // Its last character goes, whole.
        while (last > 0 && (uint8_t(stem[last]) & 0xC0) == 0x80) {
            --last;
        }
        stem.resize(last);
    }
    const std::string renamed = stem + mark + ext;
    return parent.empty() ? renamed : parent + "/" + renamed;
}

std::string recordName(const Token& token) {
    return std::string(recordPrefix) + hex(token);
}

} // namespace rcsdk::file_transfer
