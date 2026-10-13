// What the demo's native code asks of Android beyond the SDK.
package local.rcsdk.qmldemo;

import android.content.Context;
import android.net.wifi.WifiManager;

public final class Platform {
    private static WifiManager.MulticastLock multicast;

    private Platform() {}

    // Wi-Fi drops broadcasts and multicasts to save power unless an
    // application holds this lock: the demo's LAN discovery needs them.
    public static synchronized void holdMulticastLock(Context context) {
        if (multicast == null) {
            WifiManager wifi = context.getApplicationContext().getSystemService(WifiManager.class);
            if (wifi != null) {
                multicast = wifi.createMulticastLock("rcsdk-demo-discovery");
                multicast.setReferenceCounted(false);
                multicast.acquire();
            }
        }
    }
}
