// Reed-Solomon erasure coding: any m lost blocks of a group come back, m + 1 do not.
#include "protocol/fec.hpp"
#include "support/check.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <numeric>
#include <random>

using namespace rcsdk;
using namespace rcsdk::protocol;

namespace {

using Block = std::vector<uint8_t>;

// k blocks of `size` bytes, the last one `last` bytes.
std::vector<Block> blocks(std::mt19937& random, size_t k, size_t size, size_t last) {
    std::vector<Block> data(k);
    for (size_t i = 0; i < k; ++i) {
        data[i].resize(i + 1 == k ? last : size);
        for (auto& byte : data[i]) {
            byte = uint8_t(random());
        }
    }
    return data;
}

std::vector<std::span<const uint8_t>> spans(const std::vector<Block>& data) {
    return {data.begin(), data.end()};
}

// Loses `lostData` data blocks and `lostParity` parity blocks at random, and recovers.
bool roundTrip(std::mt19937& random, size_t k, size_t m, size_t lostData, size_t lostParity) {
    const size_t size = 1 + random() % 1200, last = 1 + random() % size;
    const auto data = blocks(random, k, size, last);
    auto parity = fec::encode(spans(data), m, size);
    CHECK(parity.size() == m && std::all_of(parity.begin(), parity.end(), [&](const Block& p) {
              return p.size() == size;
          }));
    std::vector<size_t> order(k);
    std::iota(order.begin(), order.end(), 0);
    std::shuffle(order.begin(), order.end(), random);
    auto received = data;
    for (size_t i = 0; i < lostData; ++i) {
        received[order[i]].clear();
    }
    std::vector<size_t> parityOrder(m);
    std::iota(parityOrder.begin(), parityOrder.end(), 0);
    std::shuffle(parityOrder.begin(), parityOrder.end(), random);
    for (size_t j = 0; j < lostParity; ++j) {
        parity[parityOrder[j]].clear();
    }
    const auto before = received;
    if (!fec::recover(received, parity, size)) {
        CHECK(received == before); // Nothing changes.
        return false;
    }
    for (size_t i = 0; i < k; ++i) {
        received[i].resize(i + 1 == k ? last : size);
    }
    CHECK(received == data);
    return true;
}

} // namespace

int main() {
    return runTest("fec", [] {
        std::mt19937 random(7);
        // Every loss the parity covers comes back; one more does not.
        for (const auto [k, m] : {std::pair<size_t, size_t>{1, 1}, {1, 4}, {2, 1}, {10, 3}, {64, 7}, {64, 32},
                                  {64, 64}, {200, 55}, {128, 127}}) {
            for (size_t lost = 0; lost <= std::min(k, m); ++lost) {
                CHECK(roundTrip(random, k, m, lost, m - lost)); // Exactly as many parity blocks as lost data.
                CHECK(roundTrip(random, k, m, lost, 0));
            }
            if (m < k) {
                CHECK(!roundTrip(random, k, m, m + 1, 0));
            }
            if (m <= k) {
                CHECK(!roundTrip(random, k, m, m, 1)); // A parity block short.
            }
        }
        // Random groups and losses.
        for (int i = 0; i < 300; ++i) {
            const size_t k = 1 + random() % 200, m = 1 + random() % std::min<size_t>(fec::maxBlocks - k, 64);
            const size_t lostData = random() % (std::min(k, m) + 1);
            CHECK(roundTrip(random, k, m, lostData, random() % (m - lostData + 1)));
        }
        // Nothing lost: nothing to do, even without parity.
        std::vector<Block> whole = blocks(random, 5, 100, 50);
        std::vector<Block> none(2);
        CHECK(fec::recover(whole, none, 100));

        // How long a group of 64 full fragments with 20% parity takes, both ways.
        const auto data = blocks(random, 64, 1200, 1200);
        const auto started = std::chrono::steady_clock::now();
        auto parity = fec::encode(spans(data), 13, 1200);
        const auto encoded = std::chrono::steady_clock::now();
        auto received = data;
        for (size_t i = 0; i < 13; ++i) {
            received[i * 4].clear();
        }
        CHECK(fec::recover(received, parity, 1200) && received == data);
        const auto recovered = std::chrono::steady_clock::now();
        std::printf("fec: 64 x 1200 bytes + 13 parity: encode %.0f us, recover 13 %.0f us\n",
                    std::chrono::duration<double, std::micro>(encoded - started).count(),
                    std::chrono::duration<double, std::micro>(recovered - encoded).count());
    });
}
