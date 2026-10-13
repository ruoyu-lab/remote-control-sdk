// What every platform's file system shares: the walk through a directory's
// components, the names messages give, and the test seam for free space.
#include "files.hpp"

#include <mutex>

namespace rcsdk::file_transfer::files {
namespace {

std::mutex mutex;
FreeSpace replacement;

} // namespace

void setFreeSpace(FreeSpace space) {
    std::lock_guard lock(mutex);
    replacement = std::move(space);
}

std::optional<Directory> Directory::find(std::string_view relative) const {
    return walk(relative, false);
}

Directory Directory::make(std::string_view relative) const {
    return *walk(relative, true);
}

std::string Directory::under(std::string_view name) const {
    return below_.empty() ? std::string(name) : below_ + "/" + std::string(name);
}

std::optional<Directory> Directory::walk(std::string_view relative, bool make) const {
    std::optional<Directory> at = *this;
    for (size_t start = 0; at && start < relative.size();) {
        const size_t slash = std::min(relative.find('/', start), relative.size());
        at = at->child(std::string(relative.substr(start, slash - start)), make);
        start = slash + 1;
    }
    return at;
}

uint64_t freeSpace(const std::string& directory) {
    std::unique_lock lock(mutex);
    const auto space = replacement;
    lock.unlock();
    if (space) {
        if (const auto bytes = space(directory)) {
            return *bytes;
        }
    }
    return systemFreeSpace(directory);
}

} // namespace rcsdk::file_transfer::files
