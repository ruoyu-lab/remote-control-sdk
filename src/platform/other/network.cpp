// Systems whose Wi-Fi an application cannot keep out of power saving
// (Apple's: none of theirs offers it). (Android, Windows: their network.cpp.)
#include "platform/platform.hpp"

namespace rcsdk::platform {

std::shared_ptr<void> lowLatencyWifi() {
    fail(RCS_ERROR_UNSUPPORTED, "This system lets no application keep its Wi-Fi out of power saving");
}

} // namespace rcsdk::platform
