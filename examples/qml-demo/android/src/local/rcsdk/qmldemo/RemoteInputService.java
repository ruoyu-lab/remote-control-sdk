// A viewer's input on this device, from the demo's native code
// (screen_input_android.cpp), through the one way Android lets an application
// act on others: an accessibility service the user turns on in Settings.
// Positions are fractions of the screen (0..1). A press, the moves while it
// is held and its release become one continuing stroke (dispatched piece by
// piece as the last piece is done, moves in between joined), so taps, long
// presses and drags act as a finger's; a wheel becomes a short swipe; text
// goes into the focused field.
package local.rcsdk.qmldemo;

import android.accessibilityservice.AccessibilityService;
import android.accessibilityservice.GestureDescription;
import android.content.Context;
import android.content.Intent;
import android.graphics.Path;
import android.graphics.Rect;
import android.os.Build;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.provider.Settings;
import android.util.DisplayMetrics;
import android.view.WindowManager;
import android.view.accessibility.AccessibilityEvent;
import android.view.accessibility.AccessibilityNodeInfo;

import java.util.function.Consumer;

public final class RemoteInputService extends AccessibilityService {
    private static volatile RemoteInputService instance;
    private static final Handler main = new Handler(Looper.getMainLooper());
    private static final long STEP_MS = 8; // How long each piece of a stroke takes.

    // On the main thread: the stroke being made, where it is, and where it goes next.
    private GestureDescription.StrokeDescription stroke;
    private float strokeX, strokeY;
    private boolean dispatching, moved, released;
    private float nextX, nextY;

    @Override
    protected void onServiceConnected() {
        instance = this;
    }

    @Override
    public boolean onUnbind(Intent intent) {
        instance = null;
        return super.onUnbind(intent);
    }

    @Override
    public void onAccessibilityEvent(AccessibilityEvent event) {}

    @Override
    public void onInterrupt() {}

    // Whether the user turned the service on.
    public static boolean enabled() {
        return instance != null;
    }

    public static void openSettings(Context context) {
        context.startActivity(
            new Intent(Settings.ACTION_ACCESSIBILITY_SETTINGS).addFlags(Intent.FLAG_ACTIVITY_NEW_TASK));
    }

    public static void press(float u, float v) {
        post(service -> service.down(u, v));
    }

    public static void move(float u, float v) {
        post(service -> service.towards(u, v, false));
    }

    public static void release(float u, float v) {
        post(service -> service.towards(u, v, true));
    }

    // A swipe from (u, v) by (du, dv), fractions of the screen.
    public static void swipe(float u, float v, float du, float dv) {
        post(service -> service.swipeBy(u, v, du, dv));
    }

    public static void back() {
        post(service -> service.performGlobalAction(GLOBAL_ACTION_BACK));
    }

    public static void home() {
        post(service -> service.performGlobalAction(GLOBAL_ACTION_HOME));
    }

    public static void recents() {
        post(service -> service.performGlobalAction(GLOBAL_ACTION_RECENTS));
    }

    public static void type(String text) {
        post(service -> service.edit(text, false));
    }

    public static void backspace() {
        post(service -> service.edit("", true));
    }

    public static void enter() {
        post(RemoteInputService::submit);
    }

    private static void post(Consumer<RemoteInputService> action) {
        main.post(() -> {
            RemoteInputService service = instance;
            if (service != null) {
                action.accept(service);
            }
        });
    }

    // ---- Strokes ---------------------------------------------------------------------

    private float x(float u) {
        return Math.max(0, Math.min(u, 1)) * (screen().width() - 1);
    }

    private float y(float v) {
        return Math.max(0, Math.min(v, 1)) * (screen().height() - 1);
    }

    private Rect screen() {
        WindowManager windows = getSystemService(WindowManager.class);
        if (Build.VERSION.SDK_INT >= 30) {
            return windows.getMaximumWindowMetrics().getBounds();
        }
        DisplayMetrics metrics = new DisplayMetrics();
        windows.getDefaultDisplay().getRealMetrics(metrics);
        return new Rect(0, 0, metrics.widthPixels, metrics.heightPixels);
    }

    private void down(float u, float v) {
        strokeX = x(u);
        strokeY = y(v);
        moved = released = false;
        Path path = new Path();
        path.moveTo(strokeX, strokeY);
        stroke = new GestureDescription.StrokeDescription(path, 0, 1, true);
        dispatch();
    }

    private void towards(float u, float v, boolean release) {
        if (stroke == null) {
            return;
        }
        nextX = x(u);
        nextY = y(v);
        moved = true;
        released |= release;
        if (!dispatching) {
            next();
        }
    }

    // The next piece: to where the pointer went meanwhile, the last one lifting the finger.
    private void next() {
        if (stroke == null || !moved) {
            return;
        }
        Path path = new Path();
        path.moveTo(strokeX, strokeY);
        if (nextX != strokeX || nextY != strokeY) {
            path.lineTo(nextX, nextY);
        }
        strokeX = nextX;
        strokeY = nextY;
        moved = false;
        stroke = stroke.continueStroke(path, 0, STEP_MS, !released);
        dispatch();
        if (released) {
            stroke = null;
        }
    }

    private void dispatch() {
        GestureDescription gesture = new GestureDescription.Builder().addStroke(stroke).build();
        dispatching = dispatchGesture(gesture, new GestureResultCallback() {
            @Override
            public void onCompleted(GestureDescription done) {
                dispatching = false;
                next();
            }

            @Override
            public void onCancelled(GestureDescription done) { // Another gesture, or the screen changed.
                dispatching = false;
                stroke = null;
            }
        }, main);
        if (!dispatching) {
            stroke = null;
        }
    }

    private void swipeBy(float u, float v, float du, float dv) {
        if (stroke != null) {
            return; // A finger is down.
        }
        Path path = new Path();
        path.moveTo(x(u), y(v));
        path.lineTo(x(u + du), y(v + dv));
        dispatchGesture(new GestureDescription.Builder()
                            .addStroke(new GestureDescription.StrokeDescription(path, 0, 120))
                            .build(),
                        null, main);
    }

    // ---- Text --------------------------------------------------------------------------

    // Types `text` over the selection of the focused field, or deletes what is
    // selected, else the character before the caret.
    private void edit(String text, boolean delete) {
        AccessibilityNodeInfo node = findFocus(AccessibilityNodeInfo.FOCUS_INPUT);
        if (node == null || !node.isEditable()) {
            return;
        }
        CharSequence shown = node.getText();
        String now = shown == null || node.isShowingHintText() ? "" : shown.toString();
        int start = node.getTextSelectionStart(), end = node.getTextSelectionEnd();
        if (start < 0 || end < 0 || Math.max(start, end) > now.length()) {
            start = end = now.length();
        }
        int from = Math.min(start, end), to = Math.max(start, end);
        if (delete && from == to) {
            if (from == 0) {
                return;
            }
            from = now.offsetByCodePoints(from, -1);
        }
        String changed = now.substring(0, from) + text + now.substring(to);
        Bundle arguments = new Bundle();
        arguments.putCharSequence(AccessibilityNodeInfo.ACTION_ARGUMENT_SET_TEXT_CHARSEQUENCE, changed);
        node.performAction(AccessibilityNodeInfo.ACTION_SET_TEXT, arguments);
        int caret = from + text.length();
        Bundle selection = new Bundle();
        selection.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_START_INT, caret);
        selection.putInt(AccessibilityNodeInfo.ACTION_ARGUMENT_SELECTION_END_INT, caret);
        node.performAction(AccessibilityNodeInfo.ACTION_SET_SELECTION, selection);
    }

    // The keyboard's action key in the focused field (Android 11 on).
    private void submit() {
        AccessibilityNodeInfo node = findFocus(AccessibilityNodeInfo.FOCUS_INPUT);
        if (node != null && Build.VERSION.SDK_INT >= 30) {
            node.performAction(AccessibilityNodeInfo.AccessibilityAction.ACTION_IME_ENTER.getId());
        }
    }
}
