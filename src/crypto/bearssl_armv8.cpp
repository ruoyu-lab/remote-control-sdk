// bearssl_armv8.hpp through the ACLE intrinsics: each function that uses the
// instructions is compiled for them alone (target attributes) and called only
// where the processor has them.
#include "crypto/bearssl_armv8.hpp"

#if defined(__aarch64__) && __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
#include <arm_neon.h>

#include <algorithm>
#include <cstdint>
#include <cstring>

#ifdef __linux__ // Android too.
#include <asm/hwcap.h>
#include <sys/auxv.h>
#endif

#define RCSDK_AES __attribute__((target("+aes")))
#define RCSDK_SHA2 __attribute__((target("+sha2")))

namespace rcsdk::crypto::armv8 {
namespace {

// What the compiler may assume (every ARM64 processor of Apple's has both),
// else what Linux and Android say the processor has.
bool hasAes() noexcept {
#if defined(__ARM_FEATURE_AES)
    return true;
#elif defined(__linux__)
    return getauxval(AT_HWCAP) & HWCAP_AES;
#else
    return false;
#endif
}

bool hasSha256() noexcept {
#if defined(__ARM_FEATURE_SHA2)
    return true;
#elif defined(__linux__)
    return getauxval(AT_HWCAP) & HWCAP_SHA2;
#else
    return false;
#endif
}

// ---- AES-CTR ----------------------------------------------------------------------

struct AesKeys {
    const br_block_ctr_class* vtable;
    uint8_t roundKeys[15][16];
    unsigned rounds; // 10, 12 or 14.
};
static_assert(sizeof(AesKeys) <= sizeof(br_aes_gen_ctr_keys));

extern const br_block_ctr_class aesCtrClass;

// SubWord: AESE with a zero round key is SubBytes and ShiftRows, the latter
// moving nothing when all four columns hold the word.
RCSDK_AES uint32_t subWord(uint32_t word) {
    const uint8x16_t state = vaeseq_u8(vreinterpretq_u8_u32(vdupq_n_u32(word)), vdupq_n_u8(0));
    return vgetq_lane_u32(vreinterpretq_u32_u8(state), 0);
}

// FIPS 197 key expansion, its words little-endian (byte 0 lowest): RotWord
// is a rotation right by 8 bits, Rcon goes into the low byte.
RCSDK_AES void aesInit(const br_block_ctr_class** context, const void* key, size_t length) {
    auto& keys = *reinterpret_cast<AesKeys*>(context);
    keys.vtable = &aesCtrClass;
    const size_t keyWords = length / 4;
    keys.rounds = unsigned(keyWords + 6);
    uint32_t words[60];
    std::memcpy(words, key, length);
    uint32_t rcon = 1;
    for (size_t i = keyWords; i < 4 * (keys.rounds + 1); ++i) {
        uint32_t word = words[i - 1];
        if (i % keyWords == 0) {
            word = subWord((word >> 8) | (word << 24)) ^ rcon;
            rcon = (rcon << 1) ^ (0x11b & (0 - (rcon >> 7)));
        } else if (keyWords == 8 && i % keyWords == 4) {
            word = subWord(word);
        }
        words[i] = words[i - keyWords] ^ word;
    }
    std::memcpy(keys.roundKeys, words, 16 * (keys.rounds + 1));
}

// The keystream of `n` counter blocks (`base` with cc, cc + 1... big-endian in
// its last word), XORed into the first `length` bytes of `data`. The blocks
// go through each round together: the processor pipelines them.
template <size_t n>
RCSDK_AES void ctrBlocks(const uint8x16_t* roundKeys, unsigned rounds, uint8x16_t base, uint32_t cc, uint8_t* data,
                         size_t length) {
    uint8x16_t blocks[n];
    for (size_t i = 0; i < n; ++i) {
        blocks[i] =
            vreinterpretq_u8_u32(vsetq_lane_u32(__builtin_bswap32(cc + uint32_t(i)), vreinterpretq_u32_u8(base), 3));
    }
    for (unsigned round = 0; round + 1 < rounds; ++round) {
        for (size_t i = 0; i < n; ++i) {
            blocks[i] = vaesmcq_u8(vaeseq_u8(blocks[i], roundKeys[round]));
        }
    }
    for (size_t i = 0; i < n; ++i) {
        blocks[i] = veorq_u8(vaeseq_u8(blocks[i], roundKeys[rounds - 1]), roundKeys[rounds]);
    }
    if (length == 16 * n) {
        for (size_t i = 0; i < n; ++i) {
            vst1q_u8(data + 16 * i, veorq_u8(vld1q_u8(data + 16 * i), blocks[i]));
        }
        return;
    }
    uint8_t stream[16 * n];
    for (size_t i = 0; i < n; ++i) {
        vst1q_u8(stream + 16 * i, blocks[i]);
    }
    for (size_t i = 0; i < length; ++i) {
        data[i] ^= stream[i];
    }
}

// As br_aes_x86ni_ctr_run: the counter in the last 4 bytes of each block
// (from cc, wrapping at 32 bits), after the 12 bytes of `iv`; the new counter
// (a partial last block not counted).
RCSDK_AES uint32_t aesRun(const br_block_ctr_class* const* context, const void* iv, uint32_t cc, void* data,
                          size_t length) {
    const auto& keys = *reinterpret_cast<const AesKeys*>(context);
    uint8x16_t roundKeys[15];
    for (unsigned round = 0; round <= keys.rounds; ++round) {
        roundKeys[round] = vld1q_u8(keys.roundKeys[round]);
    }
    uint8_t first[16] = {};
    std::memcpy(first, iv, 12);
    const uint8x16_t base = vld1q_u8(first);
    auto* bytes = static_cast<uint8_t*>(data);
    for (; length >= 128; bytes += 128, length -= 128, cc += 8) {
        ctrBlocks<8>(roundKeys, keys.rounds, base, cc, bytes, 128);
    }
    if (length > 64) {
        ctrBlocks<8>(roundKeys, keys.rounds, base, cc, bytes, length);
    } else if (length > 16) {
        ctrBlocks<4>(roundKeys, keys.rounds, base, cc, bytes, length);
    } else if (length > 0) {
        ctrBlocks<1>(roundKeys, keys.rounds, base, cc, bytes, length);
    }
    return cc + uint32_t(length / 16);
}

const br_block_ctr_class aesCtrClass = {sizeof(AesKeys), 16, 4, aesInit, aesRun};

// ---- SHA-256 ----------------------------------------------------------------------

// FIPS 180-4, 4.2.2 and 5.3.3.
constexpr uint32_t roundConstants[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
constexpr uint32_t initialHash[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                     0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};

extern const br_hash_class sha256Class;

// Four rounds with the message words w0 (and the constants at k); then, while
// the schedule goes on, w0 becomes the words four on: W[t+16..t+19] from
// W[t..t+15] = w0 w1 w2 w3.
RCSDK_SHA2 inline void fourRounds(uint32x4_t& abcd, uint32x4_t& efgh, uint32x4_t& w0, uint32x4_t w1, uint32x4_t w2,
                                  uint32x4_t w3, const uint32_t* k, bool schedule) {
    const uint32x4_t wk = vaddq_u32(w0, vld1q_u32(k));
    if (schedule) {
        w0 = vsha256su1q_u32(vsha256su0q_u32(w0, w1), w2, w3);
    }
    const uint32x4_t before = abcd;
    abcd = vsha256hq_u32(abcd, efgh, wk);
    efgh = vsha256h2q_u32(efgh, before, wk);
}

// The compression function over `count` 64-byte blocks; `state` is A..H.
RCSDK_SHA2 void compress(uint32_t* state, const uint8_t* blocks, size_t count) {
    uint32x4_t abcd = vld1q_u32(state), efgh = vld1q_u32(state + 4);
    for (; count > 0; --count, blocks += 64) {
        uint32x4_t w0 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blocks)));
        uint32x4_t w1 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blocks + 16)));
        uint32x4_t w2 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blocks + 32)));
        uint32x4_t w3 = vreinterpretq_u32_u8(vrev32q_u8(vld1q_u8(blocks + 48)));
        const uint32x4_t abcdBefore = abcd, efghBefore = efgh;
        for (size_t t = 0; t < 64; t += 16) {
            const bool schedule = t < 48;
            fourRounds(abcd, efgh, w0, w1, w2, w3, roundConstants + t, schedule);
            fourRounds(abcd, efgh, w1, w2, w3, w0, roundConstants + t + 4, schedule);
            fourRounds(abcd, efgh, w2, w3, w0, w1, roundConstants + t + 8, schedule);
            fourRounds(abcd, efgh, w3, w0, w1, w2, roundConstants + t + 12, schedule);
        }
        abcd = vaddq_u32(abcd, abcdBefore);
        efgh = vaddq_u32(efgh, efghBefore);
    }
    vst1q_u32(state, abcd);
    vst1q_u32(state + 4, efgh);
}

br_sha256_context& sha256Context(const br_hash_class** context) {
    return *reinterpret_cast<br_sha256_context*>(context);
}

void sha256Init(const br_hash_class** context) {
    auto& c = sha256Context(context);
    c.vtable = &sha256Class;
    std::memcpy(c.val, initialHash, sizeof(c.val));
    c.count = 0;
}

void sha256Update(const br_hash_class** context, const void* data, size_t length) {
    auto& c = sha256Context(context);
    auto* bytes = static_cast<const uint8_t*>(data);
    const auto buffered = size_t(c.count) & 63;
    c.count += length;
    if (buffered > 0) {
        const size_t taken = std::min(64 - buffered, length);
        std::memcpy(c.buf + buffered, bytes, taken);
        if (buffered + taken < 64) {
            return;
        }
        compress(c.val, c.buf, 1);
        bytes += taken;
        length -= taken;
    }
    if (length >= 64) {
        compress(c.val, bytes, length / 64);
    }
    std::memcpy(c.buf, bytes + length / 64 * 64, length % 64);
}

// Padding (0x80, zeros, the length in bits big-endian) into one block or two.
void sha256Out(const br_hash_class* const* context, void* output) {
    const auto& c = *reinterpret_cast<const br_sha256_context*>(context);
    uint8_t last[128] = {};
    const auto buffered = size_t(c.count) & 63;
    std::memcpy(last, c.buf, buffered);
    last[buffered] = 0x80;
    const size_t blocks = buffered < 56 ? 1 : 2;
    const uint64_t bits = c.count << 3;
    for (size_t i = 0; i < 8; ++i) {
        last[64 * blocks - 1 - i] = uint8_t(bits >> (8 * i));
    }
    uint32_t state[8];
    std::memcpy(state, c.val, sizeof(state));
    compress(state, last, blocks);
    auto* bytes = static_cast<uint8_t*>(output);
    for (size_t i = 0; i < 8; ++i) {
        for (size_t j = 0; j < 4; ++j) {
            bytes[4 * i + j] = uint8_t(state[i] >> (24 - 8 * j));
        }
    }
}

// The state as BearSSL's own (br_hmac's padded keys come and go through it).
uint64_t sha256State(const br_hash_class* const* context, void* output) {
    return br_sha256_state(reinterpret_cast<const br_sha256_context*>(context), output);
}

void sha256SetState(const br_hash_class** context, const void* state, uint64_t count) {
    br_sha256_set_state(&sha256Context(context), state, count);
}

// As br_sha256_vtable's: its identifier, sizes and padding.
constexpr uint32_t sha256Descriptor = BR_HASHDESC_ID(br_sha256_ID) | BR_HASHDESC_OUT(32) | BR_HASHDESC_STATE(32) |
                                      BR_HASHDESC_LBLEN(6) | BR_HASHDESC_MD_PADDING | BR_HASHDESC_MD_PADDING_BE;

const br_hash_class sha256Class = {
    sizeof(br_sha256_context), sha256Descriptor, sha256Init, sha256Update, sha256Out, sha256State, sha256SetState,
};

} // namespace

const br_block_ctr_class* aesCtr() noexcept {
    return hasAes() ? &aesCtrClass : nullptr;
}

const br_hash_class* sha256() noexcept {
    return hasSha256() ? &sha256Class : nullptr;
}

} // namespace rcsdk::crypto::armv8

#else

namespace rcsdk::crypto::armv8 {

const br_block_ctr_class* aesCtr() noexcept {
    return nullptr;
}

const br_hash_class* sha256() noexcept {
    return nullptr;
}

} // namespace rcsdk::crypto::armv8

#endif
