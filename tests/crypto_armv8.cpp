// The BearSSL backend's ARMv8 classes (crypto/bearssl_armv8.cpp) against
// published vectors (NIST SP 800-38A's AES-CTR with each key size, FIPS
// 180-4's SHA-256, RFC 4231's HMAC-SHA256) and against BearSSL's own
// software, which they replace where the processor has the instructions:
// random keys, lengths, alignments, counters about to wrap, input split
// every way, states handed between the two. Skipped (77) where the processor
// has neither (or is not ARM64).
//
//   rcsdk_test_crypto_armv8 [--bench]
//
// --bench: MB/s of AES-256-CTR, HMAC-SHA256 (its key set up each time, as
// crypto::hmacSha256 does) and both as crypto::Aead seals a message, at 64 B,
// 1200 B (a datagram) and 16 KiB, BearSSL's software against the
// instructions; and crypto::Aead itself, as the backend chose.
#include "support/check.hpp"

#include "crypto/bearssl_armv8.hpp"
#include "crypto/crypto.hpp"
#include "util/bytes.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <string>
#include <string_view>

using namespace rcsdk;
namespace armv8 = rcsdk::crypto::armv8;

namespace {

Bytes hex(std::string_view text) {
    Bytes bytes;
    for (size_t i = 0; i + 1 < text.size(); i += 2) {
        bytes.push_back(uint8_t(std::stoul(std::string(text.substr(i, 2)), nullptr, 16)));
    }
    return bytes;
}

Bytes text(std::string_view value) {
    return Bytes(value.begin(), value.end());
}

std::mt19937_64 generator(20261013); // Fixed: a failure repeats.

size_t below(size_t limit) {
    return size_t(generator() % limit);
}

Bytes randomBytes(size_t size) {
    Bytes bytes(size);
    for (auto& byte : bytes) {
        byte = uint8_t(generator());
    }
    return bytes;
}

// ---- AES-CTR ----------------------------------------------------------------------

// What 64-bit processors without the instructions use.
const br_block_ctr_class* const softwareCtr = &br_aes_ct64_ctr_vtable;

// `data` through `implementation`'s run with `key`, the first 12 bytes of
// `iv` and the counter `cc`; the counter it returns.
uint32_t runCtr(const br_block_ctr_class* implementation, std::span<const uint8_t> key, const uint8_t* iv, uint32_t cc,
                uint8_t* data, size_t length) {
    br_aes_gen_ctr_keys keys;
    implementation->init(&keys.vtable, key.data(), key.size());
    return implementation->run(&keys.vtable, iv, cc, data, length);
}

void aesVectors(const br_block_ctr_class* aes) {
    // NIST SP 800-38A F.5.1, F.5.3, F.5.5: CTR-AES128, -AES192, -AES256, four
    // blocks; the counter f0f1...fcfdfeff counts in its last 32 bits.
    const auto counter = hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
    const auto plain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                           "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
    const struct {
        const char* key;
        const char* cipher;
    } vectors[] = {
        {"2b7e151628aed2a6abf7158809cf4f3c", "874d6191b620e3261bef6864990db6ce9806f66b7970fdff8617187bb9fffdff"
                                             "5ae4df3edbd5d35e5b4f09020db03eab1e031dda2fbe03d1792170a0f3009cee"},
        {"8e73b0f7da0e6452c810f32b809079e562f8ead2522c6b7b",
         "1abc932417521ca24f2b0459fe7e6e0b090339ec0aa6faefd5ccc2c6f4ce8e94"
         "1e36b26bd1ebc670d1bd1d665620abf74f78a7f6d29809585a97daec58c6b050"},
        {"603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4",
         "601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5"
         "2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6"},
    };
    const auto cc = uint32_t(loadBigEndian(counter, 12, 4));
    for (const auto& vector : vectors) {
        const auto key = hex(vector.key), expected = hex(vector.cipher);
        for (const size_t length : {size_t(64), size_t(23), size_t(16), size_t(5)}) {
            Bytes data(plain.begin(), plain.begin() + ptrdiff_t(length));
            CHECK(runCtr(aes, key, counter.data(), cc, data.data(), length) == cc + uint32_t(length / 16));
            CHECK(std::equal(data.begin(), data.end(), expected.begin()));
        }
        // Decryption is the same.
        Bytes data = expected;
        runCtr(aes, key, counter.data(), cc, data.data(), data.size());
        CHECK(data == plain);
    }
}

// Random keys of each size, lengths, offsets in the buffer and counters
// (some about to wrap at 32 bits), against BearSSL's software; nothing
// outside the bytes given is written; a run split into whole blocks carries
// the counter on as one would.
void aesCrossCheck(const br_block_ctr_class* aes) {
    constexpr size_t lengths[] = {1, 15, 16, 17, 31, 32, 33, 63, 64, 65, 127, 128, 129, 191, 255, 256, 1200, 16384};
    for (size_t round = 0; round < 3000; ++round) {
        const auto key = randomBytes(16 + 8 * below(3));
        const auto iv = randomBytes(12);
        const size_t length = round < std::size(lengths) * 3 ? lengths[round % std::size(lengths)]
                              : round % 2                    ? below(300)
                                                             : below(5000);
        const uint32_t cc = round % 3 == 0 ? uint32_t(0) - uint32_t(below(20)) : uint32_t(generator());
        const size_t offset = below(16);
        Bytes buffer = randomBytes(length + 48), expected = buffer;
        const uint32_t next = runCtr(aes, key, iv.data(), cc, buffer.data() + offset, length);
        CHECK(next == runCtr(softwareCtr, key, iv.data(), cc, expected.data() + offset, length));
        CHECK(next == cc + uint32_t(length / 16));
        CHECK(buffer == expected);
        // Whole blocks at a time, then the rest: the ciphertext back to the plaintext...
        Bytes pieces = expected;
        br_aes_gen_ctr_keys keys;
        aes->init(&keys.vtable, key.data(), key.size());
        uint32_t counter = cc;
        for (size_t done = 0; done < length;) {
            const size_t piece = std::min(length - done, 16 * (1 + below(12)));
            counter = aes->run(&keys.vtable, iv.data(), counter, pieces.data() + offset + done, piece);
            done += piece;
        }
        CHECK(counter == next);
        runCtr(softwareCtr, key, iv.data(), cc, expected.data() + offset, length); // ...here as well.
        CHECK(pieces == expected);
    }
}

// ---- SHA-256 and HMAC -------------------------------------------------------------

Bytes digest(const br_hash_class* hash, std::span<const uint8_t> message, size_t maxPiece) {
    br_sha256_context context;
    hash->init(&context.vtable);
    for (size_t done = 0; done < message.size();) {
        const size_t piece = std::min(message.size() - done, maxPiece ? below(maxPiece + 1) : message.size());
        hash->update(&context.vtable, message.data() + done, piece);
        done += piece;
    }
    Bytes output(32);
    hash->out(&context.vtable, output.data());
    return output;
}

Bytes hmac(const br_hash_class* hash, std::span<const uint8_t> key, std::span<const uint8_t> message) {
    br_hmac_key_context keyContext;
    br_hmac_key_init(&keyContext, hash, key.data(), key.size());
    br_hmac_context context;
    br_hmac_init(&context, &keyContext, 0);
    br_hmac_update(&context, message.data(), message.size());
    Bytes output(32);
    br_hmac_out(&context, output.data());
    return output;
}

void shaVectors(const br_hash_class* sha) {
    // FIPS 180-4's examples (one block, two blocks, a million "a") and the empty message.
    const struct {
        Bytes message;
        const char* digest;
    } vectors[] = {
        {{}, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"},
        {text("abc"), "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"},
        {text("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"),
         "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1"},
        {text("abcdefghbcdefghicdefghijdefghijkefghijklfghijklmghijklmnhijklmnoijklmnopjklmnopqklmnopqrlmnopqrsmnopq"
              "rstnopqrstu"),
         "cf5b16a778af8380036ce59e7b0492370b249b11e8f07a51afac45037afee9d1"},
        {Bytes(1000000, 'a'), "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0"},
    };
    for (const auto& vector : vectors) {
        CHECK(digest(sha, vector.message, 0) == hex(vector.digest));
        CHECK(digest(sha, vector.message, 100) == hex(vector.digest));
    }
    // RFC 4231, test cases 1 to 7 (5 truncated to 128 bits).
    const struct {
        Bytes key, data;
        const char* mac;
    } macs[] = {
        {Bytes(20, 0x0b), text("Hi There"), "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7"},
        {text("Jefe"), text("what do ya want for nothing?"),
         "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843"},
        {Bytes(20, 0xaa), Bytes(50, 0xdd), "773ea91e36800e46854db8ebd09181a72959098b3ef8c122d9635514ced565fe"},
        {hex("0102030405060708090a0b0c0d0e0f10111213141516171819"), Bytes(50, 0xcd),
         "82558a389a443c0ea4cc819899f2083a85f0faa3e578f8077a2e3ff46729665b"},
        {Bytes(20, 0x0c), text("Test With Truncation"), "a3b6167473100ee06e0c796c2955552b"},
        {Bytes(131, 0xaa), text("Test Using Larger Than Block-Size Key - Hash Key First"),
         "60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54"},
        {Bytes(131, 0xaa),
         text("This is a test using a larger than block-size key and a larger than block-size data. The key needs to "
              "be hashed before being used by the HMAC algorithm."),
         "9b09ffa71b942fcb27635fbcd5b0e944bfdc63644f0713938a7f51535c3a35e2"},
    };
    for (const auto& vector : macs) {
        const auto expected = hex(vector.mac);
        const auto mac = hmac(sha, vector.key, vector.data);
        CHECK(std::equal(expected.begin(), expected.end(), mac.begin()));
    }
}

// Random messages fed in random pieces, against BearSSL's software; the state
// handed from either to the other after whole blocks (as br_hmac does with
// its padded keys); HMAC with random keys.
void shaCrossCheck(const br_hash_class* sha) {
    const br_hash_class* software = &br_sha256_vtable;
    for (size_t round = 0; round < 2000; ++round) {
        const size_t length = round < 200 ? round : round % 2 ? below(300) : below(5000);
        const size_t offset = below(16);
        const auto buffer = randomBytes(length + 16);
        const auto message = std::span(buffer).subspan(offset, length);
        const auto expected = digest(software, message, 0);
        CHECK(digest(sha, message, round % 4 ? 1 + below(200) : 0) == expected);
        const size_t split = 64 * below(length / 64 + 1);
        for (const auto& [from, to] : {std::pair{sha, software}, std::pair{software, sha}}) {
            br_sha256_context first, second;
            from->init(&first.vtable);
            from->update(&first.vtable, message.data(), split);
            uint8_t state[32];
            const uint64_t count = from->state(&first.vtable, state);
            CHECK(count == split);
            to->init(&second.vtable);
            to->set_state(&second.vtable, state, count);
            to->update(&second.vtable, message.data() + split, length - split);
            Bytes output(32);
            to->out(&second.vtable, output.data());
            CHECK(output == expected);
        }
        const auto key = randomBytes(below(200));
        CHECK(hmac(sha, key, message) == hmac(software, key, message));
    }
}

// ---- Measurement ------------------------------------------------------------------

// MB/s of `work` over `size` bytes, run for about half a second.
template <class Work>
double megabytesPerSecond(size_t size, Work&& work) {
    using Clock = std::chrono::steady_clock;
    for (int i = 0; i < 16; ++i) {
        work();
    }
    size_t runs = 0;
    const auto start = Clock::now();
    std::chrono::duration<double> elapsed{};
    do {
        for (int i = 0; i < 16; ++i) {
            work();
        }
        runs += 16;
        elapsed = Clock::now() - start;
    } while (elapsed.count() < 0.5);
    return double(runs * size) / elapsed.count() / 1e6;
}

int bench(const br_block_ctr_class* aes, const br_hash_class* sha) {
    crypto::Key key{}, macKey{};
    crypto::random(key);
    crypto::random(macKey);
    const auto iv = randomBytes(12), associated = randomBytes(16);
    const crypto::Aead aead(key, macKey);
    std::printf("MB/s (10^6 bytes a second), one thread; BearSSL's software / the ARMv8 instructions\n");
    std::printf("%-8s %-22s %-22s %-22s %s\n", "bytes", "AES-256-CTR", "HMAC-SHA256", "CTR + HMAC (Aead)",
                "crypto::Aead::seal");
    for (const size_t size : {size_t(64), size_t(1200), size_t(16384)}) {
        const auto plain = randomBytes(size);
        Bytes data(size), header(12);
        uint32_t counter = 0;
        const auto ctr = [&](const br_block_ctr_class* implementation) {
            br_aes_gen_ctr_keys keys;
            implementation->init(&keys.vtable, key.data(), key.size());
            return megabytesPerSecond(size, [&] {
                std::copy(plain.begin(), plain.end(), data.begin());
                implementation->run(&keys.vtable, iv.data(), counter++, data.data(), size);
            });
        };
        uint8_t output[32];
        const auto mac = [&](const br_hash_class* hash) {
            return megabytesPerSecond(size, [&] {
                br_hmac_key_context keyContext;
                br_hmac_key_init(&keyContext, hash, macKey.data(), macKey.size());
                br_hmac_context context;
                br_hmac_init(&context, &keyContext, 0);
                br_hmac_update(&context, plain.data(), plain.size());
                br_hmac_out(&context, output);
            });
        };
        // As Aead::seal: the plaintext copied out, encrypted in place, then
        // the MAC over sequence and length, the associated data and the ciphertext.
        const auto seal = [&](const br_block_ctr_class* implementation, const br_hash_class* hash) {
            br_aes_gen_ctr_keys keys;
            implementation->init(&keys.vtable, key.data(), key.size());
            return megabytesPerSecond(size, [&] {
                std::copy(plain.begin(), plain.end(), data.begin());
                implementation->run(&keys.vtable, iv.data(), counter++, data.data(), size);
                br_hmac_key_context keyContext;
                br_hmac_key_init(&keyContext, hash, macKey.data(), macKey.size());
                br_hmac_context context;
                br_hmac_init(&context, &keyContext, 0);
                br_hmac_update(&context, header.data(), header.size());
                br_hmac_update(&context, associated.data(), associated.size());
                br_hmac_update(&context, data.data(), data.size());
                br_hmac_out(&context, output);
            });
        };
        uint64_t sequence = 0;
        const double sealed = megabytesPerSecond(size, [&] { aead.seal(sequence++, associated, plain); });
        const auto pair = [](double software, double hardware) {
            char cell[64];
            std::snprintf(cell, sizeof(cell), "%.0f / %.0f (x%.1f)", software, hardware, hardware / software);
            return std::string(cell);
        };
        std::printf("%-8zu %-22s %-22s %-22s %.0f\n", size, pair(ctr(softwareCtr), ctr(aes)).c_str(),
                    pair(mac(&br_sha256_vtable), mac(sha)).c_str(),
                    pair(seal(softwareCtr, &br_sha256_vtable), seal(aes, sha)).c_str(), sealed);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const br_block_ctr_class* aes = armv8::aesCtr();
    const br_hash_class* sha = armv8::sha256();
    if (!aes && !sha) {
        std::printf("crypto_armv8: the processor has neither ARMv8 AES nor SHA-256 instructions; skipped\n");
        return 77;
    }
    std::printf("crypto_armv8: AES %s, SHA-256 %s\n", aes ? "yes" : "no", sha ? "yes" : "no");
    if (argc > 1 && std::string(argv[1]) == "--bench") {
        CHECK(aes && sha);
        return bench(aes, sha);
    }
    return runTest("crypto_armv8", [&] {
        if (aes) {
            aesVectors(aes);
            aesVectors(softwareCtr); // The vectors themselves.
            aesCrossCheck(aes);
        }
        if (sha) {
            shaVectors(sha);
            shaVectors(&br_sha256_vtable);
            shaCrossCheck(sha);
        }
    });
}
