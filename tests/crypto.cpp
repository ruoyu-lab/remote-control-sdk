// The cryptography (the system's or BearSSL's) against published vectors.
#include "crypto/cpace.hpp"
#include "crypto/crypto.hpp"
#include "support/check.hpp"
#include "util/util.hpp"

#include <string>
#include <string_view>

using namespace rcsdk::crypto;

static Bytes hex(const std::string& text) {
    Bytes bytes;
    for (size_t i = 0; i < text.size(); i += 2) {
        bytes.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
    }
    return bytes;
}

static cpace::Point point(const std::string& text) {
    cpace::Point value{};
    const auto bytes = hex(text);
    std::copy(bytes.begin(), bytes.end(), value.begin());
    return value;
}

// CPACE-X25519-SHA512 against the test vectors of draft-irtf-cfrg-cpace-21
// (appendix B.1, "Test vector for CPace using group X25519 and hash SHA-512").
static void cpaceVectors() {
    using namespace cpace;
    const auto sid = hex("7e4b4791d6a8ef019b936c79fb7f2c57");
    const std::string prs = "Password";
    const auto ci = hex("0b415f696e69746961746f720b425f726573706f6e646572");
    // calculate_generator: lv_cat("CPace255", PRS, zero padding, CI, sid), hashed and mapped.
    const auto g = point("d04bf6d41f6a289632a2e929fa29bebd51092512a7829fdde7d314b62f05a73f");
    CHECK(mapToCurve(std::span<const uint8_t, 32>(
              point("03998087bdb1a2617bbe25ef5a7c18cd4f84f902328701790958755ee4aed1d3"))) == g);
    CHECK(generator(Bytes(prs.begin(), prs.end()), ci, sid) == g);
    // The shares, K from either side, ISK in initiator-responder order.
    const auto ya = point("21b4f4bd9e64ed355c3eb676a28ebedaf6d8f17bdc365995b319097153044080");
    const auto yb = point("848b0779ff415f0af4ea14df9dd1d3c29ac41d836c7808896c4eba19c51ac40a");
    const auto sharedA = point("1d13c89278cdadd826f6d8d7f887701430f8380ddc17611cdd6dc989ce0c9f32");
    const auto sharedB = point("248cccf6d5cdc3646f0ad593f9e6cef4e69d4945f8372e623512ecea32185623");
    const auto k = point("5b067effbdc0b2a0e1d907b21ebb25cfedb96a852179a847c37e43ee71322c6b");
    CHECK(multiply(ya, g) == sharedA && multiply(yb, g) == sharedB);
    CHECK(sharedSecret(ya, sharedB) == k && sharedSecret(yb, sharedA) == k);
    const std::string ada = "ADa", adb = "ADb";
    const auto isk = sessionKey(sid, k, sharedA, Bytes(ada.begin(), ada.end()), sharedB, Bytes(adb.begin(), adb.end()));
    CHECK(equal(isk, hex("6e19b875f7a561d6b3ca3dbb9ef42ac55de3e717881018204b8922b4d5e53bb2"
                         "aa82c300bea7b65d2b671da71922ddf6472301b79bc270adfa8bf413285f2263")));
    // A field element of 2^254 or more maps as p minus it (Monocypher reads 254 bits).
    auto high = point("92806dc608984dbf4e4aae478c6ec453ae979cc01ecc1a2a7cf49f5cee56551b");
    high[31] |= 0x40;
    const auto negated = point("5b7f9239f767b240b1b551b873913bac5168633fe133e5d5830b60a311a9aa24");
    CHECK(mapToCurve(std::span<const uint8_t, 32>(high)) == mapToCurve(std::span<const uint8_t, 32>(negated)));
    // scalar_mult_vfy: shares of low order give the neutral element: abort.
    for (const char* weak : {"0000000000000000000000000000000000000000000000000000000000000000",
                             "0100000000000000000000000000000000000000000000000000000000000000",
                             "ecffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                             "edffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                             "eeffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff7f",
                             "e0eb7a7c3b41b8ae1656e3faf19fc46ada098deb9c32b1fd866205165f49b800",
                             "5f9c95bca3508c24b1d0b1559c83ef5b04445cc4581c8e86d8224eddd09f1157"}) {
        CHECK(!sharedSecret(ya, point(weak)));
    }
}

// HMAC-SHA256 against RFC 4231, test cases 1 to 7 (5 truncated to 128 bits),
// whole and in parts.
static void hmacVectors() {
    const auto text = [](std::string_view value) {
        return Bytes(value.begin(), value.end());
    };
    const struct {
        Bytes key, data;
        const char* mac;
    } vectors[] = {
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
    for (const auto& vector : vectors) {
        const auto expected = hex(vector.mac);
        const std::span<const uint8_t> data = vector.data;
        const Key whole = hmacSha256(vector.key, data);
        const Key parts = hmacSha256(vector.key, {data.first(7), {}, data.subspan(7)});
        CHECK(equal(std::span(whole).first(expected.size()), expected));
        CHECK(equal(std::span(parts).first(expected.size()), expected));
    }
}

int main() {
    return runTest("crypto", [] {
        // RFC 5869, test case 1.
        CHECK(equal(hkdf(Bytes(22, 0x0b), hex("000102030405060708090a0b0c"), hex("f0f1f2f3f4f5f6f7f8f9"), 42),
                    hex("3cb25f25faacd57a90434f64d0362f2a2d2d0a90cf1a5a4c5db02d56ecc4c5bf34007208d5b887185865")));
        hmacVectors();
        // NIST SP 800-38A F.5.5: CTR-AES256, four blocks; the counter carries
        // through its low bytes (...fdff -> ...fe00).
        Key key{};
        const auto keyBytes = hex("603deb1015ca71be2b73aef0857d77811f352c073b6108d72d9810a30914dff4");
        std::copy(keyBytes.begin(), keyBytes.end(), key.begin());
        const auto counter = hex("f0f1f2f3f4f5f6f7f8f9fafbfcfdfeff");
        const auto plain = hex("6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51"
                               "30c81c46a35ce411e5fbc1191a0a52eff69f2445df4f9b17ad2b417be66c3710");
        const auto expected = hex("601ec313775789a5b7a7f504bbf3d228f443e3ca4d62b59aca84e990cacaf5c5"
                                  "2b0930daa23de94ce87017ba2d84988ddfc9c58db67aada613c2dd08457941a6");
        const AesCtr ctr(key);
        Bytes cipher(plain.size());
        ctr.apply(std::span<const uint8_t, 16>(counter.data(), 16), plain, cipher);
        CHECK(equal(cipher, expected));
        Bytes partial(23); // Not a whole number of blocks.
        ctr.apply(std::span<const uint8_t, 16>(counter.data(), 16), std::span(plain).first(23), partial);
        CHECK(equal(partial, std::span(expected).first(23)));
        // The counter carries across all 128 bits: two blocks from ...0aff ffffffff
        // are the first block there and the second from ...0b00 00000000.
        const auto wrapping = hex("000102030405060708090affffffffff");
        const auto carried = hex("000102030405060708090b0000000000");
        Bytes both(32), first(16), second(16);
        ctr.apply(std::span<const uint8_t, 16>(wrapping.data(), 16), std::span(plain).first(32), both);
        ctr.apply(std::span<const uint8_t, 16>(wrapping.data(), 16), std::span(plain).first(16), first);
        ctr.apply(std::span<const uint8_t, 16>(carried.data(), 16), std::span(plain).subspan(16, 16), second);
        CHECK(equal(std::span(both).first(16), first) && equal(std::span(both).last(16), second));

        // ECDH P-256: both sides derive the same secret; invalid points are refused.
        Ecdh alice, bob;
        CHECK(alice.publicKey()[0] == 0x04 && !equal(alice.publicKey(), bob.publicKey()));
        CHECK(equal(alice.agree(bob.publicKey()), bob.agree(alice.publicKey())));
        Ecdh carol;
        CHECK(!equal(alice.agree(bob.publicKey()), alice.agree(carol.publicKey())));
        auto bad = Bytes(bob.publicKey().begin(), bob.publicKey().end());
        bad[0] = 0x05;
        CHECK(throws<rcsdk::Failure>([&] { alice.agree(bad); }));
        CHECK(throws<rcsdk::Failure>([&] { alice.agree(std::span(bob.publicKey()).first(33)); }));
        Bytes offCurve(65, 0x01);
        offCurve[0] = 0x04;
        CHECK(throws<rcsdk::Failure>([&] { alice.agree(offCurve); }));

        // SHA-256 (FIPS 180-4's examples: one block, two, a million "a"; and
        // nothing), SHA-1 (FIPS 180-2 "abc") and PBKDF2-HMAC-SHA256 (RFC 7914
        // section 11, first 32 bytes).
        const std::string abc = "abc";
        CHECK(equal(sha256({}), hex("e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")));
        CHECK(equal(sha256(Bytes(abc.begin(), abc.end())),
                    hex("ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")));
        const std::string twoBlocks = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        CHECK(equal(sha256(Bytes(twoBlocks.begin(), twoBlocks.end())),
                    hex("248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1")));
        CHECK(equal(sha256(Bytes(1000000, 'a')),
                    hex("cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0")));
        CHECK(equal(sha1(Bytes(abc.begin(), abc.end())), hex("a9993e364706816aba3e25717850c26c9cd0d89d")));
        const std::string salt = "salt", nacl = "NaCl";
        CHECK(equal(pbkdf2Sha256("passwd", Bytes(salt.begin(), salt.end()), 1),
                    hex("55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc")));
        CHECK(equal(pbkdf2Sha256("Password", Bytes(nacl.begin(), nacl.end()), 80000),
                    hex("4ddcd8f60b98be21830cee5ef22701f9641a4418d04c0414aeff08876b34ab56")));

        // ECDSA P-256 / SHA-256 against RFC 6979 A.2.5 ("sample"): the published
        // signature verifies; the key imports and yields its published public key.
        const auto publicBytes = hex("0460fed4ba255a9d31c961eb74c6356d68c049b8923b61fa6ce669622e60f29fb6"
                                     "7903fe1008b8bc99a41ae9e95628bc64f2f1b20c2d7e9f5177a3c294d4462299");
        PublicKey rfcPublic{};
        std::copy(publicBytes.begin(), publicBytes.end(), rfcPublic.begin());
        const auto signatureBytes = hex("efd48b2aacb6a8fd1140dd9cd45e81d69d2c877b56aaf991c34d0ea84eaf3716"
                                        "f7cb1c942d657c41d436c7a1b6e29f65f3e900dbb9aff4064dc4ab2f843acda8");
        Signature rfcSignature{};
        std::copy(signatureBytes.begin(), signatureBytes.end(), rfcSignature.begin());
        const std::string sample = "sample";
        const Bytes sampleBytes(sample.begin(), sample.end());
        CHECK(validPublicKey(rfcPublic) && verify(rfcPublic, sampleBytes, rfcSignature));
        CHECK(!verify(rfcPublic, hex("00"), rfcSignature));
        auto forged = rfcSignature;
        forged[63] ^= 1;
        CHECK(!verify(rfcPublic, sampleBytes, forged));
        auto rfcKey = publicBytes;
        const auto scalar = hex("c9afa9d845ba75166b5c215767b1d6934e50c3db36e89b127b8a622b120f6721");
        rfcKey.insert(rfcKey.end(), scalar.begin(), scalar.end());
        const DeviceKey imported(rfcKey);
        CHECK(equal(imported.publicKey(), rfcPublic) && verify(rfcPublic, sampleBytes, imported.sign(sampleBytes)));
        // Generated keys sign what their public half verifies; a scalar with
        // another key's public half, or a key of the wrong size, is refused.
        const auto generated = DeviceKey::generate();
        const DeviceKey own(generated);
        CHECK(generated[0] == 0x04 && !equal(own.publicKey(), rfcPublic));
        CHECK(verify(own.publicKey(), sampleBytes, own.sign(sampleBytes)));
        CHECK(!verify(rfcPublic, sampleBytes, own.sign(sampleBytes)));
        auto mismatched = rfcKey;
        std::copy(generated.end() - 32, generated.end(), mismatched.end() - 32);
        CHECK(throws<rcsdk::Failure>([&] { DeviceKey invalid(mismatched); }));
        CHECK(throws<rcsdk::Failure>([&] { DeviceKey invalid{std::span(generated).first(65)}; }));
        CHECK(!validPublicKey(offCurve) && !validPublicKey(std::span(rfcPublic).first(33)));

        // AEAD: round trip; any change to the sequence, associated data or bytes is detected.
        Key macKey{};
        random(macKey);
        const Aead aead(key, macKey);
        const Bytes header = hex("0102030405"), message = plain;
        const auto sealed = aead.seal(7, header, message);
        CHECK(sealed.size() == message.size() + Aead::tagSize);
        CHECK(!equal(std::span(sealed).first(message.size()), message));
        const auto opened = aead.open(7, header, sealed);
        CHECK(opened && equal(*opened, message));
        CHECK(!aead.open(8, header, sealed));
        CHECK(!aead.open(7, hex("0102030406"), sealed));
        for (const size_t at : {size_t(0), message.size() - 1, sealed.size() - 1}) {
            auto tampered = sealed;
            tampered[at] ^= 0x80;
            CHECK(!aead.open(7, header, tampered));
        }
        CHECK(!aead.open(7, header, std::span(sealed).first(10)));
        CHECK(aead.seal(1, {}, {}).size() == Aead::tagSize && aead.open(1, {}, aead.seal(1, {}, {})));
        // Different sequences never reuse keystream.
        CHECK(!equal(aead.seal(1, {}, message), aead.seal(2, {}, message)));

        cpaceVectors();

        Key random1{}, random2{};
        random(random1);
        random(random2);
        CHECK(!equal(random1, random2));
        CHECK(!equal(hex("00"), hex("0000")));
    });
}
