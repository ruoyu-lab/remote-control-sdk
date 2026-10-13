// Systems that let no application restart, shut down, sleep or sign out the
// device, and that have no wake information to tell: iOS, Android, a browser,
// and systems without a layer of their own.
#include "system.hpp"

namespace rcsdk::power {
namespace {

#if defined(__APPLE__)
constexpr char actionsWhy[] = "iOS lets no application restart, shut down, sleep or sign out the device";
constexpr char interfacesWhy[] = "An iPhone or iPad is not woken over the network";
#elif defined(__ANDROID__)
constexpr char actionsWhy[] = "Android lets no application restart, shut down, sleep or sign out the device";
constexpr char interfacesWhy[] = "An Android device is not woken over the network";
#elif defined(__EMSCRIPTEN__)
constexpr char actionsWhy[] = "A browser lets no page restart, shut down, sleep or sign out the machine";
constexpr char interfacesWhy[] = "A browser shows a page no network interface";
#else
constexpr char actionsWhy[] = "This system's layer carries out no power actions";
constexpr char interfacesWhy[] = "This system's layer lists no network interfaces";
#endif

class NoMachine final : public Machine {
public:
    Abilities abilities() override { return {0, actionsWhy}; }
    void check(rcs_power_action, uint32_t) override { throw Refused(RCS_ERROR_UNSUPPORTED, actionsWhy); }
    void act(rcs_power_action, uint32_t) override { throw Refused(RCS_ERROR_UNSUPPORTED, actionsWhy); }
    std::vector<rcs_power_interface> interfaces() override { throw Refused(RCS_ERROR_UNSUPPORTED, interfacesWhy); }
    std::string noInterfaces() override { return interfacesWhy; }
};

} // namespace

std::shared_ptr<Machine> systemMachine() {
    return std::make_shared<NoMachine>();
}

} // namespace rcsdk::power
