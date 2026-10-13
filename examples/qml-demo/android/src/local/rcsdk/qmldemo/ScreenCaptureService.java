// The screen shared: a foreground service (Android requires one for
// MediaProjection) draws the screen into an ImageReader's hardware buffers
// through a mirroring virtual display, its longer side at most the size
// asked for, and turns it with the device; with sound, records what the
// device plays (AudioPlaybackCapture: media, games and unknown usages, not
// the demo's own) as 48 kHz stereo float. Each picture and piece of sound goes to the native
// side (ScreenShare), which pushes it to the SDK.
package local.rcsdk.qmldemo;

import android.app.Notification;
import android.app.NotificationChannel;
import android.app.NotificationManager;
import android.app.Service;
import android.content.Intent;
import android.content.pm.ServiceInfo;
import android.content.res.Configuration;
import android.graphics.PixelFormat;
import android.graphics.Rect;
import android.hardware.HardwareBuffer;
import android.hardware.display.DisplayManager;
import android.hardware.display.VirtualDisplay;
import android.media.AudioAttributes;
import android.media.AudioFormat;
import android.media.AudioPlaybackCaptureConfiguration;
import android.media.AudioRecord;
import android.media.Image;
import android.media.ImageReader;
import android.media.projection.MediaProjection;
import android.media.projection.MediaProjectionManager;
import android.os.Build;
import android.os.Handler;
import android.os.HandlerThread;
import android.os.IBinder;
import android.os.Process;
import android.util.DisplayMetrics;
import android.util.Log;
import android.view.Display;
import android.view.WindowManager;

import java.util.concurrent.atomic.AtomicBoolean;

public final class ScreenCaptureService extends Service {
    static final String EXTRA_HANDLE = "handle";
    static final String EXTRA_LONG_SIDE = "longSide";
    static final String EXTRA_SOUND = "sound";
    static final String EXTRA_RESULT = "result";
    static final String EXTRA_DATA = "data";
    private static final String TAG = "rcsdk-demo";
    private static final String CHANNEL = "screen";
    private static final int RATE = 48000;

    private long handle;
    private int longSide;
    private MediaProjection projection;
    private HandlerThread thread;
    private Handler handler;
    private VirtualDisplay display;
    private ImageReader reader;
    private int width, height, dpi;
    private AudioRecord recorder;
    private Thread soundThread;
    private volatile boolean recording;
    private final AtomicBoolean ended = new AtomicBoolean();

    @Override
    public IBinder onBind(Intent intent) {
        return null;
    }

    @Override
    public int onStartCommand(Intent intent, int flags, int startId) {
        if (projection != null || intent == null) {
            return START_NOT_STICKY;
        }
        handle = intent.getLongExtra(EXTRA_HANDLE, 0);
        longSide = intent.getIntExtra(EXTRA_LONG_SIDE, 1920);
        NotificationManager notifications = getSystemService(NotificationManager.class);
        notifications.createNotificationChannel(new NotificationChannel(
            CHANNEL, getString(R.string.capture_channel), NotificationManager.IMPORTANCE_LOW));
        Notification notification = new Notification.Builder(this, CHANNEL)
                                        .setContentTitle(getString(R.string.capture_notification))
                                        .setSmallIcon(android.R.drawable.ic_menu_share)
                                        .setOngoing(true)
                                        .build();
        // Before the projection is taken, as Android 14 requires.
        startForeground(1, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_MEDIA_PROJECTION);
        MediaProjectionManager manager = getSystemService(MediaProjectionManager.class);
        Intent data = intent.getParcelableExtra(EXTRA_DATA);
        projection = data == null ? null : manager.getMediaProjection(intent.getIntExtra(EXTRA_RESULT, 0), data);
        if (projection == null) {
            end("系统没有给出屏幕");
            return START_NOT_STICKY;
        }
        thread = new HandlerThread("rcsdk-screen");
        thread.start();
        handler = new Handler(thread.getLooper());
        // Before the virtual display, as Android 14 requires; the user may stop it from the status bar.
        projection.registerCallback(new MediaProjection.Callback() {
            @Override
            public void onStop() {
                end("共享屏幕已停止");
            }
        }, handler);
        measure();
        reader = newReader();
        display = projection.createVirtualDisplay("rcsdk-demo", width, height, dpi,
                                                  DisplayManager.VIRTUAL_DISPLAY_FLAG_AUTO_MIRROR, reader.getSurface(),
                                                  null, handler);
        if (intent.getBooleanExtra(EXTRA_SOUND, false)) {
            startSound();
        }
        ScreenShare.nativeState(handle, true, null);
        return START_NOT_STICKY;
    }

    // Turned: the virtual display takes the screen's new shape.
    @Override
    public void onConfigurationChanged(Configuration configuration) {
        super.onConfigurationChanged(configuration);
        if (handler == null) {
            return;
        }
        handler.post(() -> {
            final int oldWidth = width, oldHeight = height;
            measure();
            if (display == null || (width == oldWidth && height == oldHeight)) {
                return;
            }
            ImageReader old = reader;
            reader = newReader();
            display.resize(width, height, dpi);
            display.setSurface(reader.getSurface());
            old.close();
        });
    }

    @Override
    public void onDestroy() {
        end(null);
        super.onDestroy();
    }

    // The screen's size as it is turned now, its longer side at most longSide.
    private void measure() {
        int screenWidth, screenHeight;
        if (Build.VERSION.SDK_INT >= 30) {
            Rect bounds = getSystemService(WindowManager.class).getMaximumWindowMetrics().getBounds();
            screenWidth = bounds.width();
            screenHeight = bounds.height();
        } else {
            DisplayMetrics metrics = new DisplayMetrics();
            getSystemService(DisplayManager.class).getDisplay(Display.DEFAULT_DISPLAY).getRealMetrics(metrics);
            screenWidth = metrics.widthPixels;
            screenHeight = metrics.heightPixels;
        }
        double scale = Math.min(1.0, (double) longSide / Math.max(screenWidth, screenHeight));
        width = (int) Math.round(screenWidth * scale) & ~1;
        height = (int) Math.round(screenHeight * scale) & ~1;
        dpi = Math.max(1, (int) Math.round(getResources().getDisplayMetrics().densityDpi * scale));
    }

    private ImageReader newReader() {
        ImageReader images =
            ImageReader.newInstance(width, height, PixelFormat.RGBA_8888, 3, HardwareBuffer.USAGE_GPU_SAMPLED_IMAGE);
        images.setOnImageAvailableListener(this::picture, handler);
        return images;
    }

    private void picture(ImageReader images) {
        Image image = images.acquireLatestImage();
        if (image == null) {
            return;
        }
        HardwareBuffer buffer = image.getHardwareBuffer();
        if (buffer != null) {
            ScreenShare.nativePicture(handle, buffer); // Drawn into the SDK's own before it returns.
            buffer.close();
        }
        image.close();
    }

    private void startSound() {
        try {
            AudioPlaybackCaptureConfiguration playback =
                new AudioPlaybackCaptureConfiguration.Builder(projection)
                    .addMatchingUsage(AudioAttributes.USAGE_MEDIA)
                    .addMatchingUsage(AudioAttributes.USAGE_GAME)
                    .addMatchingUsage(AudioAttributes.USAGE_UNKNOWN)
                    .excludeUid(Process.myUid()) // The demo's own: the viewers' voices it plays.
                    .build();
            AudioFormat format = new AudioFormat.Builder()
                                     .setEncoding(AudioFormat.ENCODING_PCM_FLOAT)
                                     .setSampleRate(RATE)
                                     .setChannelMask(AudioFormat.CHANNEL_IN_STEREO)
                                     .build();
            int minimum =
                AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_STEREO, AudioFormat.ENCODING_PCM_FLOAT);
            recorder = new AudioRecord.Builder()
                           .setAudioFormat(format)
                           .setBufferSizeInBytes(Math.max(minimum, RATE / 10 * 2 * 4))
                           .setAudioPlaybackCaptureConfig(playback)
                           .build();
            recorder.startRecording();
        } catch (SecurityException | UnsupportedOperationException | IllegalStateException e) {
            Log.w(TAG, "The device's sound is not shared: " + e);
            recorder = null;
            return;
        }
        recording = true;
        soundThread = new Thread(() -> {
            float[] samples = new float[RATE / 100 * 2]; // 10 ms.
            while (recording) {
                int read = recorder.read(samples, 0, samples.length, AudioRecord.READ_BLOCKING);
                if (read > 0) {
                    ScreenShare.nativeSound(handle, samples, read / 2);
                }
            }
        }, "rcsdk-sound");
        soundThread.start();
    }

    // Stops everything once, from any thread; `reason` says why when it was not this application.
    private void end(String reason) {
        if (!ended.compareAndSet(false, true)) {
            return;
        }
        recording = false;
        if (recorder != null) {
            recorder.stop();
            try {
                soundThread.join();
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
            }
            recorder.release();
        }
        if (display != null) {
            display.release();
        }
        if (projection != null) {
            projection.stop();
        }
        if (thread != null) { // Its pictures done before the reader goes.
            thread.quitSafely();
            if (Thread.currentThread() != thread) {
                try {
                    thread.join();
                } catch (InterruptedException e) {
                    Thread.currentThread().interrupt();
                }
            }
        }
        if (reader != null) {
            reader.close();
        }
        ScreenShare.nativeState(handle, false, reason);
        stopForeground(STOP_FOREGROUND_REMOVE);
        stopSelf();
    }
}
