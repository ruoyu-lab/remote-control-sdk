// crypto.hpp from the system (RCSDK_CRYPTO_BACKEND system): Security and
// CommonCrypto on macOS and iOS, CNG on Windows; elsewhere every call fails.
#include "crypto/crypto.hpp"

#include "util/util.hpp"

#include <algorithm>
#include <cstring>
#include <initializer_list>

#ifdef __APPLE__
#include <CommonCrypto/CommonCryptor.h>
#include <CommonCrypto/CommonDigest.h>
#include <CommonCrypto/CommonHMAC.h>
#include <CommonCrypto/CommonKeyDerivation.h>
#include <Security/Security.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#ifndef BCRYPT_KDF_RAW_SECRET
#define BCRYPT_KDF_RAW_SECRET L"TRUNCATE"
#endif
#endif

namespace rcsdk::crypto {
namespace {

[[noreturn]] void cryptoFailure() {
    fail(RCS_ERROR_INTERNAL, "System cryptography failed");
}

#ifdef _WIN32
void check(NTSTATUS status) {
    if (status < 0) {
        cryptoFailure();
    }
}

// Algorithm providers are expensive to open and safe to share between threads.
BCRYPT_ALG_HANDLE provider(LPCWSTR algorithm, ULONG flags) {
    BCRYPT_ALG_HANDLE handle = nullptr;
    check(BCryptOpenAlgorithmProvider(&handle, algorithm, nullptr, flags));
    return handle;
}
BCRYPT_ALG_HANDLE hmacProvider() {
    static const BCRYPT_ALG_HANDLE handle = provider(BCRYPT_SHA256_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    return handle;
}
BCRYPT_ALG_HANDLE sha1HmacProvider() {
    static const BCRYPT_ALG_HANDLE handle = provider(BCRYPT_SHA1_ALGORITHM, BCRYPT_ALG_HANDLE_HMAC_FLAG);
    return handle;
}
BCRYPT_ALG_HANDLE sha1Provider() {
    static const BCRYPT_ALG_HANDLE handle = provider(BCRYPT_SHA1_ALGORITHM, 0);
    return handle;
}
BCRYPT_ALG_HANDLE md5Provider() {
    static const BCRYPT_ALG_HANDLE handle = provider(BCRYPT_MD5_ALGORITHM, 0);
    return handle;
}
BCRYPT_ALG_HANDLE sha256Provider() {
    static const BCRYPT_ALG_HANDLE handle = provider(BCRYPT_SHA256_ALGORITHM, 0);
    return handle;
}
BCRYPT_ALG_HANDLE ecdsaProvider() {
    static const BCRYPT_ALG_HANDLE handle = provider(BCRYPT_ECDSA_P256_ALGORITHM, 0);
    return handle;
}

// Hash or HMAC with CNG over the concatenation of `parts`.
void digest(BCRYPT_ALG_HANDLE algorithm, std::span<const uint8_t> key,
            std::initializer_list<std::span<const uint8_t>> parts, std::span<uint8_t> output) {
    BCRYPT_HASH_HANDLE hash = nullptr;
    check(BCryptCreateHash(algorithm, &hash, nullptr, 0, const_cast<PUCHAR>(key.data()), ULONG(key.size()), 0));
    NTSTATUS status = 0;
    for (const auto part : parts) {
        if (status >= 0 && !part.empty()) {
            status = BCryptHashData(hash, const_cast<PUCHAR>(part.data()), ULONG(part.size()), 0);
        }
    }
    if (status >= 0) {
        status = BCryptFinishHash(hash, output.data(), ULONG(output.size()), 0);
    }
    BCryptDestroyHash(hash);
    check(status);
}
BCRYPT_ALG_HANDLE aesEcbProvider() {
    static const BCRYPT_ALG_HANDLE handle = [] {
        BCRYPT_ALG_HANDLE aes = provider(BCRYPT_AES_ALGORITHM, 0);
        check(BCryptSetProperty(aes, BCRYPT_CHAINING_MODE,
                                reinterpret_cast<PUCHAR>(const_cast<wchar_t*>(BCRYPT_CHAIN_MODE_ECB)),
                                sizeof(BCRYPT_CHAIN_MODE_ECB), 0));
        return aes;
    }();
    return handle;
}
BCRYPT_ALG_HANDLE ecdhProvider() {
    static const BCRYPT_ALG_HANDLE handle = provider(BCRYPT_ECDH_P256_ALGORITHM, 0);
    return handle;
}

struct KeyHandle {
    BCRYPT_KEY_HANDLE value = nullptr;
    ~KeyHandle() {
        if (value) {
            BCryptDestroyKey(value);
        }
    }
};
#endif

#ifdef __APPLE__
template <class T>
struct CFOwned {
    T value = nullptr;
    ~CFOwned() {
        if (value) {
            CFRelease(value);
        }
    }
};

CFOwned<CFMutableDictionaryRef> ecAttributes(bool publicKey, bool privateKey = false) {
    CFOwned<CFMutableDictionaryRef> attributes{
        CFDictionaryCreateMutable(nullptr, 0, &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks)};
    const int bits = 256;
    CFOwned<CFNumberRef> size{CFNumberCreate(nullptr, kCFNumberIntType, &bits)};
    CFDictionarySetValue(attributes.value, kSecAttrKeyType, kSecAttrKeyTypeECSECPrimeRandom);
    CFDictionarySetValue(attributes.value, kSecAttrKeySizeInBits, size.value);
    if (publicKey) {
        CFDictionarySetValue(attributes.value, kSecAttrKeyClass, kSecAttrKeyClassPublic);
    } else if (privateKey) {
        CFDictionarySetValue(attributes.value, kSecAttrKeyClass, kSecAttrKeyClassPrivate);
    }
    return attributes;
}

// A key from its external representation (X9.63), or null.
SecKeyRef importKey(std::span<const uint8_t> bytes, bool publicKey) {
    CFOwned<CFDataRef> data{CFDataCreate(nullptr, bytes.data(), CFIndex(bytes.size()))};
    const auto attributes = ecAttributes(publicKey, !publicKey);
    return SecKeyCreateWithData(data.value, attributes.value, nullptr);
}

// Security.framework speaks X9.62 DER signatures (SEQUENCE of two INTEGERs);
// the wire carries r || s.
Bytes derSignature(const Signature& raw) {
    const auto integer = [](std::span<const uint8_t> value) {
        size_t start = 0;
        while (start + 1 < value.size() && value[start] == 0) {
            ++start;
        }
        Bytes out{0x02};
        const bool pad = value[start] & 0x80;
        out.push_back(uint8_t(value.size() - start + (pad ? 1 : 0)));
        if (pad) {
            out.push_back(0);
        }
        out.insert(out.end(), value.begin() + ptrdiff_t(start), value.end());
        return out;
    };
    const Bytes r = integer(std::span(raw).first(32)), s = integer(std::span(raw).last(32));
    Bytes der{0x30, uint8_t(r.size() + s.size())};
    der.insert(der.end(), r.begin(), r.end());
    der.insert(der.end(), s.begin(), s.end());
    return der;
}

std::optional<Signature> rawSignature(std::span<const uint8_t> der) {
    if (der.size() < 8 || der[0] != 0x30 || der[1] != der.size() - 2) {
        return std::nullopt;
    }
    Signature raw{};
    size_t offset = 2;
    for (size_t half = 0; half < 2; ++half) {
        if (offset + 2 > der.size() || der[offset] != 0x02) {
            return std::nullopt;
        }
        size_t length = der[offset + 1];
        offset += 2;
        if (offset + length > der.size()) {
            return std::nullopt;
        }
        auto value = der.subspan(offset, length);
        offset += length;
        while (!value.empty() && value[0] == 0) {
            value = value.subspan(1);
        }
        if (value.size() > 32) {
            return std::nullopt;
        }
        std::copy(value.begin(), value.end(), raw.begin() + ptrdiff_t(half * 32 + 32 - value.size()));
    }
    return offset == der.size() ? std::optional(raw) : std::nullopt;
}
#endif

} // namespace

void random(std::span<uint8_t> output) {
#ifdef __APPLE__
    if (SecRandomCopyBytes(kSecRandomDefault, output.size(), output.data()) != errSecSuccess) {
        cryptoFailure();
    }
#elif defined(_WIN32)
    check(BCryptGenRandom(nullptr, output.data(), ULONG(output.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG));
#else
    (void)output;
    cryptoFailure();
#endif
}

Key sha256(std::span<const uint8_t> message) {
    Key output{};
#ifdef __APPLE__
    CC_SHA256(message.data(), CC_LONG(message.size()), output.data());
#elif defined(_WIN32)
    digest(sha256Provider(), {}, {message}, output);
#else
    (void)message;
    cryptoFailure();
#endif
    return output;
}

Key pbkdf2Sha256(std::string_view password, std::span<const uint8_t> salt, uint32_t iterations) {
    Key output{};
#ifdef __APPLE__
    if (CCKeyDerivationPBKDF(kCCPBKDF2, password.data(), password.size(), salt.data(), salt.size(),
                             kCCPRFHmacAlgSHA256, iterations, output.data(), output.size()) != kCCSuccess) {
        cryptoFailure();
    }
#elif defined(_WIN32)
    check(BCryptDeriveKeyPBKDF2(hmacProvider(), reinterpret_cast<PUCHAR>(const_cast<char*>(password.data())),
                                ULONG(password.size()), const_cast<PUCHAR>(salt.data()), ULONG(salt.size()),
                                iterations, output.data(), ULONG(output.size()), 0));
#else
    (void)password;
    (void)salt;
    (void)iterations;
    cryptoFailure();
#endif
    return output;
}

Key hmacSha256(std::span<const uint8_t> key, std::span<const uint8_t> message) {
    return hmacSha256(key, {message});
}

Key hmacSha256(std::span<const uint8_t> key, std::initializer_list<std::span<const uint8_t>> parts) {
    Key output{};
#ifdef __APPLE__
    CCHmacContext context;
    CCHmacInit(&context, kCCHmacAlgSHA256, key.data(), key.size());
    for (const auto part : parts) {
        CCHmacUpdate(&context, part.data(), part.size());
    }
    CCHmacFinal(&context, output.data());
#elif defined(_WIN32)
    digest(hmacProvider(), key, parts, output);
#else
    (void)key;
    (void)parts;
    cryptoFailure();
#endif
    return output;
}

std::array<uint8_t, 20> hmacSha1(std::span<const uint8_t> key, std::span<const uint8_t> message) {
    std::array<uint8_t, 20> output{};
#ifdef __APPLE__
    CCHmac(kCCHmacAlgSHA1, key.data(), key.size(), message.data(), message.size(), output.data());
#elif defined(_WIN32)
    digest(sha1HmacProvider(), key, {message}, output);
#else
    (void)key;
    (void)message;
    cryptoFailure();
#endif
    return output;
}

std::array<uint8_t, 20> sha1(std::span<const uint8_t> message) {
    std::array<uint8_t, 20> output{};
#ifdef __APPLE__
    CC_SHA1(message.data(), CC_LONG(message.size()), output.data());
#elif defined(_WIN32)
    digest(sha1Provider(), {}, {message}, output);
#else
    (void)message;
    cryptoFailure();
#endif
    return output;
}

std::array<uint8_t, 16> md5(std::span<const uint8_t> message) {
    std::array<uint8_t, 16> output{};
#ifdef __APPLE__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations" // Required by TURN, not used for security alone.
    CC_MD5(message.data(), CC_LONG(message.size()), output.data());
#pragma clang diagnostic pop
#elif defined(_WIN32)
    digest(md5Provider(), {}, {message}, output);
#else
    (void)message;
    cryptoFailure();
#endif
    return output;
}

// ---- ECDH P-256 ----------------------------------------------------------------

struct Ecdh::Impl {
#ifdef __APPLE__
    CFOwned<SecKeyRef> privateKey;
#elif defined(_WIN32)
    KeyHandle pair;
#endif
};

Ecdh::Ecdh() : impl_(std::make_unique<Impl>()) {
#ifdef __APPLE__
    const auto attributes = ecAttributes(false);
    impl_->privateKey.value = SecKeyCreateRandomKey(attributes.value, nullptr);
    if (!impl_->privateKey.value) {
        cryptoFailure();
    }
    CFOwned<SecKeyRef> publicKey{SecKeyCopyPublicKey(impl_->privateKey.value)};
    CFOwned<CFDataRef> data{publicKey.value ? SecKeyCopyExternalRepresentation(publicKey.value, nullptr) : nullptr};
    if (!data.value || CFDataGetLength(data.value) != CFIndex(publicKeySize)) {
        cryptoFailure();
    }
    std::memcpy(publicKey_.data(), CFDataGetBytePtr(data.value), publicKeySize);
#elif defined(_WIN32)
    check(BCryptGenerateKeyPair(ecdhProvider(), &impl_->pair.value, 256, 0));
    check(BCryptFinalizeKeyPair(impl_->pair.value, 0));
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 64> blob{};
    ULONG written = 0;
    check(BCryptExportKey(impl_->pair.value, nullptr, BCRYPT_ECCPUBLIC_BLOB, blob.data(), ULONG(blob.size()), &written,
                          0));
    if (written != blob.size()) {
        cryptoFailure();
    }
    publicKey_[0] = 0x04;
    std::memcpy(publicKey_.data() + 1, blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 64);
#else
    cryptoFailure();
#endif
}

Ecdh::~Ecdh() = default;

Key Ecdh::agree(std::span<const uint8_t> peer) const {
    if (peer.size() != publicKeySize || peer[0] != 0x04) {
        fail(RCS_ERROR_AUTHENTICATION, "The peer's key exchange value is malformed");
    }
    Key shared{};
#ifdef __APPLE__
    CFOwned<CFDataRef> data{CFDataCreate(nullptr, peer.data(), CFIndex(peer.size()))};
    const auto attributes = ecAttributes(true);
    CFOwned<SecKeyRef> peerKey{SecKeyCreateWithData(data.value, attributes.value, nullptr)};
    if (!peerKey.value) {
        fail(RCS_ERROR_AUTHENTICATION, "The peer's key exchange value is not a P-256 point");
    }
    CFOwned<CFDictionaryRef> parameters{CFDictionaryCreate(nullptr, nullptr, nullptr, 0, &kCFTypeDictionaryKeyCallBacks,
                                                           &kCFTypeDictionaryValueCallBacks)};
    CFOwned<CFDataRef> secret{SecKeyCopyKeyExchangeResult(
        impl_->privateKey.value, kSecKeyAlgorithmECDHKeyExchangeStandard, peerKey.value, parameters.value, nullptr)};
    if (!secret.value || CFDataGetLength(secret.value) != CFIndex(shared.size())) {
        fail(RCS_ERROR_AUTHENTICATION, "Key agreement with the peer failed");
    }
    std::memcpy(shared.data(), CFDataGetBytePtr(secret.value), shared.size());
#elif defined(_WIN32)
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 64> blob{};
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDH_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), peer.data() + 1, 64);
    KeyHandle peerKey;
    if (BCryptImportKeyPair(ecdhProvider(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &peerKey.value, blob.data(),
                            ULONG(blob.size()), 0) < 0) {
        fail(RCS_ERROR_AUTHENTICATION, "The peer's key exchange value is not a P-256 point");
    }
    BCRYPT_SECRET_HANDLE secret = nullptr;
    if (BCryptSecretAgreement(impl_->pair.value, peerKey.value, &secret, 0) < 0) {
        fail(RCS_ERROR_AUTHENTICATION, "Key agreement with the peer failed");
    }
    ULONG written = 0;
    const NTSTATUS status =
        BCryptDeriveKey(secret, BCRYPT_KDF_RAW_SECRET, nullptr, shared.data(), ULONG(shared.size()), &written, 0);
    BCryptDestroySecret(secret);
    if (status < 0 || written != shared.size()) {
        fail(RCS_ERROR_AUTHENTICATION, "Key agreement with the peer failed");
    }
    std::reverse(shared.begin(), shared.end()); // CNG returns the raw secret little-endian.
#else
    cryptoFailure();
#endif
    return shared;
}

// ---- ECDSA P-256 ----------------------------------------------------------------

#ifdef _WIN32
namespace {
// A CNG key blob: header, then X, Y (and d for a private key).
template <size_t Parts>
std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 32 * Parts> eccBlob(ULONG magic, std::span<const uint8_t> parts) {
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 32 * Parts> blob{};
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = magic;
    header->cbKey = 32;
    std::memcpy(blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), parts.data(), 32 * Parts);
    return blob;
}

bool importPublic(std::span<const uint8_t> key, KeyHandle& handle) {
    if (key.size() != 65 || key[0] != 0x04) {
        return false;
    }
    auto blob = eccBlob<2>(BCRYPT_ECDSA_PUBLIC_P256_MAGIC, key.subspan(1));
    return BCryptImportKeyPair(ecdsaProvider(), nullptr, BCRYPT_ECCPUBLIC_BLOB, &handle.value, blob.data(),
                               ULONG(blob.size()), 0) >= 0;
}
} // namespace
#endif

bool validPublicKey(std::span<const uint8_t> key) noexcept {
    if (key.size() != 65 || key[0] != 0x04) {
        return false;
    }
#ifdef __APPLE__
    CFOwned<SecKeyRef> imported{importKey(key, true)};
    return imported.value != nullptr;
#elif defined(_WIN32)
    KeyHandle handle;
    return importPublic(key, handle);
#else
    return false;
#endif
}

bool verify(const PublicKey& key, std::span<const uint8_t> message, const Signature& signature) {
    const Key hash = sha256(message);
#ifdef __APPLE__
    CFOwned<SecKeyRef> imported{importKey(key, true)};
    if (!imported.value) {
        return false;
    }
    const Bytes der = derSignature(signature);
    CFOwned<CFDataRef> digestData{CFDataCreate(nullptr, hash.data(), CFIndex(hash.size()))};
    CFOwned<CFDataRef> signatureData{CFDataCreate(nullptr, der.data(), CFIndex(der.size()))};
    return SecKeyVerifySignature(imported.value, kSecKeyAlgorithmECDSASignatureDigestX962SHA256, digestData.value,
                                 signatureData.value, nullptr);
#elif defined(_WIN32)
    KeyHandle handle;
    if (!importPublic(key, handle)) {
        return false;
    }
    return BCryptVerifySignature(handle.value, nullptr, const_cast<PUCHAR>(hash.data()), ULONG(hash.size()),
                                 const_cast<PUCHAR>(signature.data()), ULONG(signature.size()), 0) >= 0;
#else
    (void)hash;
    (void)signature;
    return false;
#endif
}

struct DeviceKey::Impl {
#ifdef __APPLE__
    CFOwned<SecKeyRef> key;
#elif defined(_WIN32)
    KeyHandle key;
#endif
};

std::array<uint8_t, DeviceKey::size> DeviceKey::generate() {
    std::array<uint8_t, size> out{};
#ifdef __APPLE__
    const auto attributes = ecAttributes(false);
    CFOwned<SecKeyRef> key{SecKeyCreateRandomKey(attributes.value, nullptr)};
    CFOwned<CFDataRef> data{key.value ? SecKeyCopyExternalRepresentation(key.value, nullptr) : nullptr};
    if (!data.value || CFDataGetLength(data.value) != CFIndex(size)) {
        cryptoFailure();
    }
    std::memcpy(out.data(), CFDataGetBytePtr(data.value), size);
#elif defined(_WIN32)
    KeyHandle key;
    check(BCryptGenerateKeyPair(ecdsaProvider(), &key.value, 256, 0));
    check(BCryptFinalizeKeyPair(key.value, 0));
    std::array<uint8_t, sizeof(BCRYPT_ECCKEY_BLOB) + 96> blob{};
    ULONG written = 0;
    check(BCryptExportKey(key.value, nullptr, BCRYPT_ECCPRIVATE_BLOB, blob.data(), ULONG(blob.size()), &written, 0));
    if (written != blob.size()) {
        cryptoFailure();
    }
    out[0] = 0x04;
    std::memcpy(out.data() + 1, blob.data() + sizeof(BCRYPT_ECCKEY_BLOB), 96);
    SecureZeroMemory(blob.data(), blob.size());
#else
    cryptoFailure();
#endif
    return out;
}

DeviceKey::DeviceKey(std::span<const uint8_t> key) : impl_(std::make_unique<Impl>()) {
    const auto invalid = [] {
        fail(RCS_ERROR_INVALID_ARGUMENT, "The device key is not a P-256 key pair (see rcs_generate_device_key)");
    };
    if (key.size() != size || key[0] != 0x04) {
        invalid();
    }
#ifdef __APPLE__
    impl_->key.value = importKey(key, false);
    if (!impl_->key.value) {
        invalid();
    }
#elif defined(_WIN32)
    auto blob = eccBlob<3>(BCRYPT_ECDSA_PRIVATE_P256_MAGIC, key.subspan(1));
    const bool imported = BCryptImportKeyPair(ecdsaProvider(), nullptr, BCRYPT_ECCPRIVATE_BLOB, &impl_->key.value,
                                              blob.data(), ULONG(blob.size()), 0) >= 0;
    SecureZeroMemory(blob.data(), blob.size());
    if (!imported) {
        invalid();
    }
#else
    invalid();
#endif
    std::copy_n(key.begin(), publicKey_.size(), publicKey_.begin());
    // The system may take a private scalar with any public point: check that
    // they belong together, so the peer can verify what this key signs.
    constexpr std::string_view probe = "rcsdk device key";
    const auto bytes = std::span(reinterpret_cast<const uint8_t*>(probe.data()), probe.size());
    if (!verify(publicKey_, bytes, sign(bytes))) {
        invalid();
    }
}

DeviceKey::~DeviceKey() = default;

Signature DeviceKey::sign(std::span<const uint8_t> message) const {
    const Key hash = sha256(message);
    Signature signature{};
#ifdef __APPLE__
    CFOwned<CFDataRef> digestData{CFDataCreate(nullptr, hash.data(), CFIndex(hash.size()))};
    CFOwned<CFDataRef> der{SecKeyCreateSignature(impl_->key.value, kSecKeyAlgorithmECDSASignatureDigestX962SHA256,
                                                 digestData.value, nullptr)};
    const auto raw = der.value ? rawSignature(std::span(CFDataGetBytePtr(der.value), size_t(CFDataGetLength(der.value))))
                               : std::nullopt;
    if (!raw) {
        cryptoFailure();
    }
    signature = *raw;
#elif defined(_WIN32)
    ULONG written = 0;
    check(BCryptSignHash(impl_->key.value, nullptr, const_cast<PUCHAR>(hash.data()), ULONG(hash.size()),
                         signature.data(), ULONG(signature.size()), &written, 0));
    if (written != signature.size()) {
        cryptoFailure();
    }
#else
    (void)hash;
    cryptoFailure();
#endif
    return signature;
}

// ---- AES-256-CTR over the system's AES block function ---------------------------

struct AesCtr::Impl {
#ifdef __APPLE__
    CCCryptorRef ecb = nullptr;
    ~Impl() {
        if (ecb) {
            CCCryptorRelease(ecb);
        }
    }
#elif defined(_WIN32)
    KeyHandle key;
#endif
};

AesCtr::AesCtr(const Key& key) : impl_(std::make_unique<Impl>()) {
#ifdef __APPLE__
    if (CCCryptorCreate(kCCEncrypt, kCCAlgorithmAES, kCCOptionECBMode, key.data(), key.size(), nullptr, &impl_->ecb) !=
        kCCSuccess) {
        cryptoFailure();
    }
#elif defined(_WIN32)
    check(BCryptGenerateSymmetricKey(aesEcbProvider(), &impl_->key.value, nullptr, 0, const_cast<PUCHAR>(key.data()),
                                     ULONG(key.size()), 0));
#else
    (void)key;
    cryptoFailure();
#endif
}

AesCtr::~AesCtr() = default;

void AesCtr::apply(std::span<const uint8_t, 16> iv, std::span<const uint8_t> input, std::span<uint8_t> output) const {
    if (output.size() < input.size()) {
        fail(RCS_ERROR_INTERNAL, "AES-CTR output too small");
    }
    // A chunk of counter blocks at a time, encrypted in one call (ECB), then
    // XORed in: no allocation, whatever the message's size.
    std::array<uint8_t, 4096> stream;
    std::array<uint8_t, 16> counter{};
    std::copy(iv.begin(), iv.end(), counter.begin());
    for (size_t done = 0; done < input.size(); done += stream.size()) {
        const size_t length = std::min(stream.size(), input.size() - done);
        const size_t bytes = (length + 15) / 16 * 16;
        for (size_t at = 0; at < bytes; at += 16) {
            std::copy(counter.begin(), counter.end(), stream.begin() + ptrdiff_t(at));
            for (int i = 15; i >= 0 && ++counter[size_t(i)] == 0; --i) {
            }
        }
#ifdef __APPLE__
        size_t written = 0;
        if (CCCryptorUpdate(impl_->ecb, stream.data(), bytes, stream.data(), bytes, &written) != kCCSuccess ||
            written != bytes) {
            cryptoFailure();
        }
#elif defined(_WIN32)
        ULONG written = 0;
        check(BCryptEncrypt(impl_->key.value, stream.data(), ULONG(bytes), nullptr, nullptr, 0, stream.data(),
                            ULONG(bytes), &written, 0));
#endif
        for (size_t i = 0; i < length; ++i) {
            output[done + i] = input[done + i] ^ stream[i];
        }
    }
}

} // namespace rcsdk::crypto
