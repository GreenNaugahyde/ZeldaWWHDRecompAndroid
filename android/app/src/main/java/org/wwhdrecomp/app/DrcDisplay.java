package org.wwhdrecomp.app;

import android.app.Presentation;
import android.content.Context;
import android.graphics.Color;
import android.hardware.display.DisplayManager;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.view.Display;
import android.view.MotionEvent;
import android.view.SurfaceHolder;
import android.view.SurfaceView;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.view.WindowManager;

/**
 * The GamePad picture on a second display: the lower screen of dual-screen devices (AYN Thor and
 * the like), or any other presentation display. An Android Presentation on that display holds a
 * surface the renderer presents the GamePad image to (runtime/src/vk/vk_device.cpp,
 * present_drc_window); touches there are the GamePad's touch screen. Without a second display, or
 * with the option off, the GamePad picture stays in the main window's layout.
 */
final class DrcDisplay implements DisplayManager.DisplayListener {
    private static boolean DEBUG;  // debuggable build: log touches
    interface Listener {
        /** the GamePad picture moved to the second display (true) or back to the main window */
        void onDrcDisplayChanged(boolean active);
    }

    private final MainActivity activity;
    private final Listener listener;
    private final DisplayManager displays;
    private Screen screen;
    private boolean wanted;  // the game runs, the activity is visible and the option is on

    DrcDisplay(MainActivity a, Listener l) {
        activity = a;
        listener = l;
        DEBUG = a.debuggable();
        displays = (DisplayManager) a.getSystemService(Context.DISPLAY_SERVICE);
        displays.registerDisplayListener(this, new Handler(Looper.getMainLooper()));
    }

    /** a second display the GamePad picture can use, or null */
    Display available() {
        Display[] d = displays.getDisplays(DisplayManager.DISPLAY_CATEGORY_PRESENTATION);
        for (Display x : d)
            if (x.getDisplayId() != Display.DEFAULT_DISPLAY && x.isValid()) return x;
        return null;
    }

    boolean active() { return screen != null; }

    /** show the GamePad picture there whenever possible (false: keep it in the main window) */
    void setWanted(boolean w) {
        wanted = w;
        update();
    }

    void release() {
        wanted = false;
        update();
        displays.unregisterDisplayListener(this);
    }

    private void update() {
        Display d = wanted ? available() : null;
        if (screen != null && (d == null || screen.getDisplay().getDisplayId() != d.getDisplayId())) {
            Screen s = screen;
            screen = null;
            s.dismiss();  // its surface goes: the renderer lets go of the window
            listener.onDrcDisplayChanged(false);
        }
        if (screen == null && d != null) {
            try {
                Screen s = new Screen(activity, d);
                // closed by someone else (the system): the GamePad picture goes back to the main window
                s.setOnDismissListener(x -> {
                    if (screen == s) {
                        screen = null;
                        listener.onDrcDisplayChanged(false);
                    }
                });
                screen = s;
                s.show();
                listener.onDrcDisplayChanged(true);
            } catch (WindowManager.InvalidDisplayException e) {
                screen = null;  // removed meanwhile
            }
        }
    }

    @Override
    public void onDisplayAdded(int id) { update(); }

    @Override
    public void onDisplayRemoved(int id) { update(); }

    @Override
    public void onDisplayChanged(int id) {}

    /** the presentation: a black window with the picture surface over the whole display */
    private static final class Screen extends Presentation implements SurfaceHolder.Callback {
        private int w, h;

        Screen(Context outer, Display d) { super(outer, d); }

        @Override
        protected void onCreate(Bundle state) {
            super.onCreate(state);
            // touches only: keys and controllers stay with the game's window (a dialog would also
            // take a controller's B as Back and close itself)
            setCancelable(false);
            getWindow().addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON | WindowManager.LayoutParams.FLAG_NOT_FOCUSABLE);
            SurfaceView v = new SurfaceView(getContext());
            v.setBackgroundColor(Color.TRANSPARENT);
            v.getHolder().addCallback(this);
            v.setOnTouchListener((view, e) -> touch(e));
            setContentView(v);
            getWindow().getDecorView().setBackgroundColor(Color.BLACK);
            WindowInsetsController c = getWindow().getInsetsController();
            if (c != null) {
                c.hide(WindowInsets.Type.systemBars());
                c.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
            }
        }

        @Override
        public void surfaceCreated(SurfaceHolder holder) {}

        @Override
        public void surfaceChanged(SurfaceHolder holder, int format, int width, int height) {
            w = width;
            h = height;
            Native.drcSurfaceChanged(holder.getSurface());
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            Native.drcSurfaceChanged(null);
        }

        // the GamePad picture fills the display at 16:9 (bars where the display has another shape)
        private boolean touch(MotionEvent e) {
            if (w == 0 || h == 0) return false;
            float pw = w, ph = h, px = 0, py = 0;
            if (pw / ph > 16f / 9f) {
                pw = ph * 16f / 9f;
                px = (w - pw) / 2;
            } else {
                ph = pw * 9f / 16f;
                py = (h - ph) / 2;
            }
            int a = e.getActionMasked();
            boolean down = a != MotionEvent.ACTION_UP && a != MotionEvent.ACTION_CANCEL;
            float tx = Math.max(0, Math.min(1, (e.getX() - px) / pw)), ty = Math.max(0, Math.min(1, (e.getY() - py) / ph));
            Native.setTouch(down, tx, ty);
            if (DEBUG) android.util.Log.d("wwhd", "GamePad display touch " + (down ? "down" : "up") + " " + tx + "," + ty);
            return true;
        }
    }
}
