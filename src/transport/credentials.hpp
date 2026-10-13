// How this end proves itself in a handshake and what it accepts from the peer
// (rcs_identity): a shared secret (the connection key, a password, a
// PIN, or none) and optionally a device key, with the peer's device keys this
// end trusts.
#pragma once

#include "crypto/crypto.hpp"
#include "util/api.hpp"
#include "util/util.hpp"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace rcsdk::transport {

// The key a password derives: PBKDF2-SHA256 with a salt the sharer picks per
// run. Derived once per run on the sharer and once per sharer run on the
// viewer (it costs a few hundred milliseconds by design).
class PasswordKeys {
public:
    static constexpr uint32_t iterations = 600'000;
    using Salt = std::array<uint8_t, 16>;

    PasswordKeys(); // Picks this run's salt.
    const Salt& salt() const noexcept { return salt_; }
    crypto::Key derive(const std::string& password, const Salt& salt);
    // Whether derive() has this key at hand (no PBKDF2 to run).
    bool derived(const std::string& password, const Salt& salt);

private:
    Salt salt_{};
    std::mutex mutex_;
    std::string password_; // The last derivation's inputs and result.
    Salt derivedSalt_{};
    crypto::Key key_{};
    bool derived_ = false;
};

// The PIN handshakes a server allows (rcs_identity.pin_attempts). An
// attempt is held from the hello to the handshake's end. It is a failure when
// the server answered, which lets the client test one guess, and the client
// did not prove the PIN by the end; a hello refused before an answer costs
// nothing. While the failures and the attempts under way add up to the limit,
// PIN hellos are refused (PinLocked), until the host sets the PIN again (reset).
class PinAttempts {
public:
    // Each failure: the failures since the last reset and the attempts left.
    // Called without the lock held.
    using Report = std::function<void(uint32_t failures, uint32_t left)>;

    PinAttempts(uint32_t limit, Report report) : limit_(limit), report_(std::move(report)) {}
    // Starts counting afresh with `limit`; attempts under way no longer count.
    void reset(uint32_t limit);

    // One handshake's attempt: a failure once answered, unless it was proven
    // before it ends.
    class Attempt {
    public:
        Attempt(std::shared_ptr<PinAttempts> owner, uint64_t generation)
            : owner_(std::move(owner)), generation_(generation) {}
        ~Attempt();
        Attempt(const Attempt&) = delete;
        Attempt& operator=(const Attempt&) = delete;
        void answered() noexcept { answered_ = true; }
        void proven() noexcept { proven_ = true; }

    private:
        std::shared_ptr<PinAttempts> owner_;
        uint64_t generation_;
        bool answered_ = false, proven_ = false;
    };
    // Throws PinLocked when no attempt is left.
    static std::unique_ptr<Attempt> take(const std::shared_ptr<PinAttempts>& attempts);

private:
    void end(uint64_t generation, bool failed);

    std::mutex mutex_;
    uint32_t limit_, failures_ = 0, underWay_ = 0;
    uint64_t generation_ = 0; // Attempts of an earlier one end uncounted.
    Report report_;
};

struct Credentials {
    rcs_secret secret = RCS_SECRET_KEY;
    crypto::Key key{};                          // KEY
    std::string password;                       // PASSWORD, PIN
    std::shared_ptr<PasswordKeys> passwordKeys; // PASSWORD: this run's derivations
    std::shared_ptr<PinAttempts> pinAttempts;   // PIN, on a server
    std::shared_ptr<const crypto::DeviceKey> device; // Proves this end; null: none.
    std::vector<crypto::PublicKey> trusted;     // The peer must prove one; empty: any, or none.
    bool forwarder = false; // A forwarder's hop: the handshake's magic is RCSFWD (handshake.hpp).

    static Credentials fromKey(std::span<const uint8_t, 32> key);
};

// The credentials as they are when a handshake starts: a server that listens
// for long (a sharer serving viewers while it waits for more) takes the host's
// latest for each. A handshake keeps the ones it started with.
using CredentialsSource = std::function<std::shared_ptr<const Credentials>()>;
// Always `credentials`, which must outlive every handshake.
CredentialsSource fixedCredentials(const Credentials& credentials);

// Short hex of a public key's SHA-256, for messages and logs.
std::string fingerprint(const crypto::PublicKey& key);

// The peer proved no device key this end trusts: another one, or none.
class UntrustedPeer : public Failure {
public:
    UntrustedPeer(const std::string& message, const std::optional<crypto::PublicKey>& key)
        : Failure(RCS_ERROR_AUTHENTICATION, message), key_(key) {}
    // The key it proved, if any.
    const std::optional<crypto::PublicKey>& key() const noexcept { return key_; }

private:
    std::optional<crypto::PublicKey> key_;
};

// A PIN hello this end refuses: no attempt is left (PinAttempts).
class PinLocked : public Failure {
public:
    PinLocked()
        : Failure(RCS_ERROR_AUTHENTICATION, "Refused a PIN handshake: the failed ones reached the limit "
                                            "(rcs_identity.pin_attempts) until the PIN is set again") {}
};

} // namespace rcsdk::transport
