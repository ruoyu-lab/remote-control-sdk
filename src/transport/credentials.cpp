#include "transport/credentials.hpp"

#include "util/bytes.hpp"

#include <algorithm>

namespace rcsdk::transport {

PasswordKeys::PasswordKeys() {
    crypto::random(salt_);
}

crypto::Key PasswordKeys::derive(const std::string& password, const Salt& salt) {
    std::lock_guard lock(mutex_);
    if (!derived_ || password != password_ || salt != derivedSalt_) {
        key_ = crypto::pbkdf2Sha256(password, salt, iterations);
        password_ = password;
        derivedSalt_ = salt;
        derived_ = true;
    }
    return key_;
}

bool PasswordKeys::derived(const std::string& password, const Salt& salt) {
    std::lock_guard lock(mutex_);
    return derived_ && password == password_ && salt == derivedSalt_;
}

void PinAttempts::reset(uint32_t limit) {
    std::lock_guard lock(mutex_);
    limit_ = limit;
    failures_ = underWay_ = 0;
    ++generation_;
}

std::unique_ptr<PinAttempts::Attempt> PinAttempts::take(const std::shared_ptr<PinAttempts>& attempts) {
    std::lock_guard lock(attempts->mutex_);
    if (attempts->failures_ + attempts->underWay_ >= attempts->limit_) {
        throw PinLocked();
    }
    ++attempts->underWay_;
    return std::make_unique<Attempt>(attempts, attempts->generation_);
}

void PinAttempts::end(uint64_t generation, bool failed) {
    uint32_t failures, left;
    {
        std::lock_guard lock(mutex_);
        if (generation != generation_) {
            return;
        }
        --underWay_;
        if (!failed) {
            return;
        }
        failures = ++failures_;
        left = limit_ > failures_ ? limit_ - failures_ : 0;
    }
    if (report_) {
        report_(failures, left);
    }
}

PinAttempts::Attempt::~Attempt() {
    owner_->end(generation_, answered_ && !proven_);
}

Credentials Credentials::fromKey(std::span<const uint8_t, 32> key) {
    Credentials credentials;
    std::copy(key.begin(), key.end(), credentials.key.begin());
    return credentials;
}

CredentialsSource fixedCredentials(const Credentials& credentials) {
    return [&credentials] {
        return std::shared_ptr<const Credentials>(std::shared_ptr<void>(), &credentials);
    };
}

std::string fingerprint(const crypto::PublicKey& key) {
    const auto hash = crypto::sha256(key);
    return hex(std::span(hash).first(8));
}

} // namespace rcsdk::transport
