#include "protocol/fec.hpp"

#include <array>
#include <utility>

namespace rcsdk::protocol::fec {
namespace {

// GF(2^8) with the primitive polynomial x^8 + x^4 + x^3 + x^2 + 1, and every
// product in a table (64 KiB): a block times a coefficient is a lookup per byte.
struct Field {
    std::array<uint8_t, 512> exp{};
    std::array<uint8_t, 256> log{};
    std::vector<uint8_t> products = std::vector<uint8_t>(256 * 256);

    Field() {
        unsigned x = 1;
        for (unsigned i = 0; i < 255; ++i) {
            exp[i] = uint8_t(x);
            log[x] = uint8_t(i);
            x <<= 1;
            if (x & 0x100) {
                x ^= 0x11D;
            }
        }
        for (unsigned i = 255; i < exp.size(); ++i) {
            exp[i] = exp[i - 255];
        }
        for (unsigned a = 1; a < 256; ++a) {
            for (unsigned b = 1; b < 256; ++b) {
                products[a * 256 + b] = exp[log[a] + log[b]];
            }
        }
    }
    uint8_t mul(uint8_t a, uint8_t b) const noexcept { return products[a * 256u + b]; }
    uint8_t inv(uint8_t a) const noexcept { return exp[255 - log[a]]; } // a != 0
    const uint8_t* row(uint8_t a) const noexcept { return &products[a * 256u]; }
};

const Field& field() {
    static const Field f;
    return f;
}

uint8_t cauchy(size_t parity, size_t data, size_t m) {
    return field().inv(uint8_t(parity ^ (m + data)));
}

// out ^= c * in, over in's bytes.
void addScaled(std::span<uint8_t> out, std::span<const uint8_t> in, uint8_t c) {
    if (!c) {
        return;
    }
    const uint8_t* row = field().row(c);
    for (size_t i = 0; i < in.size(); ++i) {
        out[i] ^= row[in[i]];
    }
}

} // namespace

std::vector<std::vector<uint8_t>> encode(std::span<const std::span<const uint8_t>> data, size_t m, size_t size) {
    std::vector<std::vector<uint8_t>> parity(m, std::vector<uint8_t>(size, 0));
    for (size_t j = 0; j < m; ++j) {
        for (size_t i = 0; i < data.size(); ++i) {
            addScaled(parity[j], data[i], cauchy(j, i, m));
        }
    }
    return parity;
}

bool recover(std::vector<std::vector<uint8_t>>& data, std::span<const std::vector<uint8_t>> parity, size_t size) {
    const Field& f = field();
    const size_t m = parity.size();
    std::vector<size_t> missing, rows;
    for (size_t i = 0; i < data.size(); ++i) {
        if (data[i].empty()) {
            missing.push_back(i);
        }
    }
    for (size_t j = 0; j < m && rows.size() < missing.size(); ++j) {
        if (!parity[j].empty()) {
            rows.push_back(j);
        }
    }
    if (missing.empty() || rows.size() < missing.size()) {
        return missing.empty();
    }
    const size_t e = missing.size();
    // What the missing blocks contribute to each parity block used: the
    // parity less the blocks that arrived.
    std::vector<std::vector<uint8_t>> syndromes;
    for (const size_t j : rows) {
        auto s = parity[j];
        for (size_t i = 0; i < data.size(); ++i) {
            if (!data[i].empty()) {
                addScaled(s, data[i], cauchy(j, i, m));
            }
        }
        syndromes.push_back(std::move(s));
    }
    // Invert the e x e Cauchy submatrix (always invertible) by Gauss-Jordan elimination.
    std::vector<std::vector<uint8_t>> a(e, std::vector<uint8_t>(2 * e, 0));
    for (size_t r = 0; r < e; ++r) {
        for (size_t c = 0; c < e; ++c) {
            a[r][c] = cauchy(rows[r], missing[c], m);
        }
        a[r][e + r] = 1;
    }
    for (size_t c = 0; c < e; ++c) {
        size_t pivot = c;
        while (!a[pivot][c]) {
            ++pivot;
        }
        std::swap(a[pivot], a[c]);
        const uint8_t scale = f.inv(a[c][c]);
        for (auto& value : a[c]) {
            value = f.mul(value, scale);
        }
        for (size_t r = 0; r < e; ++r) {
            if (r != c && a[r][c]) {
                const uint8_t factor = a[r][c];
                for (size_t k = 0; k < 2 * e; ++k) {
                    a[r][k] ^= f.mul(factor, a[c][k]);
                }
            }
        }
    }
    for (size_t c = 0; c < e; ++c) {
        std::vector<uint8_t> block(size, 0);
        for (size_t r = 0; r < e; ++r) {
            addScaled(block, syndromes[r], a[c][e + r]);
        }
        data[missing[c]] = std::move(block);
    }
    return true;
}

} // namespace rcsdk::protocol::fec
