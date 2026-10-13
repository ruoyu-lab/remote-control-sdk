// Built against the installed SDK package by tests/installed_consumer.cmake.
#include <rcsdk/clipboard.h>
#include <rcsdk/rcsdk.hpp>
#include <rcsdk/supervisor.h>

#include <iostream>

int main() {
    const auto capabilities = rcsdk::capabilities();
    rcsdk::registerModule(*rcs_clipboard_module()); // An official module, installed beside the core.
    rcs_clipboard_settings clipboard = RCS_CLIPBOARD_SETTINGS_INIT;
    rcsdk::Session session(rcsdk::SessionConfig(RCS_ROLE_VIEW)
                               .setKey(rcsdk::generateConnectionKey())
                               .useModule(RCS_CLIPBOARD_MODULE, clipboard));
    if (session.state() != RCS_STATE_IDLE) {
        return 1;
    }
    uint32_t sessions = 0; // An official component, installed beside the core too (Windows has the sessions).
    const rcs_result listed = rcs_list_os_sessions(nullptr, sizeof(rcs_os_session_info), 0, &sessions);
    if (listed != RCS_OK && listed != RCS_ERROR_UNSUPPORTED) {
        return 1;
    }
    std::cout << "Installed SDK " << rcsdk::version() << " (platform " << capabilities.platform << ", "
              << rcsdk::codecImplementations().size() << " codec implementations, " << rcsdk::registeredModules().size()
              << " module) is usable\n";
    return 0;
}
