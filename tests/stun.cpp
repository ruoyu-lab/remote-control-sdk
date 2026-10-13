// STUN against RFC 5769's sample messages, and our own encoding.
#include "support/check.hpp"
#include "transport/stun.hpp"

#include <string>

using namespace rcsdk;
using namespace rcsdk::transport;

static crypto::Bytes hex(const std::string& text) {
    crypto::Bytes bytes;
    for (size_t i = 0; i < text.size(); i += 2) {
        if (text[i] != ' ') {
            bytes.push_back(uint8_t(std::stoul(text.substr(i, 2), nullptr, 16)));
        } else {
            --i;
        }
    }
    return bytes;
}

static crypto::Bytes text(const std::string& value) {
    return crypto::Bytes(value.begin(), value.end());
}

int main() {
    return runTest("stun", [] {
        const auto password = text("VOkJxbRl1RmTxUk/WvJxBt");

        // RFC 5769 2.1: a Binding request with ICE attributes.
        const auto request = hex("000100582112a442b7e7a701bc34d686fa87dfae802200105354554e20746573742063"
                                 "6c69656e74002400046e0001ff80290008932ff9b151263b36000600096576746a3a68"
                                 "36765920202000080014 9aeaa70cbfd8cb56781ef2b5b2d3f249c1b571a2 80280004e57a3bcf");
        const auto parsed = stun::parse(request);
        CHECK(parsed && parsed->method == stun::binding && parsed->klass == stun::request);
        CHECK(parsed->string(stun::username) == "evtj:h6vY" && parsed->u32(stun::priority) == 0x6e0001ffu);
        CHECK(parsed->u64(stun::iceControlled) == 0x932ff9b151263b36ull);
        CHECK(stun::verifyIntegrity(request, password) && stun::verifyFingerprint(request));
        CHECK(!stun::verifyIntegrity(request, text("wrong")));
        auto tampered = request;
        tampered[30] ^= 1;
        CHECK(!stun::verifyIntegrity(tampered, password) && !stun::verifyFingerprint(tampered));

        // 2.2 and 2.3: responses with XOR-MAPPED-ADDRESS, IPv4 and IPv6.
        const auto v4 = hex("010100 3c2112a442b7e7a701bc34d686fa87dfae8022000b7465737420766563746f7220"
                            "002000080001a147e112a643000800142b91f599fd9e90c38c7489f92af9ba53f06be7d7"
                            "80280004c07d4c96");
        const auto response = stun::parse(v4);
        CHECK(response && response->klass == stun::success);
        CHECK(response->address(stun::xorMappedAddress)->toString() == "192.0.2.1:32853");
        CHECK(stun::verifyIntegrity(v4, password) && stun::verifyFingerprint(v4));
        const auto v6 = hex("010100482112a442b7e7a701bc34d686fa87dfae8022000b7465737420766563746f7220"
                            "002000140002a1470113a9faa5d3f179bc25f4b5bed2b9d900080014a382954e4be67bf1"
                            "1784c97c8292c275bfe3ed4180280004c8fb0b4c");
        const auto response6 = stun::parse(v6);
        CHECK(response6 && stun::verifyIntegrity(v6, password) && stun::verifyFingerprint(v6));
        CHECK(response6->address(stun::xorMappedAddress)->toString() == "[2001:db8:1234:5678:11:2233:4455:6677]:32853");

        // Long-term credentials (TURN): key = MD5("user:realm:pass"); RFC 1321 MD5 of "abc".
        const auto expected = crypto::md5(text("user:realm:pass"));
        CHECK(stun::longTermKey("user", "realm", "pass") == crypto::Bytes(expected.begin(), expected.end()));
        const auto abc = crypto::md5(text("abc"));
        CHECK(crypto::Bytes(abc.begin(), abc.end()) == hex("900150983cd24fb0d6963f7d28e17f72"));

        // Our own messages round-trip, attributes padded, integrity and fingerprint verified.
        stun::Message message;
        message.method = stun::binding;
        message.klass = stun::request;
        message.transaction = stun::newTransaction();
        message.addString(stun::username, "abc:defgh");
        message.addU32(stun::priority, 12345);
        message.add(stun::useCandidate, {});
        message.addAddress(stun::xorPeerAddress, Endpoint::parse("10.1.2.3", 4567));
        message.addAddress(stun::xorMappedAddress, Endpoint::parse("2001:db8::7", 80));
        const auto key = text("secret");
        const auto bytes = stun::encode(message, key);
        CHECK(bytes.size() % 4 == 0 && stun::looksLikeStun(bytes));
        CHECK(stun::verifyIntegrity(bytes, key) && stun::verifyFingerprint(bytes));
        const auto back = stun::parse(bytes);
        CHECK(back && back->transaction == message.transaction && back->has(stun::useCandidate));
        CHECK(back->string(stun::username) == "abc:defgh" && back->u32(stun::priority) == 12345u);
        CHECK(back->address(stun::xorPeerAddress)->toString() == "10.1.2.3:4567");
        CHECK(back->address(stun::xorMappedAddress)->toString() == "[2001:db8::7]:80");
        // Attributes appended after MESSAGE-INTEGRITY still pass the integrity
        // check, so they are ignored: here a forged USE-CANDIDATE.
        message.attributes.clear();
        message.method = stun::binding;
        message.addString(stun::username, "abc:defgh");
        auto forged = stun::encode(message, key, false);
        forged.insert(forged.end(), {0x00, 0x25, 0x00, 0x00});
        forged[3] = uint8_t(forged.size() - stun::headerSize);
        const auto appended = stun::parse(forged);
        CHECK(stun::verifyIntegrity(forged, key) && appended && !appended->has(stun::useCandidate));
        CHECK(appended->string(stun::username) == "abc:defgh");
        // Methods beyond binding encode their bits correctly.
        message.method = stun::channelBind;
        message.klass = stun::error;
        const auto channel = stun::parse(stun::encode(message));
        CHECK(channel && channel->method == stun::channelBind && channel->klass == stun::error);

        CHECK(!stun::parse(text("not stun at all, really")));
        CHECK(stun::crc32(text("123456789")) == 0xCBF43926u);

        // FINGERPRINT, when there, is the last attribute and matches: a
        // corrupted message, or one with more after it, is not one.
        CHECK(!stun::parse(tampered));
        auto trailing = request;
        trailing.insert(trailing.end(), {0x00, 0x25, 0x00, 0x00});
        trailing[3] = uint8_t(trailing.size() - stun::headerSize);
        CHECK(stun::looksLikeStun(trailing) && !stun::parse(trailing));

        // ICE checks: a Binding request with FINGERPRINT, for this end's ufrag,
        // with integrity under its password; the answer tells the sender where
        // it came from.
        stun::Message check;
        check.method = stun::binding;
        check.klass = stun::request;
        check.transaction = stun::newTransaction();
        check.addString(stun::username, "local:remote");
        const auto ice = text("an ICE password of 22+");
        const auto checkBytes = stun::encode(check, ice);
        CHECK(stun::parseCheck(checkBytes, "local", ice));
        CHECK(!stun::parseCheck(checkBytes, "remote", ice) && !stun::parseCheck(checkBytes, "local", key));
        CHECK(!stun::parseCheck(stun::encode(check, ice, false), "local", ice));
        const auto sender = Endpoint::parse("2001:db8::3", 4000);
        const auto answerBytes = stun::answerCheck(check, sender, ice);
        const auto answer = stun::parse(answerBytes);
        CHECK(answer && answer->klass == stun::success && answer->transaction == check.transaction &&
              answer->has(stun::fingerprint) && stun::verifyIntegrity(answerBytes, ice));
        CHECK(answer->address(stun::xorMappedAddress) == sender);

        // A new transaction ID masks XOR addresses anew (an IPv6 mask includes it).
        stun::Message renewed;
        renewed.method = stun::createPermission;
        renewed.klass = stun::request;
        renewed.transaction = stun::newTransaction();
        renewed.addAddress(stun::xorPeerAddress, sender);
        renewed.setTransaction(stun::newTransaction());
        CHECK(stun::parse(stun::encode(renewed))->address(stun::xorPeerAddress) == sender);
    });
}
