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
import android.widget.FrameLayout;
import android.view.View;
import android.graphics.RectF;
import android.graphics.Canvas;
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
    private boolean fill;    // the GamePad picture takes the display's shape (else 16:9 with bars)

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

    boolean active() { return (screen != null && !screen.tv) || remote != null; }

    /** the GamePad picture goes to a GamePad device (PadHost), not to a display of this device */
    boolean remoteOnly() { return remote != null; }

    // the second display shows the game picture instead (a TV by casting or a cable): this device shows the GamePad's
    private boolean tvPicture;

    /** the game picture on the second display (true) or the GamePad picture (false) */
    void setTvPicture(boolean tv) {
        if (tv == tvPicture) return;
        tvPicture = tv;
        if (screen != null) {  // made again for the other picture
            Screen s = screen;
            screen = null;
            s.dismiss();
            listener.onDrcDisplayChanged(false);
        }
        update();
    }

    /** the game picture is on the second display now */
    boolean tvShown() { return screen != null && screen.tv; }

    // a phone as the GamePad (PadHost): the picture goes into its video encoder's surface instead of a display
    private android.view.Surface remote;
    private float remoteAspect;  // the video's shape

    /** the GamePad picture into `s` (another device as the GamePad), null: back to a display or the main window */
    void setRemote(android.view.Surface s, float aspect) {
        remoteAspect = aspect;
        if (s == remote) return;
        if (remote != null) {
            remote = null;
            Native.drcSurfaceChanged(null);
            applyAspect();
            listener.onDrcDisplayChanged(false);
        }
        if (s != null) {
            if (screen != null) {  // (a second display gives way while the phone is the GamePad)
                Screen old = screen;
                screen = null;
                old.dismiss();
            }
            remote = s;
            Native.drcSurfaceChanged(s);
            applyAspect();
            listener.onDrcDisplayChanged(true);
        }
        update();
    }

    /** show the GamePad picture there whenever possible (false: keep it in the main window) */
    void setWanted(boolean w) {
        wanted = w;
        update();
    }

    /** the GamePad picture in the display's shape (the game draws it so), or 16:9 with bars */
    void setFill(boolean f) {
        fill = f;
        applyAspect();
    }

    private void applyAspect() {
        Screen s = screen;
        if (s != null && s.tv) return;  // (the game picture there: the GamePad picture's shape is the main window's)
        if (remote != null) Native.setDrcAspect(fill ? remoteAspect : 0);  // (a phone as the GamePad)
        else Native.setDrcAspect(fill && s != null && s.w > 0 && s.h > 0 ? s.w / (float) s.h : 0);
    }

    void release() {
        wanted = false;
        update();
        displays.unregisterDisplayListener(this);
    }

    private void update() {
        Display d = wanted && remote == null ? available() : null;
        if (screen != null && (d == null || screen.getDisplay().getDisplayId() != d.getDisplayId())) {
            Screen s = screen;
            screen = null;
            s.dismiss();  // its surface goes: the renderer lets go of the window
            applyAspect();
            listener.onDrcDisplayChanged(false);
        }
        if (screen == null && d != null && remote == null) {
            try {
                Screen s = new Screen(activity, d, this::applyAspect, () -> remote != null, tvPicture);
                // closed by someone else (the system): the GamePad picture goes back to the main window
                s.setOnDismissListener(x -> {
                    if (screen == s) {
                        screen = null;
                        applyAspect();
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
        private final Runnable sizeChanged;
        private final java.util.function.BooleanSupplier remoteActive;
        final boolean tv;  // the game picture (the renderer's TV picture output) instead of the GamePad's

        Screen(Context outer, Display d, Runnable sizeChanged, java.util.function.BooleanSupplier remoteActive, boolean tv) {
            super(outer, d);
            this.sizeChanged = sizeChanged;
            this.remoteActive = remoteActive;
            this.tv = tv;
        }

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
            if (!tv) v.setOnTouchListener((view, e) -> touch(e));
            // over the picture: the progress marks on the game's sea chart (ChartMarks)
            marks = new View(getContext()) {
                private final RectF pic = new RectF();

                @Override
                protected void onDraw(Canvas c) {
                    if (pictureRect(pic)) ChartMarks.I.draw(c, pic, getContext(), this);
                }
            };
            marks.setWillNotDraw(false);
            if (!tv) ChartMarks.I.attach(marks);
            FrameLayout root = new FrameLayout(getContext());
            root.addView(v, new FrameLayout.LayoutParams(-1, -1));
            root.addView(marks, new FrameLayout.LayoutParams(-1, -1));
            setContentView(root);
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
            if (tv) {  // the game picture: it takes this display's shape (renderer)
                Native.tvOutSurfaceChanged(holder.getSurface());
                return;
            }
            if (remoteActive.getAsBoolean()) return;
            w = width;
            h = height;
            Native.drcSurfaceChanged(holder.getSurface());
            sizeChanged.run();
        }

        @Override
        public void surfaceDestroyed(SurfaceHolder holder) {
            if (tv) {
                Native.tvOutSurfaceChanged(null);
                return;
            }
            if (!remoteActive.getAsBoolean()) Native.drcSurfaceChanged(null);  // (else the phone's surface has it now)
        }

        private View marks;

        @Override
        protected void onStop() {
            if (marks != null && !tv) ChartMarks.I.detach(marks);
            super.onStop();
        }

        /** where the picture lies on the display (as touch fits it), false before the surface has a size */
        private boolean pictureRect(RectF out) {
            if (w == 0 || h == 0) return false;
            float pa = Native.drcPictureAspect();
            if (!(pa > 0.1f)) pa = 16f / 9f;
            float pw = w, ph = h;
            if (pw / ph > pa) pw = ph * pa;
            else ph = pw / pa;
            out.set((w - pw) / 2, (h - ph) / 2, (w + pw) / 2, (h + ph) / 2);
            return true;
        }

        // the GamePad picture fills the display in its own shape (16:9, or the display's when the
        // game draws it so), bars where the shapes differ
        private boolean touch(MotionEvent e) {
            if (w == 0 || h == 0) return false;
            float pa = Native.drcPictureAspect();
            if (!(pa > 0.1f)) pa = 16f / 9f;
            float pw = w, ph = h, px = 0, py = 0;
            if (pw / ph > pa) {
                pw = ph * pa;
                px = (w - pw) / 2;
            } else {
                ph = pw / pa;
                py = (h - ph) / 2;
            }
            int a = e.getActionMasked();
            boolean down = a != MotionEvent.ACTION_UP && a != MotionEvent.ACTION_CANCEL;
            float tx = Math.max(0, Math.min(1, (e.getX() - px) / pw)), ty = Math.max(0, Math.min(1, (e.getY() - py) / ph));
            Native.setDrcTouch(down, tx, ty);  // a taller (wider) picture: its 16:9 middle is the touch screen
            // (on the game's 16:9 screen, as ControlsView's: MainActivity tells which chart square a finger opened)
            float pa16 = Native.drcPictureAspect(), kx = pa16 > 16f / 9f ? pa16 / (16f / 9f) : 1f,
                    ky = pa16 > 0.1f && pa16 < 16f / 9f ? (16f / 9f) / pa16 : 1f;
            ControlsView.fingerX = (tx - 0.5f) * kx + 0.5f;
            ControlsView.fingerY = (ty - 0.5f) * ky + 0.5f;
            if (DEBUG) android.util.Log.d("wwhd", "GamePad display touch " + (down ? "down" : "up") + " " + tx + "," + ty);
            return true;
        }
    }
}
