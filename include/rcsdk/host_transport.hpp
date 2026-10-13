// Remote Control SDK - C++ wrapper of the host's own transport
// (rcsdk/host_transport.h).
#pragma once

#include <rcsdk/host_transport.h>
#include <rcsdk/rcsdk.hpp>

namespace rcsdk {

template <>
inline rcs_host_transport_config defaults<rcs_host_transport_config>() noexcept {
    return RCS_HOST_TRANSPORT_CONFIG_INIT;
}

// RCS_TRANSPORT_HOST: the host's channel to the other end (see
// rcs_host_transport_config). It must outlive the session; send must not
// throw (an escaping exception terminates the process).
class HostTransport {
public:
    virtual ~HostTransport() = default;
    // Send the bytes to the other end on `route` (a viewer's: 0); true when the channel took them.
    virtual bool send(uint64_t route, const uint8_t* data, uint32_t size) = 0;
};

//   config.setTransport(RCS_TRANSPORT_HOST).use(rcsdk::hostTransport(channel, RCS_HOST_CHANNEL_DATAGRAM));
// maxDatagram 0 is 1200.
inline rcs_host_transport_config hostTransport(HostTransport& transport, rcs_host_channel channel,
                                               uint32_t maxDatagram = 0) noexcept {
    rcs_host_transport_config c = RCS_HOST_TRANSPORT_CONFIG_INIT;
    c.host_channel = channel;
    c.host_max_datagram = maxDatagram;
    c.user_data = &transport;
    c.on_transport_send = [](uint64_t route, const uint8_t* bytes, uint32_t size, void* data) noexcept -> int32_t {
        return static_cast<HostTransport*>(data)->send(route, bytes, size) ? 1 : 0;
    };
    return c;
}

// What arrived from the other end on `route` over the host's channel, that
// the channel closed, and that it was replaced by one to the same end
// (rcs_session_transport_receive, rcs_session_transport_closed,
// rcs_session_transport_restarted). A viewer's route is 0.
inline rcs_result transportReceive(Session& session, uint64_t route, const void* data, uint32_t size) noexcept {
    return rcs_session_transport_receive(session.handle(), route, static_cast<const uint8_t*>(data), size);
}
inline rcs_result transportClosed(Session& session, uint64_t route) noexcept {
    return rcs_session_transport_closed(session.handle(), route);
}
inline rcs_result transportRestarted(Session& session, uint64_t route) noexcept {
    return rcs_session_transport_restarted(session.handle(), route);
}

} // namespace rcsdk
