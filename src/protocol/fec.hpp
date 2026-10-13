// Forward error correction for video over datagrams: a systematic
// Reed-Solomon erasure code over GF(2^8) (polynomial 0x11D) with a Cauchy
// matrix. k data blocks get m parity blocks (k + m <= 255); any k of the
// k + m rebuild the data, so up to m lost blocks come back without a round
// trip. Blocks shorter than the group's block size count as padded with zeros.
// Parity block j is the sum over the data blocks i of data_i times
// 1 / (x_j + y_i) with x_j = j and y_i = m + i, both ends computing the same.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace rcsdk::protocol::fec {

inline constexpr size_t maxBlocks = 255; // Data and parity blocks of one group.

// The m parity blocks of `data`, each `size` bytes (no data block is longer).
std::vector<std::vector<uint8_t>> encode(std::span<const std::span<const uint8_t>> data, size_t m, size_t size);

// Rebuilds the missing data blocks (empty entries of `data`) from the parity
// blocks that arrived (non-empty entries of `parity`, each `size` bytes).
// False, changing nothing, when fewer parity blocks arrived than data blocks
// are missing. A rebuilt block is `size` bytes: the caller cuts it to its length.
bool recover(std::vector<std::vector<uint8_t>>& data, std::span<const std::vector<uint8_t>> parity, size_t size);

} // namespace rcsdk::protocol::fec
