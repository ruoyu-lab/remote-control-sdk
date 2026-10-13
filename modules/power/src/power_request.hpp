// The system's power request, and what tests put in its place.
#pragma once

#include <rcsdk/power.h>

#include <functional>
#include <memory>
#include <stdexcept>
#include <string>

namespace rcsdk::power {

// A request the system did not take: the result and why.
class Refused : public std::runtime_error {
public:
    Refused(rcs_result result, const std::string& why) : std::runtime_error(why), result_(result) {}
    rcs_result result() const noexcept { return result_; }

private:
    rcs_result result_;
};

// Keeps the machine awake while it lives.
class Request {
public:
    virtual ~Request() = default;
};

// The system's request for `mode` (SYSTEM or DISPLAY) under `reason`; throws Refused.
std::unique_ptr<Request> systemRequest(rcs_keep_awake mode, const std::string& reason);
bool systemAvailable();

// What the module asks for: the system's request, unless a test put a factory
// in its place (an empty one restores the system's).
using Factory = std::function<std::unique_ptr<Request>(rcs_keep_awake mode, const std::string& reason)>;
void setFactory(Factory factory);
std::unique_ptr<Request> createRequest(rcs_keep_awake mode, const std::string& reason);

} // namespace rcsdk::power
