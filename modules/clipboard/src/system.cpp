#include "system.hpp"

#include <mutex>

namespace rcsdk::clipboard {
namespace {

std::mutex mutex;
Factory factory;

} // namespace

void setFactory(Factory replacement) {
    std::lock_guard lock(mutex);
    factory = std::move(replacement);
}

std::unique_ptr<SystemClipboard> createClipboard(rcs_role role) {
    std::unique_lock lock(mutex);
    const auto made = factory;
    lock.unlock();
    return made ? made(role) : createPlatformClipboard();
}

uint32_t systemFormats() noexcept {
    std::lock_guard lock(mutex);
    return factory ? allFormats : platformFormats();
}

bool systemCarriesFiles() noexcept {
    std::lock_guard lock(mutex);
    return factory || platformCarriesFiles();
}

uint32_t systemFiles(std::string& why) {
    std::unique_lock lock(mutex);
    if (factory) {
        return RCS_CLIPBOARD_SEND | RCS_CLIPBOARD_RECEIVE;
    }
    lock.unlock();
    return platformFiles(why);
}

} // namespace rcsdk::clipboard
