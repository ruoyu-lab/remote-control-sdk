// Shows the system's dialog for sharing the screen (the whole screen, or from
// Android 14 on one app) and hands the user's answer to ScreenCaptureService.
package local.rcsdk.qmldemo;

import android.app.Activity;
import android.content.Intent;
import android.media.projection.MediaProjectionManager;
import android.os.Bundle;

public final class ProjectionActivity extends Activity {
    private static final int REQUEST = 1;

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        if (state == null) {
            MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
            startActivityForResult(manager.createScreenCaptureIntent(), REQUEST);
        }
    }

    @Override
    protected void onActivityResult(int request, int result, Intent data) {
        super.onActivityResult(request, result, data);
        Intent asked = getIntent();
        long handle = asked.getLongExtra(ScreenCaptureService.EXTRA_HANDLE, 0);
        if (result != RESULT_OK || data == null) {
            ScreenShare.nativeState(handle, false, "没有允许共享屏幕");
        } else {
            Intent service = new Intent(this, ScreenCaptureService.class);
            service.putExtras(asked);
            service.putExtra(ScreenCaptureService.EXTRA_RESULT, result);
            service.putExtra(ScreenCaptureService.EXTRA_DATA, data);
            startForegroundService(service);
        }
        finish();
    }
}
