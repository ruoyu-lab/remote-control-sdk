// Android's Wi-Fi out of its power saving while connected: a WifiManager
// lock in WIFI_MODE_FULL_LOW_LATENCY (Android 10), through the host's Java VM.
// The application declares android.permission.WAKE_LOCK; Android applies the
// mode while Wi-Fi is in use and the application in front with the screen on
// (otherwise the lock does nothing), so it is held whether or not Wi-Fi is
// on (asking would take ACCESS_WIFI_STATE as well). `adb shell dumpsys wifi`
// lists the lock as "rcsdk".
//
// What the Wi-Fi connected is (rcs_list_wifi): WifiManager.getConnectionInfo()
// says its frequency (band and channel), signal and send rate (the
// application declares android.permission.ACCESS_WIFI_STATE, which the AAR's
// manifest does), of the interface ConnectivityManager gives the Wi-Fi network.
//
// And what its networks are: ConnectivityManager says, to Java only (through
// the host's Java VM; the application declares
// android.permission.ACCESS_NETWORK_STATE, which the AAR's manifest does), of
// each network its interface, what carries it, whether it is metered and
// which one is the default; Data Saver (getRestrictBackgroundStatus) holds
// back this application on metered ones. Without the VM: the interfaces only.
// Changes reach native code by no callback: network_poll.cpp looks. A socket
// that is to use one network is marked for it (android_setsocknetwork with
// the network's handle): netd routes an application's sockets by that mark,
// not by the device they are bound to.
#include "platform/android/jni.hpp"
#include "platform/platform.hpp"
#include "transport/interfaces.hpp"

#include <android/multinetwork.h>
#include <net/if.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <mutex>

namespace rcsdk::platform {

std::vector<Network> networks() {
    const auto interfaces = transport::interfaces();
    std::vector<Network> list;
    for (const auto& i : interfaces) {
        Network network{i.name, i.index};
        for (const auto& address : i.addresses) {
            (address.ipv6() ? network.ipv6 : network.ipv4) = true;
        }
        list.push_back(std::move(network));
    }
    if (!android::hasJava()) { // The default by the system's routes, as elsewhere.
        const uint32_t preferred = transport::defaultInterface(interfaces);
        for (auto& network : list) {
            network.flags = network.index == preferred ? RCS_NETWORK_FLAG_DEFAULT : 0;
        }
        std::stable_partition(list.begin(), list.end(),
                              [](const Network& n) { return (n.flags & RCS_NETWORK_FLAG_DEFAULT) != 0; });
        return list;
    }
    JNIEnv* env = android::env();
    const auto manager = android::systemService(env, "connectivity");
    const auto managerType = android::findClass(env, "android/net/ConnectivityManager");
    const auto networkType = android::findClass(env, "android/net/Network");
    const auto propertiesType = android::findClass(env, "android/net/LinkProperties");
    const auto capabilitiesType = android::findClass(env, "android/net/NetworkCapabilities");
    const jmethodID getAll = android::method(env, managerType.get(), "getAllNetworks", "()[Landroid/net/Network;");
    const jmethodID getActive = android::method(env, managerType.get(), "getActiveNetwork", "()Landroid/net/Network;");
    const jmethodID getProperties = android::method(env, managerType.get(), "getLinkProperties",
                                                    "(Landroid/net/Network;)Landroid/net/LinkProperties;");
    const jmethodID getCapabilities = android::method(env, managerType.get(), "getNetworkCapabilities",
                                                      "(Landroid/net/Network;)Landroid/net/NetworkCapabilities;");
    const jmethodID getSaving = android::method(env, managerType.get(), "getRestrictBackgroundStatus", "()I");
    const jmethodID equals = android::method(env, networkType.get(), "equals", "(Ljava/lang/Object;)Z");
    const jmethodID interfaceName =
        android::method(env, propertiesType.get(), "getInterfaceName", "()Ljava/lang/String;");
    const jmethodID hasTransport = android::method(env, capabilitiesType.get(), "hasTransport", "(I)Z");
    const jmethodID hasCapability = android::method(env, capabilitiesType.get(), "hasCapability", "(I)Z");
    const std::string permission = " (the application declares android.permission.ACCESS_NETWORK_STATE)";
    android::Local<jobjectArray> all(env, static_cast<jobjectArray>(env->CallObjectMethod(manager.get(), getAll)));
    android::check(env, RCS_ERROR_PERMISSION_REQUIRED, "ConnectivityManager.getAllNetworks()" + permission);
    android::Local<> active(env, env->CallObjectMethod(manager.get(), getActive));
    android::check(env, RCS_ERROR_PERMISSION_REQUIRED, "ConnectivityManager.getActiveNetwork()" + permission);
    constexpr jint savingData = 3; // RESTRICT_BACKGROUND_STATUS_ENABLED: Data Saver, this application not exempt.
    const bool saving = env->CallIntMethod(manager.get(), getSaving) == savingData;
    android::check(env, RCS_ERROR_INTERNAL, "ConnectivityManager.getRestrictBackgroundStatus()");
    // NetworkCapabilities' TRANSPORT_* and NET_CAPABILITY_* values.
    constexpr jint cellular = 0, wifi = 1, ethernet = 3, vpn = 4, notMetered = 11, temporarilyNotMetered = 25;
    const jsize count = all ? env->GetArrayLength(all.get()) : 0;
    for (jsize n = 0; n < count; ++n) {
        android::Local<> network(env, env->GetObjectArrayElement(all.get(), n));
        android::Local<> properties(env, env->CallObjectMethod(manager.get(), getProperties, network.get()));
        android::Local<> facts(env, env->CallObjectMethod(manager.get(), getCapabilities, network.get()));
        android::check(env, RCS_ERROR_PERMISSION_REQUIRED, "ConnectivityManager.getNetworkCapabilities()" + permission);
        if (!properties || !facts) {
            continue; // Gone meanwhile.
        }
        android::Local<jstring> name(env, static_cast<jstring>(env->CallObjectMethod(properties.get(), interfaceName)));
        android::check(env, RCS_ERROR_INTERNAL, "LinkProperties.getInterfaceName()");
        const std::string interface = android::text(env, name.get());
        const auto has = [&](jmethodID which, jint value) {
            return env->CallBooleanMethod(facts.get(), which, value) == JNI_TRUE;
        };
        rcs_network_kind kind = RCS_NETWORK_OTHER;
        if (has(hasTransport, wifi)) {
            kind = RCS_NETWORK_WIFI;
        } else if (has(hasTransport, cellular)) {
            kind = RCS_NETWORK_CELLULAR;
        } else if (has(hasTransport, ethernet)) {
            kind = RCS_NETWORK_ETHERNET;
        } else if (has(hasTransport, vpn)) {
            kind = RCS_NETWORK_UNKNOWN; // A VPN that does not say what it runs over.
        }
        uint32_t flags = has(hasTransport, vpn) ? RCS_NETWORK_FLAG_VPN : 0;
        if (!has(hasCapability, notMetered) && !has(hasCapability, temporarilyNotMetered)) {
            flags |= RCS_NETWORK_FLAG_METERED | (saving ? RCS_NETWORK_FLAG_CONSTRAINED : 0);
        }
        if (active && env->CallBooleanMethod(network.get(), equals, active.get()) == JNI_TRUE) {
            flags |= RCS_NETWORK_FLAG_DEFAULT;
        }
        android::check(env, RCS_ERROR_INTERNAL, "NetworkCapabilities");
        // A cellular network's IPv4 through 464XLAT is a stacked interface of its own ("v4-rmnet_data0").
        for (auto& entry : list) {
            if (!interface.empty() && (entry.name == interface || entry.name == "v4-" + interface)) {
                entry.kind = kind;
                entry.flags = flags;
            }
        }
    }
    std::stable_partition(list.begin(), list.end(),
                          [](const Network& n) { return (n.flags & RCS_NETWORK_FLAG_DEFAULT) != 0; });
    return list;
}

uint32_t networkFacts() noexcept {
    return android::hasJava() ? RCS_NETWORK_FACT_KIND | RCS_NETWORK_FACT_METERED | RCS_NETWORK_FACT_CONSTRAINED : 0;
}

std::vector<rcs_wifi_info> wifiNetworks() {
    if (!android::hasJava()) {
        return {};
    }
    std::vector<rcs_wifi_info> list;
    for (const auto& network : networks()) {
        if (network.kind == RCS_NETWORK_WIFI && !(network.flags & RCS_NETWORK_FLAG_VPN)) {
            rcs_wifi_info info{};
            std::strncpy(info.name, network.name.c_str(), sizeof(info.name) - 1);
            info.index = network.index;
            list.push_back(info);
        }
    }
    if (list.empty()) {
        return list;
    }
    JNIEnv* env = android::env();
    const auto wifi = android::systemService(env, "wifi");
    const auto managerType = android::findClass(env, "android/net/wifi/WifiManager");
    const auto infoType = android::findClass(env, "android/net/wifi/WifiInfo");
    android::Local<> connection(
        env, env->CallObjectMethod(wifi.get(), android::method(env, managerType.get(), "getConnectionInfo",
                                                               "()Landroid/net/wifi/WifiInfo;")));
    android::check(env, RCS_ERROR_PERMISSION_REQUIRED,
                   "WifiManager.getConnectionInfo() (the application declares android.permission.ACCESS_WIFI_STATE)");
    if (!connection) {
        return list;
    }
    const auto call = [&](const char* name) {
        const jint value = env->CallIntMethod(connection.get(), android::method(env, infoType.get(), name, "()I"));
        android::check(env, RCS_ERROR_INTERNAL, std::string("WifiInfo.") + name + "()");
        return value;
    };
    const jint mhz = call("getFrequency"), rssi = call("getRssi"), rate = call("getTxLinkSpeedMbps");
    rcs_wifi_info& info = list.front(); // The one WifiInfo is of: the primary.
    if (mhz >= 2412 && mhz <= 2484) {
        info.band = RCS_WIFI_BAND_2_4_GHZ;
        info.channel = mhz == 2484 ? 14 : uint32_t(mhz - 2407) / 5;
    } else if (mhz >= 5160 && mhz <= 5885) {
        info.band = RCS_WIFI_BAND_5_GHZ;
        info.channel = uint32_t(mhz - 5000) / 5;
    } else if (mhz >= 5955 && mhz <= 7115) {
        info.band = RCS_WIFI_BAND_6_GHZ;
        info.channel = uint32_t(mhz - 5950) / 5;
    }
    info.signal_dbm = rssi > -127 && rssi < 0 ? rssi : 0; // -127: unknown.
    info.rate_mbps = rate > 0 ? uint32_t(rate) : 0;
    return list;
}

uint32_t wifiFacts() noexcept {
    return android::hasJava() ? RCS_WIFI_FACT_CHANNEL | RCS_WIFI_FACT_SIGNAL | RCS_WIFI_FACT_RATE : 0;
}

} // namespace rcsdk::platform

namespace rcsdk::transport {

bool bindToAndroidNetwork(Handle handle, uint32_t index) {
    char name[IF_NAMESIZE] = {};
    if (!platform::android::hasJava() || !if_indextoname(index, name)) {
        return false;
    }
    namespace android = platform::android;
    const std::string wanted = std::string(name).rfind("v4-", 0) == 0 ? std::string(name + 3) : std::string(name);
    JNIEnv* env = android::env();
    const auto manager = android::systemService(env, "connectivity");
    const auto managerType = android::findClass(env, "android/net/ConnectivityManager");
    const auto networkType = android::findClass(env, "android/net/Network");
    const auto propertiesType = android::findClass(env, "android/net/LinkProperties");
    const jmethodID getAll = android::method(env, managerType.get(), "getAllNetworks", "()[Landroid/net/Network;");
    const jmethodID getProperties = android::method(env, managerType.get(), "getLinkProperties",
                                                    "(Landroid/net/Network;)Landroid/net/LinkProperties;");
    const jmethodID interfaceName =
        android::method(env, propertiesType.get(), "getInterfaceName", "()Ljava/lang/String;");
    const jmethodID networkHandle = android::method(env, networkType.get(), "getNetworkHandle", "()J");
    android::Local<jobjectArray> all(env, static_cast<jobjectArray>(env->CallObjectMethod(manager.get(), getAll)));
    android::check(env, RCS_ERROR_PERMISSION_REQUIRED,
                   "ConnectivityManager.getAllNetworks() (the application declares "
                   "android.permission.ACCESS_NETWORK_STATE)");
    const jsize count = all ? env->GetArrayLength(all.get()) : 0;
    for (jsize n = 0; n < count; ++n) {
        android::Local<> network(env, env->GetObjectArrayElement(all.get(), n));
        android::Local<> properties(env, env->CallObjectMethod(manager.get(), getProperties, network.get()));
        android::check(env, RCS_ERROR_INTERNAL, "ConnectivityManager.getLinkProperties()");
        if (!properties) {
            continue;
        }
        android::Local<jstring> named(env,
                                      static_cast<jstring>(env->CallObjectMethod(properties.get(), interfaceName)));
        android::check(env, RCS_ERROR_INTERNAL, "LinkProperties.getInterfaceName()");
        if (android::text(env, named.get()) != wanted) {
            continue;
        }
        const jlong marked = env->CallLongMethod(network.get(), networkHandle);
        android::check(env, RCS_ERROR_INTERNAL, "Network.getNetworkHandle()");
        if (android_setsocknetwork(net_handle_t(marked), handle) != 0) {
            fail(RCS_ERROR_NETWORK,
                 "Could not bind the socket to the network of " + wanted + ": " + std::strerror(errno));
        }
        return true;
    }
    return false;
}

} // namespace rcsdk::transport

namespace rcsdk::platform {

std::shared_ptr<void> lowLatencyWifi() {
    static std::mutex mutex;
    static std::weak_ptr<void> held; // One lock for every session of the process.
    std::lock_guard lock(mutex);
    if (auto shared = held.lock()) {
        return shared;
    }
    JNIEnv* env = android::env();
    const auto wifi = android::systemService(env, "wifi");
    const auto managerType = android::findClass(env, "android/net/wifi/WifiManager");
    constexpr jint lowLatencyMode = 4; // WifiManager.WIFI_MODE_FULL_LOW_LATENCY
    const jmethodID create = android::method(env, managerType.get(), "createWifiLock",
                                             "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
    android::Local<> wifiLock(
        env, env->CallObjectMethod(wifi.get(), create, lowLatencyMode, android::javaString(env, "rcsdk").get()));
    android::check(env, RCS_ERROR_INTERNAL, "WifiManager.createWifiLock()");
    const auto lockType = android::findClass(env, "android/net/wifi/WifiManager$WifiLock");
    env->CallVoidMethod(wifiLock.get(), android::method(env, lockType.get(), "setReferenceCounted", "(Z)V"),
                        JNI_FALSE);
    env->CallVoidMethod(wifiLock.get(), android::method(env, lockType.get(), "acquire", "()V"));
    android::check(env, RCS_ERROR_PERMISSION_REQUIRED,
                   "Holding a low-latency Wi-Fi lock (the application declares android.permission.WAKE_LOCK)");
    std::shared_ptr<void> shared(env->NewGlobalRef(wifiLock.get()), [](void* object) {
        JNIEnv* env = android::env();
        const auto global = static_cast<jobject>(object);
        if (android::Local<jclass> type{env, env->GetObjectClass(global)}) {
            env->CallVoidMethod(global, env->GetMethodID(type.get(), "release", "()V"));
        }
        env->ExceptionClear();
        env->DeleteGlobalRef(global);
    });
    held = shared;
    return shared;
}

} // namespace rcsdk::platform
