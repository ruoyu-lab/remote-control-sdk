#include "power_request.hpp"

#include <mutex>

namespace rcsdk::power {
namespace {

std::mutex factoryMutex;
Factory replaced;

} // namespace

void setFactory(Factory factory) {
    std::lock_guard lock(factoryMutex);
    replaced = std::move(factory);
}

std::unique_ptr<Request> createRequest(rcs_keep_awake mode, const std::string& reason) {
    std::unique_lock lock(factoryMutex);
    const Factory factory = replaced;
    lock.unlock();
    return factory ? factory(mode, reason) : systemRequest(mode, reason);
}

} // namespace rcsdk::power
