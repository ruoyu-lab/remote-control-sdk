// Sharing the screen from the demo's native code (screen_share_android.cpp):
// start() asks the user (ProjectionActivity), ScreenCaptureService draws the
// screen into hardware buffers and records what the device plays, and each
// picture and piece of sound goes to the native side as it comes.
package local.rcsdk.qmldemo;

import android.content.Context;
import android.content.Intent;
import android.hardware.HardwareBuffer;

public final class ScreenShare {
    private ScreenShare() {}

    // The native side's: each picture, upright, any size; the HardwareBuffer is valid during the call.
    static native void nativePicture(long handle, HardwareBuffer buffer);
    // 48 kHz stereo float32, interleaved.
    static native void nativeSound(long handle, float[] samples, int frames);
    // Sharing started, or ended (refused, stopped by the user or the system) with why.
    static native void nativeState(long handle, boolean running, String reason);

    // Asks the user to share the screen, its longer side at most `longSide`
    // pixels, and with `sound` what the device plays (needs RECORD_AUDIO).
    public static void start(Context context, long handle, int longSide, boolean sound) {
        Intent intent = new Intent(context, ProjectionActivity.class);
        intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK);
        intent.putExtra(ScreenCaptureService.EXTRA_HANDLE, handle);
        intent.putExtra(ScreenCaptureService.EXTRA_LONG_SIDE, longSide);
        intent.putExtra(ScreenCaptureService.EXTRA_SOUND, sound);
        context.startActivity(intent);
    }

    public static void stop(Context context) {
        context.stopService(new Intent(context, ScreenCaptureService.class));
    }
}
