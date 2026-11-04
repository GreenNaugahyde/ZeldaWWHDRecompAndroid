package org.wwhdrecomp.app;

import android.app.Dialog;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.GestureDetector;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.ScaleGestureDetector;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.OverScroller;

/**
 * The full map: the whole sea chart on the screen in a Map pictures style of its own (pref fullmap_pictures, Terrain
 * by default; MiniMap.drawSeaChart draws it), pinched or zoomed from all 7 x 7 squares down to an island's ground
 * (5 world units a pixel), dragged and flung about; Link, the boat, the island names and the progress marks as on the
 * mini map. Opened by holding the mini map, by its button on the game's chart in the + menu (ChartMarks) or from
 * Controls > Screen (HUD). The game is paused while it shows. At the top the name of the island in the middle; buttons:
 * pictures, to Link, close, zoom; a controller: the left stick / d-pad move, L / R (the right stick) zoom, Y to Link, X
 * the pictures, B or + close.
 * Indoors (a dungeon, cave, house ...) the stage Link is in instead (MiniMap.drawIndoorChart), in the same pictures: a
 * dungeon's map pictures (Redrawn: drawn anew; Terrain: its ground, the rooms visited in colour, those only on its map
 * grey) or the floor plan of the collision; one floor at a time, Link's first, the others by the floor buttons at the
 * bottom left (ZL / ZR).
 */
final class FullMap extends Dialog {
    private static final float SQUARE = 100000f, SEA = 7 * SQUARE, K_MAX = 1 / 5f;
    private static FullMap shown;

    /** opens it (once) */
    static void open(MainActivity a) {
        if (shown != null && shown.isShowing()) return;
        shown = new FullMap(a);
        shown.show();
    }

    static boolean isOpen() { return shown != null && shown.isShowing(); }

    private final MainActivity a;
    private final ChartView chart;

    private FullMap(MainActivity a) {
        super(a, android.R.style.Theme_Translucent_NoTitleBar_Fullscreen);
        this.a = a;
        chart = new ChartView();
        Window w = getWindow();
        if (w != null) {
            w.setWindowAnimations(0);
            w.getAttributes().layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        }
        setContentView(chart);
    }

    @Override
    protected void onStart() {
        super.onStart();
        Window w = getWindow();
        WindowInsetsController ic = w != null ? w.getInsetsController() : null;
        if (ic != null) {
            ic.hide(WindowInsets.Type.systemBars());
            ic.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        MenuSounds.play(a, MenuSounds.WINDOW_OPEN);
        android.view.Choreographer.getInstance().postFrameCallback(pause);
    }

    @Override
    public void dismiss() {
        if (isShowing()) MenuSounds.play(a, MenuSounds.CLOSE);
        android.view.Choreographer.getInstance().removeFrameCallback(pause);
        if (a.controlsView() != null) a.controlsView().miniMap.detach(chart);
        super.dismiss();
    }

    // the game paused while the map shows (each frame: the pause runs out by itself; the game's own pauses go first)
    private final android.view.Choreographer.FrameCallback pause = new android.view.Choreographer.FrameCallback() {
        @Override
        public void doFrame(long now) {
            if (!isShowing()) return;
            Native.setGamePaused(Native.pauseKind() == 0);
            chart.stickFrame();
            android.view.Choreographer.getInstance().postFrameCallback(this);
        }
    };

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_START
                || (code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_BUTTON_B) && code == InputMapper.menuBack()) {
            dismiss();
            return true;
        }
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_LEFT: chart.step(-1, 0); return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: chart.step(1, 0); return true;
            case KeyEvent.KEYCODE_DPAD_UP: chart.step(0, -1); return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: chart.step(0, 1); return true;
            case KeyEvent.KEYCODE_BUTTON_L2: if (chart.floorStep(-1)) return true;
            // fall through
            case KeyEvent.KEYCODE_BUTTON_L1: chart.zoomBy(1 / 1.8f); return true;
            case KeyEvent.KEYCODE_BUTTON_R2: if (chart.floorStep(1)) return true;
            // fall through
            case KeyEvent.KEYCODE_BUTTON_R1: chart.zoomBy(1.8f); return true;
            case KeyEvent.KEYCODE_BUTTON_Y: chart.toLink(); return true;
            case KeyEvent.KEYCODE_BUTTON_X: chart.nextPictures(); return true;
            default: return super.onKeyDown(code, e);
        }
    }

    @Override
    public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) == InputDevice.SOURCE_JOYSTICK && e.getAction() == MotionEvent.ACTION_MOVE) {
            chart.stick(e.getAxisValue(MotionEvent.AXIS_X), e.getAxisValue(MotionEvent.AXIS_Y), e.getAxisValue(MotionEvent.AXIS_RZ));
            return true;
        }
        return super.onGenericMotionEvent(e);
    }

    private final class ChartView extends View {
        private final float dp = getResources().getDisplayMetrics().density;
        private float wx, wz, k, kFit;  // the world point in the middle, pixels per world unit, k for the whole sea
        private boolean placed;
        private int pictures;
        private final boolean deco;      // Map decorations (pref fullmap_deco): the frame, the compass, the menus' blue
        private final GameUi.GameBackground background;
        private float[] state = new float[72];
        private long stateAt;
        private final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
        private final RectF sea = new RectF(), box = new RectF(), btnClose = new RectF(), btnLink = new RectF(),
                btnPictures = new RectF(), btnIn = new RectF(), btnOut = new RectF(), btnUp = new RectF(), btnDown = new RectF(),
                floorPlate = new RectF();
        // indoors: the stage instead of the sea; its world box (the sea's: all 7 x 7 squares), known once its data is read
        private final boolean indoor;
        private final RectF world = new RectF(-SEA / 2, -SEA / 2, SEA / 2, SEA / 2);
        private boolean worldKnown = true;
        private int floor = -1;          // indoors: the FLOR index shown (-1: the stage has none)
        private boolean floorSet;        // ... set to Link's once the floors are known
        private final OverScroller fling;
        private final ScaleGestureDetector scale;
        private final GestureDetector gestures;
        // an eased move (to Link, a square, a step or zoom): from (fx, fz, fk) to (tx, tz, tk)
        private float fx, fz, fk, tx, tz, tk;
        private long animAt;
        private static final long ANIM_MS = 260;
        // a controller's sticks: move and zoom while held
        private float sx, sy, sz;
        private long stickAt;

        ChartView() {
            super(FullMap.this.getContext());
            pictures = a.prefs.getInt("fullmap_pictures", MiniMap.PICTURES_TERRAIN);
            deco = a.prefs.getBoolean("fullmap_deco", true);
            background = deco ? new GameUi.GameBackground(getContext(), getResources().getDisplayMetrics().density * 0.75f) : null;
            float[] s0 = Native.miniMapState();
            if (s0 != null && s0.length >= 72) state = s0;
            indoor = state[0] == 2;
            if (indoor) {
                // (until the stage's box is known: around Link)
                world.set(state[1] - 2500, state[2] - 2500, state[1] + 2500, state[2] + 2500);
                worldKnown = false;
            }
            text.setTextAlign(Paint.Align.CENTER);
            text.setFakeBoldText(true);
            fling = new OverScroller(getContext());
            scale = new ScaleGestureDetector(getContext(), new ScaleGestureDetector.SimpleOnScaleGestureListener() {
                @Override
                public boolean onScale(ScaleGestureDetector d) {
                    zoomAt(d.getFocusX(), d.getFocusY(), d.getScaleFactor());
                    return true;
                }
            });
            gestures = new GestureDetector(getContext(), new GestureDetector.SimpleOnGestureListener() {
                @Override
                public boolean onDown(MotionEvent e) {
                    fling.forceFinished(true);
                    animAt = 0;
                    return true;
                }

                @Override
                public boolean onScroll(MotionEvent e1, MotionEvent e2, float dx, float dy) {
                    if (scale.isInProgress()) return true;
                    wx += dx / k;
                    wz += dy / k;
                    clamp();
                    invalidate();
                    return true;
                }

                @Override
                public boolean onFling(MotionEvent e1, MotionEvent e2, float vx, float vy) {
                    // (in pixels: the world point in the middle, as pixels at this zoom)
                    fling.fling(Math.round(wx * k), Math.round(wz * k), Math.round(-vx), Math.round(-vy),
                            Math.round(world.left * k), Math.round(world.right * k), Math.round(world.top * k), Math.round(world.bottom * k));
                    postInvalidateOnAnimation();
                    return true;
                }

                @Override
                public boolean onSingleTapConfirmed(MotionEvent e) {
                    tap(e.getX(), e.getY());
                    return true;
                }

                @Override
                public boolean onDoubleTap(MotionEvent e) {
                    // nearly all in: back out to the whole sea; else 2.5 times closer at the finger
                    if (k >= kMax() * 0.8f) animate(world.centerX(), world.centerY(), kFit);
                    else {
                        float nk = Math.min(kMax(), k * 2.5f);
                        float px = wx + (e.getX() - getWidth() / 2f) / k, pz = wz + (e.getY() - getHeight() / 2f) / k;
                        animate(px - (e.getX() - getWidth() / 2f) / nk, pz - (e.getY() - getHeight() / 2f) / nk, nk);
                    }
                    MenuSounds.play(a, MenuSounds.SWITCH);
                    return true;
                }
            });
            setFocusable(true);
            setFocusableInTouchMode(true);
            requestFocus();
        }

        @Override
        protected void onSizeChanged(int w, int h, int ow, int oh) {
            // (a wide screen has the buttons beside the sea; a tall one above and below it)
            fit(!placed);
            placed = true;
        }

        /** k showing the whole world box; place: zoomed out to it, in its middle */
        private void fit(boolean place) {
            int w = getWidth(), h = getHeight();
            if (w == 0 || h == 0) return;
            float margin = (deco && !indoor ? FRAME + 12 : 12) * dp;  // (the frame inside the screen)
            kFit = Math.min((w - 2 * margin) / world.width(), (h - 2 * margin - (w > h ? 0 : 2 * bar())) / world.height());
            if (place) {
                k = kFit;
                wx = world.centerX();
                wz = world.centerY();
            }
            clamp();
        }

        /** the closest zoom: 5 world units a pixel on the sea; indoors 6 times the whole stage, a pixel a unit at least */
        private float kMax() { return indoor ? Math.max(kFit * 6, 1f) : K_MAX; }

        private float bar() { return 52 * dp; }

        private void clamp() {
            k = Math.max(kFit * 0.9f, Math.min(kMax(), k));
            // the sea fills the screen's short side as far as it can (zoomed out: it stays in the middle)
            float half = Math.min(getWidth(), getHeight() - 2 * bar()) / 2f / k * 0.9f;
            float limX = Math.max(0, world.width() / 2 - half), limZ = Math.max(0, world.height() / 2 - half);
            wx = Math.max(world.centerX() - limX, Math.min(world.centerX() + limX, wx));
            wz = Math.max(world.centerY() - limZ, Math.min(world.centerY() + limZ, wz));
        }

        private void zoomAt(float fx, float fy, float f) {
            float px = wx + (fx - getWidth() / 2f) / k, pz = wz + (fy - getHeight() / 2f) / k;
            k = Math.max(kFit * 0.9f, Math.min(kMax(), k * f));
            wx = px - (fx - getWidth() / 2f) / k;
            wz = pz - (fy - getHeight() / 2f) / k;
            clamp();
            invalidate();
        }

        void zoomBy(float f) {
            animate(wx, wz, Math.max(kFit, Math.min(kMax(), k * f)));
            MenuSounds.play(a, MenuSounds.CURSOR);
        }

        void step(int dx, int dy) {
            animate(wx + dx * getWidth() / 4f / k, wz + dy * getHeight() / 4f / k, k);
            MenuSounds.play(a, MenuSounds.CURSOR);
        }

        /** to Link: his square and around it (a third of a square across the screen's short side; Terrain closer) */
        void toLink() {
            if (indoor) {
                // his floor, him in the middle, about 2500 units across
                MiniMap mm = a.controlsView() != null ? a.controlsView().miniMap : null;
                if (mm != null) floor = mm.indoorFloorAt(state[61]);
                float side = Math.min(getWidth(), getHeight() - 2 * bar());
                animate(state[1], state[2], Math.max(k, side / 2500f));
                MenuSounds.play(a, MenuSounds.DECIDE);
                return;
            }
            if (state[0] != 1) return;
            float side = Math.min(getWidth(), getHeight() - 2 * bar());
            animate(state[1], state[2], Math.max(k, side / (pictures == MiniMap.PICTURES_TERRAIN ? SQUARE / 3 : SQUARE)));
            MenuSounds.play(a, MenuSounds.DECIDE);
        }

        void nextPictures() {
            // Terrain, Redrawn, HD, Original
            int[] order = {MiniMap.PICTURES_TERRAIN, MiniMap.PICTURES_REDRAWN, MiniMap.PICTURES_HD, MiniMap.PICTURES_ORIGINAL};
            int i = 0;
            while (i < order.length && order[i] != pictures) i++;
            pictures = order[(i + 1) % order.length];
            a.prefs.edit().putInt("fullmap_pictures", pictures).apply();
            MenuSounds.play(a, MenuSounds.SWITCH);
            invalidate();
        }

        private void animate(float x, float z, float nk) {
            fling.forceFinished(true);
            fx = wx;
            fz = wz;
            fk = k;
            tx = x;
            tz = z;
            tk = Math.max(kFit * 0.9f, Math.min(kMax(), nk));
            animAt = SystemClock.uptimeMillis();
            invalidate();
        }

        /** a controller's sticks (left: move, right up / down: zoom), applied each frame while held */
        void stick(float x, float y, float z) {
            sx = Math.abs(x) > 0.2f ? x : 0;
            sy = Math.abs(y) > 0.2f ? y : 0;
            sz = Math.abs(z) > 0.25f ? z : 0;
        }

        void stickFrame() {
            long t = SystemClock.uptimeMillis();
            float dt = stickAt == 0 ? 0 : Math.min(0.05f, (t - stickAt) / 1000f);
            stickAt = t;
            if (sx == 0 && sy == 0 && sz == 0) return;
            animAt = 0;
            float speed = Math.min(getWidth(), getHeight()) * 0.9f;  // pixels a second at full tilt
            wx += sx * speed * dt / k;
            wz += sy * speed * dt / k;
            if (sz != 0) zoomAt(getWidth() / 2f, getHeight() / 2f, (float) Math.pow(3, -sz * dt));
            clamp();
            invalidate();
        }

        private void tap(float x, float y) {
            if (btnClose.contains(x, y)) { dismiss(); return; }
            if (btnLink.contains(x, y)) { toLink(); return; }
            if (btnPictures.contains(x, y)) { nextPictures(); return; }
            if (btnIn.contains(x, y)) { zoomBy(1.8f); return; }
            if (btnOut.contains(x, y)) { zoomBy(1 / 1.8f); return; }
            if (btnUp.contains(x, y)) { floorStep(1); return; }
            if (btnDown.contains(x, y)) { floorStep(-1); return; }
            // zoomed out: a tap opens that square
            if (!indoor && k < kFit * 2.5f) {
                float px = wx + (x - getWidth() / 2f) / k, pz = wz + (y - getHeight() / 2f) / k;
                int col = (int) Math.floor((px + SEA / 2) / SQUARE), row = (int) Math.floor((pz + SEA / 2) / SQUARE);
                if (col < 0 || col > 6 || row < 0 || row > 6) return;
                // (Terrain: half a square, its islands are at their true size, smaller than the square)
                float side = Math.min(getWidth(), getHeight() - 2 * bar());
                animate((col - 3) * SQUARE, (row - 3) * SQUARE, side / SQUARE * (pictures == MiniMap.PICTURES_TERRAIN ? 2f : 0.96f));
                MenuSounds.play(a, MenuSounds.DECIDE);
            }
        }

        /** indoors: the floor above (+1) or below (-1); false when there is none that way (or on the sea) */
        boolean floorStep(int d) {
            int[] fl = floors();
            if (fl == null || floor < 0 || floor + d < 0 || floor + d >= fl.length) return false;
            floor += d;
            MenuSounds.play(a, MenuSounds.CURSOR);
            invalidate();
            return true;
        }

        private int[] floors() {
            MiniMap mm = indoor && a.controlsView() != null ? a.controlsView().miniMap : null;
            int[] fl = mm != null ? mm.indoorFloors() : null;
            return fl != null && fl.length > 1 ? fl : null;
        }

        @Override
        public boolean onTouchEvent(MotionEvent e) {
            scale.onTouchEvent(e);
            gestures.onTouchEvent(e);
            return true;
        }

        @Override
        protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            if (w == 0 || h == 0) return;
            long t = SystemClock.uptimeMillis();
            if (t - stateAt > 250) {
                float[] s = Native.miniMapState();
                if (s != null && s.length >= 72) state = s;
                stateAt = t;
            }
            if (fling.computeScrollOffset()) {
                wx = fling.getCurrX() / k;
                wz = fling.getCurrY() / k;
                clamp();
                postInvalidateOnAnimation();
            } else if (animAt != 0) {
                float f = Math.min(1, (t - animAt) / (float) ANIM_MS);
                float e = 1 - (1 - f) * (1 - f) * (1 - f);
                // (the zoom eased in its log, so it looks even)
                k = (float) (fk * Math.pow(tk / fk, e));
                wx = fx + (tx - fx) * e;
                wz = fz + (tz - fz) * e;
                if (f >= 1) animAt = 0;
                clamp();
                postInvalidateOnAnimation();
            }
            if (background != null) {
                background.setBounds(0, 0, w, h);
                background.draw(c);
            } else c.drawColor(0xFF10263A);
            float cx = w / 2f, cy = h / 2f;
            if (indoor) {
                drawIndoor(c, w, h, cx, cy);
                drawBar(c, w, h);
                return;
            }
            sea.set(cx - (SEA / 2 + wx) * k, cy - (SEA / 2 + wz) * k, cx + (SEA / 2 - wx) * k, cy + (SEA / 2 - wz) * k);
            box.set(sea);
            if (box.intersect(0, 0, w, h)) {
                MiniMap mm = a.controlsView() != null ? a.controlsView().miniMap : null;
                if (mm != null) {
                    mm.attach(this);
                    c.save();
                    c.clipRect(box);
                    float side = Math.min(w, h) * 0.3f;
                    mm.seaGrain = deco;
                    mm.drawSeaChart(c, state, pictures, a.prefs.getBoolean("minimap_names", true), box,
                            wx + (box.centerX() - cx) / k, wz + (box.centerY() - cy) / k, k, side, 0xFF);
                    mm.seaGrain = false;
                    c.restore();
                }
            }
            MiniMap mm = a.controlsView() != null ? a.controlsView().miniMap : null;
            if (deco && mm != null) {
                drawFrame(c, mm);
                drawCompass(c, mm, h);
            } else {
                line.setStyle(Paint.Style.STROKE);
                line.setStrokeWidth(3 * dp);
                line.setColor(MiniMap.EDGE);
                c.drawRect(sea, line);
            }
            drawBar(c, w, h);
        }

        // ---- Map decorations: a paper frame round the sea as on the game's charts, the corners MenuMap_00's ornament
        // (MapDeco_00, mirrored), the squares' letters A-G along the top and bottom, numbers 1-7 down the sides; the
        // wind dial with the wind's arrow at the bottom left (MiniMap.drawCompass)
        private static final float FRAME = 24;  // the frame's width (dp)
        private final RectF outer = new RectF(), orn = new RectF();
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final android.graphics.Matrix tm = new android.graphics.Matrix();
        private final android.graphics.Path band = new android.graphics.Path();

        private void drawFrame(Canvas c, MiniMap mm) {
            float f = FRAME * dp;
            outer.set(sea);
            outer.inset(-f, -f);
            // its shadow on the blue, then the paper band (the chart's paper, moving with the map)
            line.setStyle(Paint.Style.FILL);
            line.setColor(0x60000000);
            c.drawRect(outer.left + 3 * dp, outer.top + 4 * dp, outer.right + 3 * dp, outer.bottom + 4 * dp, line);
            band.reset();
            band.setFillType(android.graphics.Path.FillType.EVEN_ODD);
            band.addRect(outer, android.graphics.Path.Direction.CW);
            band.addRect(sea, android.graphics.Path.Direction.CW);
            android.graphics.Bitmap tile = mm.seaPaper();
            if (tile != null) {
                android.graphics.BitmapShader sh = new android.graphics.BitmapShader(tile,
                        android.graphics.Shader.TileMode.MIRROR, android.graphics.Shader.TileMode.MIRROR);
                float ts = 90 * dp / tile.getWidth();
                tm.setScale(ts, ts);
                tm.postTranslate(outer.left, outer.top);
                sh.setLocalMatrix(tm);
                fill.setShader(sh);
            } else {
                fill.setShader(null);
                fill.setColor(0xFFF2DDA4);
            }
            c.drawPath(band, fill);
            // its lines: the outer edge, a fine one inside it, the sea's edge
            line.setStyle(Paint.Style.STROKE);
            line.setColor(MiniMap.EDGE);
            line.setStrokeWidth(2.5f * dp);
            c.drawRect(outer, line);
            line.setStrokeWidth(2 * dp);
            c.drawRect(sea, line);
            line.setStrokeWidth(1 * dp);
            line.setAlpha(0xA0);
            c.drawRect(outer.left + 4 * dp, outer.top + 4 * dp, outer.right - 4 * dp, outer.bottom - 4 * dp, line);
            line.setAlpha(0xFF);
            // the squares' letters and numbers
            float sq = sea.width() / 7, ts = f * 0.6f;
            text.setTextSize(ts);
            text.setTypeface(android.graphics.Typeface.SERIF);
            text.setColor(0xFF3C2A10);
            float ty = ts * 0.36f;
            for (int i = 0; i < 7; i++) {
                String col = String.valueOf((char) ('A' + i)), row = String.valueOf(i + 1);
                float x = sea.left + (i + 0.5f) * sq, y = sea.top + (i + 0.5f) * sq;
                c.drawText(col, x, (outer.top + sea.top) / 2 + ty, text);
                c.drawText(col, x, (outer.bottom + sea.bottom) / 2 + ty, text);
                c.drawText(row, (outer.left + sea.left) / 2, y + ty, text);
                c.drawText(row, (outer.right + sea.right) / 2, y + ty, text);
            }
            text.setTypeface(android.graphics.Typeface.DEFAULT);
            // the corners' ornament (drawn as the top left one)
            android.graphics.Bitmap deco = mm.texture("MapDeco_00^t", 0x3C2A10, 0x5A3C14, 0xFF);
            if (deco != null) {
                float oh = f * 1.6f, ow = oh * deco.getWidth() / deco.getHeight();
                fill.setShader(null);
                fill.setAlpha(0xFF);
                for (int q = 0; q < 4; q++) {
                    boolean right = q % 2 == 1, bottom = q >= 2;
                    float x = right ? outer.right : outer.left, y = bottom ? outer.bottom : outer.top;
                    c.save();
                    c.scale(right ? -1 : 1, bottom ? -1 : 1, x, y);
                    orn.set(x - f * 0.25f, y - f * 0.25f, x - f * 0.25f + ow, y - f * 0.25f + oh);
                    c.drawBitmap(deco, null, orn, fill);
                    c.restore();
                }
            }
        }

        /** the wind dial at the bottom left, its arrow the way the wind blows now */
        private void drawCompass(Canvas c, MiniMap mm, int h) {
            float rad = 34 * dp, left = 14 * dp;
            android.view.WindowInsets wi = getRootWindowInsets();
            if (wi != null) left += wi.getInsets(WindowInsets.Type.displayCutout()).left;
            float x = left + rad, y = h - 14 * dp - rad;
            line.setStyle(Paint.Style.FILL);
            line.setColor(0x50000000);
            c.drawCircle(x + 2 * dp, y + 3 * dp, rad * 1.02f, line);
            mm.drawCompass(c, x, y, rad, state[70], 0, 0xFF);
        }

        /** indoors: the stage on the shown floor, over the whole screen; once its data is read Link's floor and its box */
        private void drawIndoor(Canvas c, int w, int h, float cx, float cy) {
            MiniMap mm = a.controlsView() != null ? a.controlsView().miniMap : null;
            if (mm == null) return;
            mm.attach(this);
            box.set(0, 0, w, h);
            if (!floorSet && mm.indoorReady()) {
                floorSet = true;
                floor = mm.indoorFloorAt(state[61]);
            }
            if (!mm.drawIndoorChart(c, state, pictures, floor, box, wx, wz, k, Math.min(w, h) * 0.3f, 0xFF)) return;
            if (!worldKnown) {
                RectF r = mm.indoorBounds();
                if (r != null && r.width() > 0 && r.height() > 0) {
                    worldKnown = true;
                    r.inset(-r.width() * 0.04f - 200, -r.height() * 0.04f - 200);
                    // (Link inside it)
                    r.union(state[1], state[2]);
                    world.set(r);
                    fit(animAt == 0 && fling.isFinished());
                    invalidate();
                }
            }
        }

        /** the name of the island in the middle (once its square is charted) at the top, the buttons right of it and at the
         *  bottom right */
        private void drawBar(Canvas c, int w, int h) {
            float b = 44 * dp, gap = 10 * dp, top = 8 * dp;
            float right = w - 12 * dp;
            android.view.WindowInsets wi = getRootWindowInsets();
            if (wi != null) {
                android.graphics.Insets cut = wi.getInsets(WindowInsets.Type.displayCutout());
                right -= cut.right;
            }
            btnClose.set(right - b, top, right, top + b);
            btnLink.set(btnClose.left - gap - b, top, btnClose.left - gap, top + b);
            btnPictures.set(btnLink.left - gap - b * 2.6f, top, btnLink.left - gap, top + b);
            btnIn.set(right - b, h - top - b, right, h - top);
            btnOut.set(btnIn.left - gap - b, h - top - b, btnIn.left - gap, h - top);
            if (indoor ? state[0] != 2 : state[0] != 1) btnLink.setEmpty();
            // indoors: the floor shown between its buttons (bottom left)
            int[] fl = floors();
            if (fl != null && floor >= 0 && floor < fl.length) {
                float left = 12 * dp;
                if (wi != null) left += wi.getInsets(WindowInsets.Type.displayCutout()).left;
                btnDown.set(left, h - top - b, left + b, h - top);
                floorPlate.set(btnDown.right + gap, h - top - b, btnDown.right + gap + b * 1.4f, h - top);
                btnUp.set(floorPlate.right + gap, h - top - b, floorPlate.right + gap + b, h - top);
                plate(c, floorPlate, b / 2);
                int n = fl[floor];
                String label = n >= 0 ? (n + 1) + "F" : "B" + (-n);
                text.setTextSize(18 * dp);
                text.setColor(0xFF3C2A10);
                c.drawText(label, floorPlate.centerX(), floorPlate.centerY() + 6.5f * dp, text);
                for (int i = 0; i < 2; i++) {
                    RectF r = i == 0 ? btnDown : btnUp;
                    boolean can = i == 0 ? floor > 0 : floor < fl.length - 1;
                    plate(c, r, b / 2);
                    float s = b * 0.2f, d = i == 0 ? 1 : -1;
                    android.graphics.Path tri = new android.graphics.Path();
                    tri.moveTo(r.centerX() - s, r.centerY() - s * 0.5f * d);
                    tri.lineTo(r.centerX() + s, r.centerY() - s * 0.5f * d);
                    tri.lineTo(r.centerX(), r.centerY() + s * 0.7f * d);
                    tri.close();
                    line.setStyle(Paint.Style.FILL);
                    line.setColor(can ? 0xFF3C2A10 : 0x553C2A10);
                    c.drawPath(tri, line);
                }
            } else {
                btnUp.setEmpty();
                btnDown.setEmpty();
            }
            // the title: the square in the middle
            int col = (int) Math.floor((wx + SEA / 2) / SQUARE), row = (int) Math.floor((wz + SEA / 2) / SQUARE);
            String title = null;
            if (!indoor && col >= 0 && col <= 6 && row >= 0 && row <= 6 && k > kFit * 1.6f) {
                int room = row * 7 + col + 1;
                title = state[11 + room] != 0 ? GameText.islandName(room) : null;
                if (title == null && state[11 + room] != 0) GameText.prepare(getContext(), this::postInvalidate);
                // Map decorations: the square as the frame names it (also uncharted), the frame being off the screen
                if (deco) {
                    String sq = (char) ('A' + col) + String.valueOf(row + 1);
                    title = title != null ? title + "  ·  " + sq : sq;
                }
            }
            if (title != null) {
                text.setTextSize(20 * dp);
                text.setTextAlign(Paint.Align.LEFT);
                float tw = text.measureText(title), left = 12 * dp;
                if (wi != null) left += wi.getInsets(WindowInsets.Type.displayCutout()).left;
                box.set(left, top, left + tw + 28 * dp, top + b);
                plate(c, box, b / 2);
                text.setColor(0xFF3C2A10);
                c.drawText(title, left + 14 * dp, top + b / 2 + 7 * dp, text);
                text.setTextAlign(Paint.Align.CENTER);
            }
            plate(c, btnClose, b / 2);
            glyphX(c, btnClose);
            if (!btnLink.isEmpty()) {
                plate(c, btnLink, b / 2);
                line.setStyle(Paint.Style.STROKE);
                line.setStrokeWidth(3 * dp);
                line.setColor(0xFFC81E1E);
                float r = b * 0.22f;
                c.drawCircle(btnLink.centerX(), btnLink.centerY(), r, line);
                line.setStyle(Paint.Style.FILL);
                c.drawCircle(btnLink.centerX(), btnLink.centerY(), r * 0.4f, line);
            }
            plate(c, btnPictures, b / 2);
            String[] names = getContext().getResources().getStringArray(R.array.minimap_pictures);
            String p = pictures < names.length ? names[pictures] : "";
            text.setTextSize(15 * dp);
            float tw = text.measureText(p);
            if (tw > btnPictures.width() * 0.84f) text.setTextSize(15 * dp * btnPictures.width() * 0.84f / tw);
            text.setColor(0xFF3C2A10);
            c.drawText(p, btnPictures.centerX(), btnPictures.centerY() + text.getTextSize() * 0.35f, text);
            plate(c, btnOut, b / 2);
            plate(c, btnIn, b / 2);
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(3.5f * dp);
            line.setStrokeCap(Paint.Cap.ROUND);
            line.setColor(0xFF3C2A10);
            float s = b * 0.22f;
            c.drawLine(btnOut.centerX() - s, btnOut.centerY(), btnOut.centerX() + s, btnOut.centerY(), line);
            c.drawLine(btnIn.centerX() - s, btnIn.centerY(), btnIn.centerX() + s, btnIn.centerY(), line);
            c.drawLine(btnIn.centerX(), btnIn.centerY() - s, btnIn.centerX(), btnIn.centerY() + s, line);
            line.setStrokeCap(Paint.Cap.BUTT);
        }

        // a plate as the mini map's buttons: paper in a brown edge, a soft shadow
        private void plate(Canvas c, RectF r, float round) {
            line.setStyle(Paint.Style.FILL);
            line.setColor(0x55000000);
            c.drawRoundRect(r.left + 1.5f * dp, r.top + 2.5f * dp, r.right + 1.5f * dp, r.bottom + 2.5f * dp, round, round, line);
            line.setColor(0xF0FFF4D8);
            c.drawRoundRect(r, round, round, line);
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(2.5f * dp);
            line.setColor(MiniMap.EDGE);
            c.drawRoundRect(r, round, round, line);
        }

        private void glyphX(Canvas c, RectF r) {
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(3.5f * dp);
            line.setStrokeCap(Paint.Cap.ROUND);
            line.setColor(0xFF3C2A10);
            float s = r.width() * 0.2f;
            c.drawLine(r.centerX() - s, r.centerY() - s, r.centerX() + s, r.centerY() + s, line);
            c.drawLine(r.centerX() - s, r.centerY() + s, r.centerX() + s, r.centerY() - s, line);
            line.setStrokeCap(Paint.Cap.BUTT);
        }
    }
}
