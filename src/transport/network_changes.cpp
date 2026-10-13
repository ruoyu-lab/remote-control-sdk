#include "transport/network_changes.hpp"

#include <atomic>

namespace rcsdk::transport {
namespace {

std::atomic<uint64_t> generation{0};

} // namespace

uint64_t networkGeneration() noexcept {
    return generation.load(std::memory_order_acquire);
}

void networkChanged() noexcept {
    generation.fetch_add(1, std::memory_order_acq_rel);
}

} // namespace rcsdk::transport
