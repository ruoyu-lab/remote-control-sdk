// Relative paths as items carry them (rcs_file_item.name): one set of rules
// on every system, so a path lands under the receiving directory wherever it
// is written, never outside it.
#pragma once

#include "common.hpp"

#include <optional>

namespace rcsdk::file_transfer {

inline constexpr std::string_view partSuffix = ".rcsdk-part";       // A file being written.
inline constexpr std::string_view recordPrefix = ".rcsdk-resume-";  // A transfer's record, then its token in hex.
inline constexpr size_t maxComponent = 255;

// Why `path` is not a relative path items may carry (see rcsdk/file_transfer.h); nullopt when it is.
std::optional<std::string> pathProblem(std::string_view path);
// Its components.
std::vector<std::string> components(std::string_view path);
// Its last component; `path` up to it ("" at the top).
std::string lastComponent(std::string_view path);
std::string parentOf(std::string_view path);
// The name RENAME tries in its n-th turn (n >= 1): "a/b (n).txt" for
// "a/b.txt", the stem cut short where the component would be too long.
std::string numbered(std::string_view path, uint32_t n);
// The record's file name for a transfer.
std::string recordName(const Token& token);

} // namespace rcsdk::file_transfer
