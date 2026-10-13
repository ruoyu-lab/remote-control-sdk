#include "protocol/connection.hpp"

namespace rcsdk::protocol {

void failGoodbye(Goodbye::Reason reason) {
    if (reason == Goodbye::restarting) {
        fail(RCS_ERROR_RESTARTING, "The peer closed the connection and will be back");
    }
    fail(RCS_ERROR_DISCONNECTED, "The peer closed the connection");
}

} // namespace rcsdk::protocol
