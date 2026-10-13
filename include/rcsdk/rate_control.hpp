// Remote Control SDK - C++ wrapper of the host's bitrate decisions
// (rcsdk/rate_control.h).
#pragma once

#include <rcsdk/rate_control.h>
#include <rcsdk/rcsdk.hpp>

namespace rcsdk {

template <>
inline rcs_rate_decider_config defaults<rcs_rate_decider_config>() noexcept {
    return RCS_RATE_DECIDER_CONFIG_INIT;
}

// Share, adaptive bitrate: the host decides (see rcs_rate_decider_config).
// It must outlive the session; decide must not throw (an escaping exception
// terminates the process).
class RateDecider {
public:
    virtual ~RateDecider() = default;
    // Each report of a viewer and the rules' proposal in decision.bitrate_bps,
    // to change or leave.
    virtual void decide(const rcs_rate_report& report, rcs_rate_decision& decision) = 0;
};

//   config.use(rcsdk::rateDecider(decider));
inline rcs_rate_decider_config rateDecider(RateDecider& decider) noexcept {
    rcs_rate_decider_config c = RCS_RATE_DECIDER_CONFIG_INIT;
    c.user_data = &decider;
    c.on_rate_report = [](const rcs_rate_report* report, rcs_rate_decision* decision, void* data) noexcept {
        static_cast<RateDecider*>(data)->decide(*report, *decision);
    };
    return c;
}

} // namespace rcsdk
