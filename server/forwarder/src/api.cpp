// rcsfwd.h: the forwarder's C interface over Forwarder.
#include "forwarder.hpp"
#include "token.hpp"

#include "crypto/crypto.hpp"
#include "transport/socket.hpp"

#include <algorithm>
#include <cstring>
#include <optional>
#include <set>
#include <string>

using rcsdk::Failure;
using rcsdk::forwarder::Forwarder;

struct rcsfwd_impl {
    std::unique_ptr<Forwarder> forwarder;
};

namespace {

thread_local std::string lastError;

rcs_result refuse(rcs_result result, std::string why) {
    lastError = std::move(why);
    return result;
}

// Runs `action`, its failure the thread's last error.
template <class Action>
rcs_result guarded(Action&& action) noexcept {
    try {
        lastError.clear();
        return action();
    } catch (const Failure& failure) {
        return refuse(failure.result(), failure.what());
    } catch (const std::exception& error) {
        return refuse(RCS_ERROR_INTERNAL, error.what());
    }
}

std::string roomName(const char* room) {
    if (!room) {
        rcsdk::fail(RCS_ERROR_INVALID_ARGUMENT, "room is required");
    }
    return std::string(room, strnlen(room, RCSFWD_ROOM_SIZE));
}

// "address:port" or "[v6]:port", numeric; none for an empty one.
std::optional<rcsdk::transport::Endpoint> listenAddress(const char* text, size_t capacity, const std::string& field) {
    const std::string value(text, strnlen(text, capacity));
    const std::string name = "rcsfwd_config." + field;
    if (value.size() == capacity) {
        rcsdk::fail(RCS_ERROR_INVALID_ARGUMENT, name + " is not NUL-terminated");
    }
    if (value.empty()) {
        return std::nullopt;
    }
    const auto colon = value.rfind(':');
    if (colon == std::string::npos || colon + 1 == value.size()) {
        rcsdk::fail(RCS_ERROR_INVALID_ARGUMENT, name + ": \"address:port\", numeric");
    }
    std::string host = value.substr(0, colon);
    if (host.size() >= 2 && host.front() == '[' && host.back() == ']') {
        host = host.substr(1, host.size() - 2);
    }
    const auto port = value.substr(colon + 1);
    if (port.find_first_not_of("0123456789") != std::string::npos || port.size() > 5 || std::stoul(port) > 65535) {
        rcsdk::fail(RCS_ERROR_INVALID_ARGUMENT, name + ": the port is 0..65535");
    }
    const auto endpoint = rcsdk::transport::Endpoint::tryParse(host, uint16_t(std::stoul(port)));
    if (!endpoint) {
        rcsdk::fail(RCS_ERROR_INVALID_ARGUMENT, name + ": \"" + host + "\" is not a numeric IPv4 or IPv6 address");
    }
    return endpoint;
}

// `value` where it is not 0, which must then be within `low`..`high`; `otherwise` for 0.
template <class T>
T within(T value, T low, T high, T otherwise, const std::string& field, const std::string& range) {
    if (value && (value < low || value > high)) {
        rcsdk::fail(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_config." + field + ": 0 or " + range);
    }
    return value ? value : otherwise;
}

rcsdk::forwarder::Settings settingsOf(const rcsfwd_config& c) {
    using rcsdk::fail;
    if (c.struct_size != sizeof(rcsfwd_config) || c.version != RCSDK_VERSION) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_config: struct_size and version are this library's (" +
                                             std::string(RCSDK_VERSION_STRING) +
                                             "); initialise it with RCSFWD_CONFIG_INIT from its headers");
    }
    rcsdk::forwarder::Settings s;
    s.listenUdp = listenAddress(c.listen_udp, sizeof(c.listen_udp), "listen_udp");
    s.listenTcp = listenAddress(c.listen_tcp, sizeof(c.listen_tcp), "listen_tcp");
    s.listenWebSocket = listenAddress(c.listen_websocket, sizeof(c.listen_websocket), "listen_websocket");
    if (!s.listenUdp && !s.listenTcp && !s.listenWebSocket) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_config: listen_udp, listen_tcp or listen_websocket is required");
    }
    std::copy(std::begin(c.identity_key), std::end(c.identity_key), s.identity.begin());
    try {
        rcsdk::crypto::DeviceKey check(s.identity);
    } catch (const Failure&) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_config.identity_key is no P-256 key pair (rcsfwd_generate_identity)");
    }
    if (c.verify_token ? c.secret_count != 0 : c.secret_count == 0 || c.secret_count > 4 || !c.secrets) {
        fail(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_config: 1 to 4 secrets, or verify_token instead");
    }
    std::set<uint32_t> ids;
    for (uint32_t i = 0; i < c.secret_count; ++i) {
        if (!ids.insert(c.secrets[i].key_id).second) {
            fail(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_config.secrets: key_id " + std::to_string(c.secrets[i].key_id) +
                                                 " is there twice");
        }
        s.secrets.push_back(c.secrets[i]);
    }
    s.verifyToken = c.verify_token;
    s.maxRooms = within<uint32_t>(c.max_rooms, 1, 65'536, 64, "max_rooms", "1..65 536");
    s.maxViewers =
        within<uint32_t>(c.max_viewers_per_room, 1, RCS_MAX_VIEWERS, 64, "max_viewers_per_room", "1..RCS_MAX_VIEWERS");
    s.maxEnds = within<uint32_t>(c.max_ends, 1, 65'536, 1024, "max_ends", "1..65 536");
    s.maxViewerBitrate = within<uint32_t>(c.max_viewer_bitrate_bps, 500'000, 1'000'000'000, 100'000'000,
                                          "max_viewer_bitrate_bps", "500 000..1 000 000 000");
    s.maxPipeBps =
        within<uint32_t>(c.max_pipe_bps, 64'000, 1'000'000'000, 50'000'000, "max_pipe_bps", "64 000..1 000 000 000");
    s.maxPublishBps = within<uint32_t>(c.max_publish_bitrate_bps, 1'000'000, 4'000'000'000u, 400'000'000,
                                       "max_publish_bitrate_bps", "1 000 000..4 000 000 000");
    s.cacheMs = within<uint32_t>(c.cache_ms, 100, 60'000, 3000, "cache_ms", "100..60 000");
    s.cacheBytes = within<uint32_t>(c.cache_bytes, 1u << 20, 1u << 30, 16u << 20, "cache_bytes", "1 MiB..1 GiB");
    s.maxCacheBytes =
        within<uint64_t>(c.max_cache_bytes, s.cacheBytes, uint64_t(64) << 30,
                         std::max<uint64_t>(256u << 20, s.cacheBytes), "max_cache_bytes", "cache_bytes..64 GiB");
    s.onEvent = c.on_event;
    s.userData = c.user_data;
    return s;
}

} // namespace

extern "C" {

rcs_result rcsfwd_generate_identity(uint8_t key[RCSFWD_IDENTITY_KEY_SIZE]) {
    if (!key) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "key is required");
    }
    return guarded([&]() -> rcs_result {
        const auto made = rcsdk::crypto::DeviceKey::generate();
        std::copy(made.begin(), made.end(), key);
        return RCS_OK;
    });
}

rcs_result rcsfwd_public_key_sha256(const uint8_t key[RCSFWD_IDENTITY_KEY_SIZE], uint8_t sha256[32]) {
    if (!key || !sha256) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "key and sha256 are required");
    }
    return guarded([&]() -> rcs_result {
        const rcsdk::crypto::DeviceKey pair(std::span(key, RCSFWD_IDENTITY_KEY_SIZE));
        const auto digest = rcsdk::crypto::sha256(pair.publicKey());
        std::copy(digest.begin(), digest.end(), sha256);
        return RCS_OK;
    });
}

rcs_result rcsfwd_sign_token(const rcsfwd_secret* secret, const rcsfwd_grant* grant, char* token, uint32_t capacity) {
    if (!secret || !grant || !token) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "secret, grant and token are required");
    }
    if (grant->struct_size != sizeof(rcsfwd_grant)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_grant: initialise it with RCSFWD_GRANT_INIT");
    }
    return guarded([&]() -> rcs_result {
        rcsdk::forwarder::Grant g;
        g.room.assign(grant->room, strnlen(grant->room, sizeof(grant->room)));
        if (g.room.size() == sizeof(grant->room)) {
            rcsdk::fail(RCS_ERROR_INVALID_ARGUMENT, "rcsfwd_grant.room is not NUL-terminated");
        }
        g.role = grant->role;
        g.route = grant->route;
        g.expires = grant->expires;
        g.maxBitrate = grant->max_bitrate_bps;
        std::copy(std::begin(grant->audience), std::end(grant->audience), g.audience.begin());
        const auto text = rcsdk::forwarder::sign(*secret, g);
        if (text.size() + 1 > capacity) {
            return refuse(RCS_ERROR_BUFFER_TOO_SMALL, "The token takes " + std::to_string(text.size() + 1) + " bytes");
        }
        std::memcpy(token, text.c_str(), text.size() + 1);
        return RCS_OK;
    });
}

rcs_result rcsfwd_create(const rcsfwd_config* config, rcsfwd* out) {
    if (!config || !out) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "config and out are required");
    }
    return guarded([&]() -> rcs_result {
        auto made = std::make_unique<rcsfwd_impl>();
        made->forwarder = std::make_unique<Forwarder>(settingsOf(*config));
        *out = made.release();
        return RCS_OK;
    });
}

rcs_result rcsfwd_start(rcsfwd forwarder) {
    if (!forwarder) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "forwarder is required");
    }
    return guarded([&]() -> rcs_result {
        forwarder->forwarder->start();
        return RCS_OK;
    });
}

rcs_result rcsfwd_stop(rcsfwd forwarder) {
    if (!forwarder) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "forwarder is required");
    }
    return guarded([&]() -> rcs_result {
        forwarder->forwarder->stop();
        return RCS_OK;
    });
}

void rcsfwd_destroy(rcsfwd forwarder) {
    delete forwarder;
}

rcs_result rcsfwd_get_port(rcsfwd forwarder, rcsfwd_entry entry, uint32_t* port) {
    if (!forwarder || !port || entry < RCSFWD_ENTRY_UDP || entry > RCSFWD_ENTRY_WEBSOCKET) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "forwarder, an entry (RCSFWD_ENTRY_*) and port are required");
    }
    return guarded([&]() -> rcs_result {
        *port = forwarder->forwarder->port(entry);
        return RCS_OK;
    });
}

rcs_result rcsfwd_get_stats(rcsfwd forwarder, rcsfwd_stats* stats) {
    if (!forwarder || !stats || stats->struct_size != sizeof(rcsfwd_stats)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "forwarder and stats (RCSFWD_STATS_INIT) are required");
    }
    return guarded([&]() -> rcs_result {
        *stats = forwarder->forwarder->stats();
        return RCS_OK;
    });
}

rcs_result rcsfwd_get_room_stats(rcsfwd forwarder, const char* room, rcsfwd_room_stats* stats) {
    if (!forwarder || !stats || stats->struct_size != sizeof(rcsfwd_room_stats)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "forwarder and stats (RCSFWD_ROOM_STATS_INIT) are required");
    }
    return guarded([&]() -> rcs_result {
        *stats = forwarder->forwarder->roomStats(roomName(room));
        return RCS_OK;
    });
}

rcs_result rcsfwd_list_viewers(rcsfwd forwarder, const char* room, rcsfwd_viewer_stats* items, uint32_t item_size,
                               uint32_t capacity, uint32_t* count) {
    if (!forwarder || !count || (capacity && !items) || item_size != sizeof(rcsfwd_viewer_stats)) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "forwarder, count and item_size (sizeof(rcsfwd_viewer_stats)) are "
                                                  "required; items with a capacity");
    }
    return guarded([&]() -> rcs_result {
        const auto list = forwarder->forwarder->viewers(roomName(room));
        *count = uint32_t(list.size());
        if (list.size() > capacity) {
            return refuse(RCS_ERROR_BUFFER_TOO_SMALL, "The room has " + std::to_string(list.size()) + " viewers");
        }
        std::copy(list.begin(), list.end(), items);
        return RCS_OK;
    });
}

rcs_result rcsfwd_close_room(rcsfwd forwarder, const char* room) {
    if (!forwarder) {
        return refuse(RCS_ERROR_INVALID_ARGUMENT, "forwarder is required");
    }
    return guarded([&]() -> rcs_result {
        forwarder->forwarder->closeRoom(roomName(room));
        return RCS_OK;
    });
}

void rcsfwd_get_last_error(char* message, uint32_t capacity) {
    if (message && capacity) {
        rcsdk::copyText(message, capacity, lastError);
    }
}

} // extern "C"
