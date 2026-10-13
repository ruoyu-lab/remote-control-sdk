#include "reader.hpp"

#include <mutex>

namespace rcsdk::text_caret {
namespace {

std::mutex mutex;
Factory override;

} // namespace

void setFactory(Factory factory) {
    std::lock_guard lock(mutex);
    override = std::move(factory);
}

std::unique_ptr<Reader> createReader(rcs_module module) {
    std::unique_lock lock(mutex);
    const auto factory = override;
    lock.unlock();
    return factory ? factory() : createPlatformReader(module);
}

bool available() noexcept {
    std::lock_guard lock(mutex);
    return override || platformAvailable();
}

} // namespace rcsdk::text_caret
