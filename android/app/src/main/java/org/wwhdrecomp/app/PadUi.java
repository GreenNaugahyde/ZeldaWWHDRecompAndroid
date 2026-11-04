package org.wwhdrecomp.app;

import android.app.Dialog;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.os.Handler;
import android.os.Looper;
import android.view.MotionEvent;
import android.view.View;
import android.view.ViewTreeObserver;
import android.util.Log;

import java.io.ByteArrayOutputStream;

/**
 * The app's dialogs on the GamePad device (PadHost): while another device is the GamePad, the dialog on top here
 * (the options, the start-state choice, questions) is drawn into a picture whenever it changes and sent there
 * (PadLink.UI_FRAME), where it lies over the GamePad picture; touches on it come back (PadLink.UI_TOUCH) and go
 * into the dialog, its controller keys too (MainActivity.padKey). The dialog shows here as well.
 * A GamePad device that draws them itself (PadLink.CAP_UI_DRAW) gets drawing orders instead of pictures (UiDraw):
 * sharp at its own size, a fraction of the data, up to 30 a second.
 */
final class PadUi {
    private static final long FRAME_MS = 66;  // at most 15 pictures a second
    private static final long DRAW_MS = 33;   // drawing orders: 30 a second
    private static final int MAX_SIDE = 1280;

    private final PadHost host;
    private final Handler main = new Handler(Looper.getMainLooper());
    private Dialog shown;
    private View root;
    private boolean posted;
    private long downTime;
    private final ViewTreeObserver.OnDrawListener onDraw = this::changed;
    private final Runnable capture = this::capture;
    private final java.util.concurrent.ExecutorService encoder = java.util.concurrent.Executors.newSingleThreadExecutor();

    private final UiDraw.Images images = new UiDraw.Images();
    private final UiDraw.Recorder recorder = new UiDraw.Recorder(images);
    private byte[] lastOrders;

    private Object target;  // the device they go to (PadHost.uiTarget)

    PadUi(PadHost host) { this.host = host; }

    /** a GamePad device came (or said it draws): the dialog on top sent anew, its bitmaps too */
    void resend() {
        images.reset();
        lastOrders = null;
        Dialog d = shown;
        shown = null;
        if (root != null) root.getViewTreeObserver().removeOnDrawListener(onDraw);
        root = null;
        update(d);
    }

    /** the dialog on top now (null: none), or the GamePad came or went (main thread) */
    void update(Dialog top) {
        Object t = host.uiTarget();
        if (t != target) {  // another device to show them (the GamePad device came or went, or only a screen device is there)
            target = t;
            images.reset();
            lastOrders = null;
            if (root != null) root.getViewTreeObserver().removeOnDrawListener(onDraw);
            root = null;
            shown = null;
        }
        if (t == null) top = null;
        if (top == shown) return;
        if (root != null) root.getViewTreeObserver().removeOnDrawListener(onDraw);
        shown = top;
        root = top != null && top.getWindow() != null ? top.getWindow().getDecorView() : null;
        if (root == null) {
            shown = null;
            main.removeCallbacks(capture);
            posted = false;
            // (after the pictures still being made: a last one sent after this would stay there)
            lastOrders = null;
            encoder.execute(() -> {
                host.sendUi(null, 0, 0);
                host.sendDraw(java.util.Collections.emptyList(), null, 0, 0);
            });
            return;
        }
        root.getViewTreeObserver().addOnDrawListener(onDraw);
        changed();
    }

    private void changed() {
        if (posted) return;
        posted = true;
        main.postDelayed(capture, host.padDraws() ? DRAW_MS : FRAME_MS);
    }

    private void capture() {
        posted = false;
        View r = root;
        if (r == null) return;
        int w = r.getWidth(), h = r.getHeight();
        if (w <= 0 || h <= 0) {
            changed();  // (not laid out yet)
            return;
        }
        if (host.padDraws()) {
            byte[] z;
            try {
                z = recorder.record(r);
            } catch (RuntimeException e) {
                Log.w("WWHD", "GamePad: dialog not recorded: " + e);
                return;
            }
            if (java.util.Arrays.equals(z, lastOrders)) return;  // (drawn again, nothing changed)
            lastOrders = z;
            java.util.List<Object[]> fresh = images.takeFresh();
            encoder.execute(() -> host.sendDraw(fresh, z, w, h));
            return;
        }
        float k = Math.min(1f, MAX_SIDE / (float) Math.max(w, h));
        Bitmap b = Bitmap.createBitmap(Math.max(1, Math.round(w * k)), Math.max(1, Math.round(h * k)), Bitmap.Config.ARGB_8888);
        Canvas c = new Canvas(b);
        c.drawColor(0xFF0C1622);  // (the dialog's see-through parts: a dark backdrop there)
        c.scale(k, k);
        r.draw(c);
        encoder.execute(() -> {
            ByteArrayOutputStream out = new ByteArrayOutputStream(1 << 17);
            b.compress(Bitmap.CompressFormat.JPEG, 82, out);
            b.recycle();
            host.sendUi(out.toByteArray(), w, h);
        });
    }

    /** a touch on the picture there: u, v 0..1 over the whole dialog window */
    void touch(int action, float u, float v) {
        View r = root;
        if (r == null) return;
        long now = android.os.SystemClock.uptimeMillis();
        if (action == MotionEvent.ACTION_DOWN) downTime = now;
        MotionEvent e = MotionEvent.obtain(downTime, now, action, u * r.getWidth(), v * r.getHeight(), 0);
        e.setSource(android.view.InputDevice.SOURCE_TOUCHSCREEN);
        r.dispatchTouchEvent(e);
        e.recycle();
    }
}
