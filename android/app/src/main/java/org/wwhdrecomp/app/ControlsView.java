package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.HapticFeedbackConstants;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Locale;

/**
 * Touch controls made for a phone, drawn over the game: a floating left stick, the camera on swipes
 * over the right side, context buttons whose icons follow the game (A, B, ZR, the item slots X/Y/R),
 * lock-on, first person, the D-pad, pause, and one-touch combat moves (macros of the GamePad
 * combinations). Every control can be moved and resized in the layout editor.
 * Touches that hit no control and land on the GamePad image go to the game as touch-panel input.
 */
final class ControlsView extends View {
    interface Listener {
        void onControlsChanged();
        void onMenu();
        /** the performance overlay was dragged: its top left corner as fractions of the view size */
        void onOverlayMoved(float fx, float fy);
        /** the layout editor finished: the layout to keep ("" = the default one) */
        void onLayoutSaved(String layout);
    }

    private static final int K_STICK = 0, K_BUTTON = 1, K_MACRO = 3, K_MENU = 4, K_EDIT = 5;
    // combat moves (K_MACRO controls' bit field)
    private static final int M_JUMP = 1, M_VERTICAL = 2, M_SPIN = 3, M_DODGE = 4;

    // game state from Native.hudState(): {flags, A action, B action, ZR action, X item, Y item, R item}
    static final int HUD_KNOWN = 1, HUD_ON_BOAT = 2, HUD_SWORD_OUT = 4, HUD_TARGETING = 8, HUD_FIRST_PERSON = 16,
            HUD_HAS_SWORD = 32, HUD_HAS_SHIELD = 64, HUD_HAS_BATON = 128, HUD_HAS_GRAPPLE = 256, HUD_HAS_BOMBS = 512;
    private static final int HUD_FLAGS = 0, HUD_A = 1, HUD_B = 2, HUD_ZR = 3, HUD_X = 4, HUD_Y = 5, HUD_R = 6;

    private static final class Ctl {
        final String id, label, icon;
        final int kind, bit, color;
        // default place: centre = (ax * W + ox * u, ay * H + oy * u), diameter d * u
        final float ax, ox, ay, oy, d;
        // the user's place (layout editor): centre as fractions of the view, diameter in u; NaN = default
        float fx = Float.NaN, fy = Float.NaN, fd = Float.NaN;
        float cx, cy, r;            // on screen
        boolean pressed;
        Ctl(String id, int kind, int bit, int color, String label, String icon, float ax, float ox, float ay, float oy, float d) {
            this.id = id;
            this.kind = kind;
            this.bit = bit;
            this.color = color;
            this.label = label;
            this.icon = icon;
            this.ax = ax;
            this.ox = ox;
            this.ay = ay;
            this.oy = oy;
            this.d = d;
        }
        boolean hit(float x, float y, float slack) {
            float rr = r * slack;
            return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= rr * rr;
        }
        boolean custom() { return !Float.isNaN(fx); }
        boolean removed;  // taken off the screen in the layout editor
        boolean removable() { return kind != K_MENU && kind != K_EDIT; }  // the way back to the menus stays
    }

    private final Listener listener;
    private final TouchIcons icons;
    private final List<Ctl> controls = new ArrayList<>();
    private final HashMap<String, Ctl> byId = new HashMap<>();
    private final SparseArray<Ctl> pointers = new SparseArray<>();
    private int drcPointer = -1;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF tmp = new RectF();

    // GamePad image on screen (letterboxed), for touch input; empty if hidden
    private final RectF drcRect = new RectF();
    private boolean controlsVisible = true;
    private float scale = 1f, opacity = 0.45f;
    private float unit = 1, baseUnit = 1;  // u (with the size setting) and without it

    // settings
    private float cameraSensitivity = 1f;
    private boolean haptics = true;
    private int combatMode;  // 0 auto (with the sword out or a target locked), 1 always, 2 never

    ControlsView(Context c, Listener l) {
        super(c);
        listener = l;
        icons = new TouchIcons(c);
        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        final int T = TouchIcons.TEAL, B = TouchIcons.BONE, O = TouchIcons.ORANGE, S = TouchIcons.SKY;
        add(new Ctl("stick", K_STICK, 0, S, "", "stick_base", 0, 2.0f, 1, -1.9f, 2.2f));
        // the D-pad as four separate buttons: baton, cannon, salvage hook, down
        add(new Ctl("dpad_up", K_BUTTON, Native.UP, T, "↑", "dpad_up_wind_waker", 0, 1.7f, 0, 2.1f, 0.85f));
        add(new Ctl("dpad_left", K_BUTTON, Native.LEFT, T, "←", "dpad_left_cannon", 0, 1.05f, 0, 2.75f, 0.85f));
        add(new Ctl("dpad_right", K_BUTTON, Native.RIGHT, T, "→", "dpad_right_grapple", 0, 2.35f, 0, 2.75f, 0.85f));
        add(new Ctl("dpad_down", K_BUTTON, Native.DOWN, T, "↓", "dpad_down", 0, 1.7f, 0, 3.4f, 0.85f));
        add(new Ctl("zl", K_BUTTON, Native.ZL, TouchIcons.VIOLET, "ZL", "zl_target", 0, 1.1f, 0, 0.9f, 1.2f));
        add(new Ctl("zr", K_BUTTON, Native.ZR, TouchIcons.AMBER, "ZR", "zr_shield", 1, -1.1f, 0, 0.9f, 1.2f));
        add(new Ctl("fp", K_BUTTON, Native.STICK_R, B, "◉", "btn_first_person", 1, -2.6f, 0, 0.9f, 0.9f));
        add(new Ctl("a", K_BUTTON, Native.A, TouchIcons.GREEN, "A", "bubble_blank_a", 1, -1.3f, 1, -1.9f, 1.5f));
        add(new Ctl("b", K_BUTTON, Native.B, TouchIcons.RED, "B", "b_sword", 1, -2.9f, 1, -1.2f, 1.25f));
        add(new Ctl("x", K_BUTTON, Native.X, S, "X", "bubble_empty_item", 1, -0.9f, 1, -3.7f, 1.05f));
        add(new Ctl("y", K_BUTTON, Native.Y, S, "Y", "bubble_empty_item", 1, -2.3f, 1, -3.4f, 1.05f));
        add(new Ctl("r", K_BUTTON, Native.R, S, "R", "bubble_empty_item", 1, -3.6f, 1, -2.6f, 1.05f));
        add(new Ctl("jump", K_MACRO, M_JUMP, O, "⤒", "combat_jump_attack", 1, -3.8f, 1, -4.3f, 0.95f));
        add(new Ctl("vertical", K_MACRO, M_VERTICAL, O, "⇓", "combat_vertical_slash", 1, -4.7f, 1, -3.3f, 0.95f));
        add(new Ctl("spin", K_MACRO, M_SPIN, O, "⟳", "combat_spin_attack", 1, -4.9f, 1, -2.1f, 0.95f));
        add(new Ctl("dodge", K_MACRO, M_DODGE, O, "⇆", "combat_dodge", 1, -4.4f, 1, -0.9f, 0.95f));
        add(new Ctl("pause", K_BUTTON, Native.PLUS, B, "❚❚", "btn_pause", 0.5f, -0.9f, 1, -0.5f, 0.75f));
        add(new Ctl("menu", K_MENU, 0, B, "≡", "btn_menu", 0.5f, 0, 1, -0.5f, 0.75f));
        add(new Ctl("edit", K_EDIT, 0, B, "✎", "btn_layout_edit", 0.5f, 0.9f, 1, -0.5f, 0.75f));
    }

    private void add(Ctl c) {
        controls.add(c);
        byId.put(c.id, c);
    }

    void setDrcRect(RectF r) {
        if (r == null) drcRect.setEmpty();
        else drcRect.set(r);
    }

    // ---- stamina wheel of the "climb any wall" mod (as runtime/src/mods/climb_hud.mm draws it on macOS)
    private final RectF tvRect = new RectF();
    private boolean climbHud;
    private final Paint hudPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    /** where the TV picture is (letterboxed), for overlays on the game image */
    void setTvRect(RectF r) {
        if (r == null) tvRect.setEmpty();
        else tvRect.set(r);
    }

    private float[] hud = {0, 0, 0};
    // the game updates the stamina 30 times a second: poll at that rate, redraw only on changes
    private final Runnable pollHud = new Runnable() {
        @Override
        public void run() {
            if (!climbHud) return;
            float[] h = Native.climbHud();
            if (!java.util.Arrays.equals(h, hud)) {
                hud = h;
                invalidate();
            }
            postDelayed(this, 33);
        }
    };

    /** the climb mod is on: poll its stamina */
    void setClimbHud(boolean on) {
        if (on == climbHud) return;
        climbHud = on;
        removeCallbacks(pollHud);
        if (on) post(pollHud);
        hud = new float[] {0, 0, 0};
        invalidate();
    }

    private void drawClimbHud(Canvas canvas) {
        if (!climbHud || tvRect.isEmpty()) return;
        float[] h = hud;
        float stamina = h[0], alpha = h[1];
        boolean exhausted = h[2] > 0.5f;
        if (alpha <= 0) return;
        // to the upper right of the screen centre, where the follow camera keeps Link
        float cx = tvRect.left + 0.60f * tvRect.width(), cy = tvRect.top + 0.38f * tvRect.height();
        float rad = 0.05f * tvRect.height();
        hudPaint.setStyle(Paint.Style.STROKE);
        // dark outline, then the empty ring, then the filled part clockwise from the top
        hudPaint.setStrokeWidth(rad * 0.5f);
        hudPaint.setColor(0xFF000000);
        hudPaint.setAlpha((int) (255 * 0.45f * alpha));
        canvas.drawCircle(cx, cy, rad * 0.75f, hudPaint);
        hudPaint.setStrokeWidth(rad * 0.34f);
        hudPaint.setColor(exhausted ? 0xFF8C140F : 0xFF1A1A1A);
        hudPaint.setAlpha((int) (255 * 0.45f * alpha));
        tmp.set(cx - rad * 0.75f, cy - rad * 0.75f, cx + rad * 0.75f, cy + rad * 0.75f);
        canvas.drawArc(tmp, -90 + 360 * stamina, 360 * (1 - stamina), false, hudPaint);
        int full;
        if (exhausted) full = 0xFFF2331F;
        else if (stamina < 0.3f) full = mix(0xFFF2331F, 0xFFFFCC26, stamina / 0.3f);
        else full = 0xFF4DE659;
        hudPaint.setColor(full);
        hudPaint.setAlpha((int) (255 * 0.95f * alpha));
        canvas.drawArc(tmp, -90, 360 * stamina, false, hudPaint);
    }

    // ---- performance overlay: frame rate, frame time, CPU/GPU load and temperatures (PerfStats)
    private PerfStats perf;
    private final Paint perfText = new Paint(Paint.ANTI_ALIAS_FLAG), perfBox = new Paint();
    private final Runnable pollPerf = new Runnable() {
        @Override
        public void run() {
            if (perf == null) return;
            perf.update();
            invalidate();
            postDelayed(this, 500);
        }
    };

    // what it shows (bits) and where (top left corner as fractions of the view; negative: default spot)
    static final int PERF_FPS = 1, PERF_FRAME = 2, PERF_CPU = 4, PERF_GPU = 8, PERF_TEMP_CPU = 16, PERF_TEMP_GPU = 32,
            PERF_TEMP_BAT = 64, PERF_SETTINGS = 128, PERF_ALL = 255;
    /** the settings lines (resolution, effects, GPU driver), from the activity */
    java.util.function.Supplier<java.util.List<String>> perfSettings;
    private int perfItems = PERF_ALL;
    private float perfFx = -1, perfFy = -1;
    private final android.graphics.RectF perfRect = new android.graphics.RectF();  // as last drawn
    private boolean perfMoveMode;
    private int perfDragPointer = -1;
    private float perfDragDx, perfDragDy;

    void setPerfHud(boolean on) {
        if (on == (perf != null)) return;
        removeCallbacks(pollPerf);
        perf = on ? new PerfStats(getContext()) : null;
        if (on) post(pollPerf);
        if (!on) perfMoveMode = false;
        invalidate();
    }

    /** a new average frame rate from now on (e.g. another resolution) */
    void resetPerfAverage() {
        PerfStats p = perf;
        if (p != null) p.resetAverage();
        invalidate();
    }

    void setPerfItems(int items) {
        perfItems = items;
        invalidate();
    }

    void setPerfPosition(float fx, float fy) {
        perfFx = fx;
        perfFy = fy;
        invalidate();
    }

    /** move mode: the overlay can be dragged even where it covers a control; a touch elsewhere ends it */
    void setPerfMoveMode(boolean on) {
        perfMoveMode = on && perf != null;
        invalidate();
    }

    /** a touch going down at (x, y): true if it grabs the overlay (or ends move mode) */
    private boolean perfTouchDown(int id, float x, float y) {
        if (perf == null || perfDragPointer >= 0) return false;
        boolean onOverlay = perfRect.contains(x, y);
        if (onOverlay && (perfMoveMode || controlAt(x, y) == null)) {
            perfDragPointer = id;
            perfDragDx = x - perfRect.left;
            perfDragDy = y - perfRect.top;
            return true;
        }
        if (perfMoveMode && !onOverlay) {  // done moving; this touch only ends the mode
            perfMoveMode = false;
            invalidate();
            return true;
        }
        return false;
    }

    private void perfDrag(float x, float y) {
        float w = perfRect.width(), h = perfRect.height();
        float left = Math.max(0, Math.min(getWidth() - w, x - perfDragDx));
        float top = Math.max(0, Math.min(getHeight() - h, y - perfDragDy));
        perfFx = left / Math.max(1, getWidth());
        perfFy = top / Math.max(1, getHeight());
        invalidate();
    }

    // a number with its unit, or "–" without one when it is unknown
    private static String unit(float v, String fmt, String u) {
        String n = num(v, fmt);
        return n.equals("–") ? n : n + u;
    }

    private static String num(float v, String fmt) { return Float.isNaN(v) || v < 0 ? "–" : String.format(java.util.Locale.ROOT, fmt, v); }

    private void drawPerfHud(Canvas canvas) {
        PerfStats p = perf;
        if (p == null) return;
        int it = perfItems;
        // measurements, then (below a separator line) the device: chip, GPU and driver
        java.util.List<String> list = new java.util.ArrayList<>();
        if ((it & PERF_FPS) != 0)
            list.add("FPS: " + num(p.gameFps, "%.1f") + "  Avg: " + num(p.avgFps, "%.1f")
                    + (Math.abs(p.shownFps - p.gameFps) > 0.5f ? "  Shown: " + num(p.shownFps, "%.0f") : ""));
        if ((it & PERF_FRAME) != 0)
            list.add("Frame: " + unit(p.frameMs, "%.1f", " ms") + "  Max: " + unit(p.frameMaxMs, "%.1f", " ms"));
        StringBuilder load = new StringBuilder();
        if ((it & PERF_CPU) != 0) load.append("CPU: ").append(unit(p.cpuPercent, "%.0f", "%")).append("  ");
        if ((it & PERF_GPU) != 0) {
            load.append("GPU: ").append(unit(p.gpuPercent, "%.0f", "%")).append("  ");
            if (p.fgGpuMs > 0) load.append("FG: ").append(unit(p.fgGpuMs, "%.1f", " ms"));
        }
        if (load.length() > 0) list.add(load.toString().trim());
        StringBuilder temp = new StringBuilder();
        if ((it & PERF_TEMP_CPU) != 0) temp.append("CPU ").append(unit(p.cpuTemp, "%.0f", " °C")).append("  ");
        if ((it & PERF_TEMP_GPU) != 0) temp.append("GPU ").append(unit(p.gpuTemp, "%.0f", " °C")).append("  ");
        if ((it & PERF_TEMP_BAT) != 0) temp.append("Bat ").append(unit(p.batteryTemp, "%.0f", " °C"));
        if (temp.length() > 0) list.add("Temp: " + temp.toString().trim());
        boolean settings = (it & PERF_SETTINGS) != 0;
        if (settings && perfSettings != null) list.addAll(perfSettings.get());
        java.util.List<String> device = new java.util.ArrayList<>();
        String chip = (it & PERF_CPU) != 0 ? PerfStats.socName() : "", gpu = (it & PERF_GPU) != 0 ? PerfStats.gpuName() : "";
        if (!chip.isEmpty() || !gpu.isEmpty())
            device.add(((chip.isEmpty() ? "" : "SoC: " + chip + "  ") + (gpu.isEmpty() ? "" : "GPU: " + gpu)).trim());
        String driver = settings ? Native.gpuDriverInfo() : "";
        if (!driver.isEmpty()) device.add("Driver: " + MainActivity.shortDriverName(driver));
        if (!list.isEmpty() && !device.isEmpty()) list.add(null);  // the separator line
        list.addAll(device);
        if (perfMoveMode) list.add(getContext().getString(R.string.perf_move_hint));
        if (list.isEmpty()) {
            perfRect.setEmpty();
            return;
        }
        String[] lines = list.toArray(new String[0]);
        float density = getResources().getDisplayMetrics().density;
        perfText.setTextSize(13 * density);
        perfText.setTypeface(android.graphics.Typeface.MONOSPACE);
        perfText.setColor(0xFFFFFFFF);
        perfBox.setColor(0x99000000);
        float pad = 6 * density, lineH = perfText.getFontSpacing(), w = 0;
        for (String l : lines) if (l != null) w = Math.max(w, perfText.measureText(l));
        float bw = w + 2 * pad, bh = lines.length * lineH + 2 * pad;
        // default: below the top edge's system overlays, at the left; else where the user put it
        float x = perfFx < 0 ? 12 * density : perfFx * getWidth(), y = perfFy < 0 ? 40 * density : perfFy * getHeight();
        x = Math.max(0, Math.min(getWidth() - bw, x));
        y = Math.max(0, Math.min(getHeight() - bh, y));
        tmp.set(x, y, x + bw, y + bh);
        perfRect.set(tmp);
        canvas.drawRoundRect(tmp, 4 * density, 4 * density, perfBox);
        if (perfMoveMode || perfDragPointer >= 0) {  // being moved: a frame around it
            stroke.setColor(0xFFFFCC26);
            stroke.setAlpha(255);
            canvas.drawRoundRect(tmp, 4 * density, 4 * density, stroke);
        }
        for (int i = 0; i < lines.length; i++) {
            if (lines[i] == null) {
                float ly = y + pad + (i + 0.5f) * lineH;
                canvas.drawRect(x + pad, ly - density / 2, x + bw - pad, ly + density / 2, perfText);
            } else {
                canvas.drawText(lines[i], x + pad, y + pad + (i + 1) * lineH - perfText.descent(), perfText);
            }
        }
    }

    private static int mix(int a, int b, float t) {
        int r = (int) (((a >> 16) & 0xFF) * (1 - t) + ((b >> 16) & 0xFF) * t);
        int g = (int) (((a >> 8) & 0xFF) * (1 - t) + ((b >> 8) & 0xFF) * t);
        int bl = (int) ((a & 0xFF) * (1 - t) + (b & 0xFF) * t);
        return 0xFF000000 | r << 16 | g << 8 | bl;
    }

    void setAppearance(boolean visible, float scale, float opacity) {
        if (!visible && controlsVisible) releaseAll();
        controlsVisible = visible;
        this.scale = scale;
        this.opacity = opacity;
        layoutControls(getWidth(), getHeight());
        if (visible) startHudPoll();
        invalidate();
    }

    /** camera speed for swipes (1 = default), vibration on presses, when the combat buttons show */
    void setTouchOptions(float cameraSensitivity, boolean haptics, int combatMode) {
        this.cameraSensitivity = cameraSensitivity;
        this.haptics = haptics;
        this.combatMode = combatMode;
        invalidate();
    }

    boolean controlsVisible() { return controlsVisible; }

    /** the icons built from the game's artwork are ready */
    void iconsChanged() {
        icons.clear();
        invalidate();
    }

    // ---- the menu and editor buttons hide after a while without touches and come back on the next touch
    private static final long MENU_HIDE_MS = 5000;
    private boolean menuShown = true;
    private final Runnable hideMenu = () -> {
        menuShown = false;
        invalidate();
    };

    /** a touch happened: show the menu button and restart its timer; true if it was hidden */
    private boolean touched() {
        boolean wasHidden = !menuShown;
        menuShown = true;
        removeCallbacks(hideMenu);
        postDelayed(hideMenu, MENU_HIDE_MS);
        if (wasHidden) invalidate();
        return wasHidden;
    }

    @Override
    protected void onAttachedToWindow() {
        super.onAttachedToWindow();
        touched();
        startHudPoll();
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(hideMenu);
        removeCallbacks(pollState);
        removeCallbacks(cameraTick);
        removeCallbacks(macroTick);
        super.onDetachedFromWindow();
    }

    // ---- game state: what A, B and ZR do, the items on X/Y/R, boat / sword / lock-on
    private int[] state = new int[7];
    private boolean hudPolling;
    private final Runnable pollState = new Runnable() {
        @Override
        public void run() {
            if (!controlsVisible || getWindowToken() == null) {
                hudPolling = false;
                return;
            }
            int[] s = null;
            try {
                s = Native.hudState();
            } catch (UnsatisfiedLinkError ignored) {
            }
            if (s != null && s.length >= 7 && !java.util.Arrays.equals(s, state)) {
                state = s;
                invalidate();
            }
            postDelayed(this, 100);
        }
    };

    private void startHudPoll() {
        if (hudPolling) return;
        hudPolling = true;
        post(pollState);
    }

    private boolean hudKnown() { return (state[HUD_FLAGS] & HUD_KNOWN) != 0; }
    private boolean hudFlag(int f) { return (state[HUD_FLAGS] & f) != 0; }

    private boolean combatShown() {
        if (editMode) return true;
        if (combatMode == 1) return true;
        if (combatMode == 2) return false;
        return !hudKnown() || hudFlag(HUD_HAS_SWORD);  // the combat moves need a sword
    }

    private boolean shown(Ctl c) {
        if (c.removed) return false;
        if (editMode) return true;
        if (c.kind == K_MENU || c.kind == K_EDIT) return menuShown;
        if (!controlsVisible) return false;
        if (c.kind == K_MACRO) return combatShown();
        // D-pad items Link doesn't have yet are hidden: the Wind Waker (up), the cannon (left: it fires
        // bombs) and the salvage hook (right)
        if (c.kind == K_BUTTON && hudKnown()) {
            if (c.bit == Native.UP) return hudFlag(HUD_HAS_BATON);
            if (c.bit == Native.LEFT) return hudFlag(HUD_HAS_BOMBS);
            if (c.bit == Native.RIGHT) return hudFlag(HUD_HAS_GRAPPLE);
        }
        return true;
    }

    // ---- output to the game
    // left stick: the finger's offset from where it went down (the stick follows a finger that goes past it)
    private int stickPointer = -1;
    private float stickOx, stickOy, stickX, stickY;
    // camera: swipe speed on the right side acts as the right stick
    private int camPointer = -1;
    private float camLastX, camLastY, camAccX, camAccY, camX, camY;
    private long lastCamTap;
    private float camDownX, camDownY;
    // pinch on the camera side (a second finger): zooms the camera in and out (Native "camera_zoom")
    private int pinchPointer = -1;
    private float pinchX, pinchY, pinchStartDist, pinchStartZoom, cameraZoom = 1f;
    // double tap on the camera side: a short ZL (centres the camera behind Link)
    private int pulseBits;
    // combat move in progress
    private int[][] macro;   // steps: {ms, buttons, stick? 1:0, sx*1000, sy*1000}
    private int macroStep = -1;
    private int macroBits;
    private boolean macroStick;
    private float macroSx, macroSy;

    int buttons() {
        int b = pulseBits | macroBits;
        for (Ctl c : controls) {
            if (c.kind == K_BUTTON && c.pressed) b |= c.bit;
        }
        return b;
    }

    float stickX(int i) { return i == 0 ? (macroStick ? macroSx : stickX) : camX; }
    float stickY(int i) { return i == 0 ? (macroStick ? macroSy : stickY) : camY; }

    private void changed() {
        listener.onControlsChanged();
        invalidate();
    }

    private void buzz() {
        if (haptics) performHapticFeedback(HapticFeedbackConstants.KEYBOARD_TAP);
    }

    // ---- camera
    private final Runnable cameraTick = new Runnable() {
        @Override
        public void run() {
            // a swipe of about 10 u per second turns the camera at full speed (times the sensitivity)
            float k = cameraSensitivity / Math.max(1f, baseUnit * 0.16f);
            float tx = clamp(camAccX * k), ty = clamp(-camAccY * k);
            camAccX = camAccY = 0;
            camX = camX * 0.35f + tx * 0.65f;
            camY = camY * 0.35f + ty * 0.65f;
            if (Math.abs(camX) < 0.02f) camX = 0;
            if (Math.abs(camY) < 0.02f) camY = 0;
            listener.onControlsChanged();
            if (camPointer >= 0 || camX != 0 || camY != 0) postDelayed(this, 16);
        }
    };

    private static float clamp(float v) { return Math.max(-1f, Math.min(1f, v)); }

    private void cameraDown(int id, float x, float y) {
        camPointer = id;
        camLastX = camDownX = x;
        camLastY = camDownY = y;
        removeCallbacks(cameraTick);
        post(cameraTick);
        long now = android.os.SystemClock.uptimeMillis();
        if (now - lastCamTap < 300) {  // second tap: centre the camera
            lastCamTap = 0;
            pulseBits = Native.ZL;
            buzz();
            changed();
            postDelayed(() -> {
                pulseBits = 0;
                changed();
            }, 150);
        }
    }

    private void pinchDown(int id, float x, float y) {
        pinchPointer = id;
        pinchX = x;
        pinchY = y;
        pinchStartDist = Math.max(1f, (float) Math.hypot(x - camLastX, y - camLastY));
        pinchStartZoom = cameraZoom;
        camAccX = camAccY = 0;
        camX = camY = 0;
        listener.onControlsChanged();
    }

    // fingers apart: closer (a smaller distance factor); together: further away
    private void pinchMoved() {
        float d = Math.max(1f, (float) Math.hypot(pinchX - camLastX, pinchY - camLastY));
        float z = Math.max(0.5f, Math.min(2f, pinchStartZoom * pinchStartDist / d));
        if (Math.abs(z - cameraZoom) < 0.005f) return;
        cameraZoom = z;
        Native.setOption("camera_zoom", Math.round(z * 100));
    }

    private void cameraUp(float x, float y) {
        camPointer = -1;
        boolean tap = Math.hypot(x - camDownX, y - camDownY) < baseUnit * 0.3f;
        lastCamTap = tap ? android.os.SystemClock.uptimeMillis() : 0;
    }

    // ---- combat moves: the GamePad combinations, pressed for the player
    private final Runnable macroTick = new Runnable() {
        @Override
        public void run() {
            macroStep++;
            applyMacroStep();
        }
    };

    private void applyMacroStep() {
        if (macro == null || macroStep >= macro.length) {
            macro = null;
            macroStep = -1;
            macroBits = 0;
            macroStick = false;
            changed();
            return;
        }
        int[] s = macro[macroStep];
        macroBits = s[1];
        macroStick = s[2] != 0;
        macroSx = s[3] / 1000f;
        macroSy = s[4] / 1000f;
        changed();
        postDelayed(macroTick, s[0]);
    }

    private void startMacro(int move) {
        if (macro != null) return;
        final int ZL = Native.ZL, A = Native.A, B = Native.B;
        switch (move) {
            case M_JUMP:      // lock on + A
                macro = new int[][] {{60, ZL, 0, 0, 0}, {140, ZL | A, 0, 0, 0}, {60, ZL, 0, 0, 0}};
                break;
            case M_VERTICAL:  // lock on + B
                macro = new int[][] {{60, ZL, 0, 0, 0}, {140, ZL | B, 0, 0, 0}, {60, ZL, 0, 0, 0}};
                break;
            case M_SPIN:      // hold B until the sword charges, then let go
                macro = new int[][] {{1100, B, 0, 0, 0}};
                break;
            case M_DODGE: {   // lock on + stick + A: sideways where the stick points, else a backflip
                float sx = stickX, sy = stickY, len = (float) Math.hypot(sx, sy);
                if (len < 0.4f) { sx = 0; sy = -1; } else { sx /= len; sy /= len; }
                int x = Math.round(sx * 1000), y = Math.round(sy * 1000);
                macro = new int[][] {{60, ZL, 0, 0, 0}, {80, ZL, 1, x, y}, {140, ZL | A, 1, x, y}, {60, ZL, 0, 0, 0}};
                break;
            }
            default:
                return;
        }
        macroStep = 0;
        applyMacroStep();
    }

    // ---- layout
    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        layoutControls(w, h);
    }

    // u = shorter side / 7 (landscape), times the size setting
    private void layoutControls(int W, int H) {
        if (W == 0 || H == 0) return;
        baseUnit = Math.min(W, H) / 7f;
        unit = baseUnit * scale;
        for (Ctl c : controls) place(c, W, H);
        text.setTextSize(unit * 0.34f);
        stroke.setStrokeWidth(Math.max(2f, unit * 0.04f));
    }

    private void place(Ctl c, int W, int H) {
        if (c.custom()) {
            c.cx = c.fx * W;
            c.cy = c.fy * H;
            c.r = c.fd * baseUnit / 2;
        } else {
            c.cx = c.ax * W + c.ox * unit;
            c.cy = c.ay * H + c.oy * unit;
            c.r = c.d * unit / 2;
        }
        c.cx = Math.max(c.r * 0.5f, Math.min(W - c.r * 0.5f, c.cx));
        c.cy = Math.max(c.r * 0.5f, Math.min(H - c.r * 0.5f, c.cy));
    }

    /** the saved layout: "id:fx,fy,d;..." for the controls the user moved, "id:-" for removed ones */
    void setLayout(String s) {
        for (Ctl c : controls) {
            c.fx = c.fy = c.fd = Float.NaN;
            c.removed = false;
        }
        if (s != null)
            for (String part : s.split(";")) {
                String[] kv = part.split(":");
                if (kv.length != 2) continue;
                Ctl c = byId.get(kv[0]);
                if (c != null && kv[1].equals("-")) {
                    c.removed = c.removable();
                    continue;
                }
                String[] v = kv[1].split(",");
                if (c == null || v.length != 3) continue;
                try {
                    c.fx = Float.parseFloat(v[0]);
                    c.fy = Float.parseFloat(v[1]);
                    c.fd = Float.parseFloat(v[2]);
                } catch (NumberFormatException e) {
                    c.fx = c.fy = c.fd = Float.NaN;
                }
            }
        layoutControls(getWidth(), getHeight());
        invalidate();
    }

    private String layoutString() {
        StringBuilder sb = new StringBuilder();
        for (Ctl c : controls) {
            if (!c.custom() && !c.removed) continue;
            if (sb.length() > 0) sb.append(';');
            if (c.removed) sb.append(c.id).append(":-");
            else sb.append(String.format(Locale.ROOT, "%s:%.4f,%.4f,%.3f", c.id, c.fx, c.fy, c.fd));
        }
        return sb.toString();
    }

    private Ctl controlAt(float x, float y) {
        // exact hits first, then a little slack (thumbs are imprecise); the stick is a zone, not a button
        for (float slack : new float[] {1f, 1.25f})
            for (Ctl c : controls) {
                if (!shown(c) || (c.kind == K_STICK && !editMode)) continue;
                if (c.hit(x, y, slack)) return c;
            }
        return null;
    }

    private void updateStick(float x, float y) {
        Ctl s = byId.get("stick");
        float r = s.r * 0.8f;
        float dx = x - stickOx, dy = y - stickOy, len = (float) Math.hypot(dx, dy);
        if (len > r) {  // drag the stick along with the finger
            stickOx = x - dx / len * r;
            stickOy = y - dy / len * r;
            dx = x - stickOx;
            dy = y - stickOy;
        }
        stickX = clamp(dx / r);
        stickY = clamp(-dy / r);
    }

    private void release(Ctl c) {
        c.pressed = false;
    }

    private void releaseAll() {
        for (Ctl c : controls) release(c);
        pointers.clear();
        if (drcPointer >= 0) Native.setTouch(false, 0, 0);
        drcPointer = -1;
        stickPointer = camPointer = pinchPointer = -1;
        stickX = stickY = camX = camY = 0;
        removeCallbacks(macroTick);
        macro = null;
        macroStep = -1;
        macroBits = pulseBits = 0;
        macroStick = false;
    }

    private boolean inDrc(float x, float y) { return !drcRect.isEmpty() && drcRect.contains(x, y); }

    private void touchDrc(boolean down, float x, float y) {
        float tx = (x - drcRect.left) / drcRect.width(), ty = (y - drcRect.top) / drcRect.height();
        Native.setTouch(down, Math.max(0, Math.min(1, tx)), Math.max(0, Math.min(1, ty)));
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editMode) return editTouch(e);
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        boolean changed = false;
        boolean menuWasHidden = touched();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = e.getPointerId(idx);
                float x = e.getX(idx), y = e.getY(idx);
                if (perfTouchDown(id, x, y)) break;
                Ctl c = controlAt(x, y);
                if (c != null) {
                    if (c.kind == K_MENU || c.kind == K_EDIT) {
                        // a touch on a hidden button only shows it
                        if (!menuWasHidden) {
                            if (c.kind == K_MENU) listener.onMenu();
                            else setEditMode(true);
                        }
                        return true;
                    }
                    pointers.put(id, c);
                    if (c.kind == K_MACRO) {
                        buzz();
                        startMacro(c.bit);
                        c.pressed = true;
                    } else {
                        c.pressed = true;
                        buzz();
                    }
                    changed = true;
                } else if (drcPointer < 0 && inDrc(x, y)) {
                    drcPointer = id;
                    touchDrc(true, x, y);
                } else if (!controlsVisible) {
                    break;
                } else if (x < getWidth() * 0.45f && !byId.get("stick").removed) {
                    if (stickPointer < 0) {
                        stickPointer = id;
                        stickOx = x;
                        stickOy = y;
                        updateStick(x, y);
                        changed = true;
                    }
                } else if (camPointer < 0) {
                    cameraDown(id, x, y);
                } else if (pinchPointer < 0) {
                    pinchDown(id, x, y);
                }
                break;
            }
            case MotionEvent.ACTION_MOVE:
                for (int i = 0; i < e.getPointerCount(); i++) {
                    int id = e.getPointerId(i);
                    float x = e.getX(i), y = e.getY(i);
                    if (id == perfDragPointer) {
                        perfDrag(x, y);
                        continue;
                    }
                    if (id == drcPointer) {
                        touchDrc(true, x, y);
                        continue;
                    }
                    if (id == stickPointer) {
                        updateStick(x, y);
                        changed = true;
                        continue;
                    }
                    if (id == pinchPointer) {
                        pinchX = x;
                        pinchY = y;
                        pinchMoved();
                        continue;
                    }
                    if (id == camPointer) {
                        if (pinchPointer < 0) {  // turning; a pinch holds the camera still
                            camAccX += x - camLastX;
                            camAccY += y - camLastY;
                        }
                        camLastX = x;
                        camLastY = y;
                        if (pinchPointer >= 0) pinchMoved();
                        continue;
                    }
                    Ctl c = pointers.get(id);
                    if (c == null) continue;
                    if (c.kind == K_BUTTON) {
                        // sliding between buttons moves the press (e.g. from B to A)
                        Ctl now = controlAt(x, y);
                        if (now != null && now != c && now.kind == K_BUTTON) {
                            release(c);
                            now.pressed = true;
                            pointers.put(id, now);
                            buzz();
                        }
                    }
                    changed = true;
                }
                break;
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                if (action == MotionEvent.ACTION_CANCEL) {
                    releaseAll();
                    perfDragPointer = -1;
                    changed = true;
                    break;
                }
                int id = e.getPointerId(idx);
                if (id == perfDragPointer) {
                    perfDragPointer = -1;
                    listener.onOverlayMoved(perfFx, perfFy);
                    invalidate();
                    break;
                }
                if (id == drcPointer) {
                    touchDrc(false, e.getX(idx), e.getY(idx));
                    drcPointer = -1;
                }
                if (id == stickPointer) {
                    stickPointer = -1;
                    stickX = stickY = 0;
                    changed = true;
                }
                if (id == pinchPointer) {
                    pinchPointer = -1;
                    lastCamTap = 0;
                } else if (id == camPointer && pinchPointer >= 0) {
                    // the first finger lifted during a pinch: the other one goes on turning the camera
                    camPointer = pinchPointer;
                    pinchPointer = -1;
                    camLastX = camDownX = pinchX;
                    camLastY = camDownY = pinchY;
                    lastCamTap = 0;
                } else if (id == camPointer) cameraUp(e.getX(idx), e.getY(idx));
                Ctl c = pointers.get(id);
                if (c != null) {
                    release(c);
                    pointers.remove(id);
                    changed = true;
                }
                break;
            }
            default:
                break;
        }
        if (changed) changed();
        return true;
    }

    // ---- layout editor: drag to move (snaps to the grid), drag the handle or pinch to resize
    private boolean editMode;
    private Ctl editSel;
    private int editPointer = -1, editPointer2 = -1;
    private boolean editResizing;
    private float editDx, editDy, editPinchStart, editSizeStart;
    private final RectF editReset = new RectF(), editDone = new RectF(), editRemove = new RectF(), editAdd = new RectF(),
            editPanel = new RectF();
    private boolean editAddOpen;  // the panel with the removed controls, to put them back
    private final List<RectF> editPanelSlots = new ArrayList<>();
    private final List<Ctl> editPanelCtls = new ArrayList<>();

    private boolean anyRemoved() {
        for (Ctl c : controls) if (c.removed) return true;
        return false;
    }

    boolean editMode() { return editMode; }

    void setEditMode(boolean on) {
        if (on == editMode) return;
        releaseAll();
        changed();
        editMode = on;
        editSel = null;
        editPointer = editPointer2 = -1;
        invalidate();
    }

    private float grid() { return baseUnit * 0.25f; }

    private float snap(float v) {
        float g = grid();
        return Math.round(v / g) * g;
    }

    /** makes the control's current place its saved place */
    private void pin(Ctl c) {
        int W = Math.max(1, getWidth()), H = Math.max(1, getHeight());
        c.fx = c.cx / W;
        c.fy = c.cy / H;
        c.fd = c.r * 2 / baseUnit;
    }

    private float handleX(Ctl c) { return c.cx + c.r * 0.75f; }
    private float handleY(Ctl c) { return c.cy + c.r * 0.75f; }

    private boolean editTouch(MotionEvent e) {
        int action = e.getActionMasked(), idx = e.getActionIndex();
        int id = e.getPointerId(idx);
        float x = e.getX(idx), y = e.getY(idx);
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN:
                if (editPointer >= 0) {  // second finger: pinch the selected control
                    if (editSel != null && editPointer2 < 0) {
                        editPointer2 = id;
                        int p = e.findPointerIndex(editPointer);
                        editPinchStart = (float) Math.hypot(x - e.getX(p), y - e.getY(p));
                        editSizeStart = editSel.r;
                    }
                    break;
                }
                if (editAddOpen) {  // the panel takes the touch: a control to put back, or outside to close
                    for (int i = 0; i < editPanelSlots.size(); i++)
                        if (editPanelSlots.get(i).contains(x, y)) {
                            Ctl c = editPanelCtls.get(i);
                            c.removed = false;
                            editSel = c;
                            buzz();
                            break;
                        }
                    if (!editPanel.contains(x, y) || !anyRemoved()) editAddOpen = false;
                    invalidate();
                    return true;
                }
                if (editDone.contains(x, y)) {
                    listener.onLayoutSaved(layoutString());
                    setEditMode(false);
                    return true;
                }
                if (editReset.contains(x, y)) {
                    for (Ctl c : controls) {
                        c.fx = c.fy = c.fd = Float.NaN;
                        c.removed = false;
                    }
                    editSel = null;
                    layoutControls(getWidth(), getHeight());
                    invalidate();
                    return true;
                }
                if (editRemove.contains(x, y)) {
                    if (editSel != null && editSel.removable()) {
                        editSel.removed = true;
                        editSel = null;
                        buzz();
                    }
                    invalidate();
                    return true;
                }
                if (editAdd.contains(x, y)) {
                    editAddOpen = anyRemoved();
                    invalidate();
                    return true;
                }
                editResizing = editSel != null && Math.hypot(x - handleX(editSel), y - handleY(editSel)) < baseUnit * 0.35f;
                if (!editResizing) {
                    Ctl c = controlAt(x, y);
                    editSel = c;
                    if (c != null) {
                        editDx = x - c.cx;
                        editDy = y - c.cy;
                    }
                }
                if (editSel != null) editPointer = id;
                invalidate();
                break;
            case MotionEvent.ACTION_MOVE: {
                if (editSel == null || editPointer < 0) break;
                int p = e.findPointerIndex(editPointer);
                if (p < 0) break;
                float px = e.getX(p), py = e.getY(p);
                if (editPointer2 >= 0) {
                    int q = e.findPointerIndex(editPointer2);
                    if (q >= 0 && editPinchStart > 0) {
                        float dist = (float) Math.hypot(px - e.getX(q), py - e.getY(q));
                        setRadius(editSel, editSizeStart * dist / editPinchStart);
                    }
                } else if (editResizing) {
                    setRadius(editSel, (float) Math.hypot(px - editSel.cx, py - editSel.cy) / 0.75f / (float) Math.sqrt(2));
                } else {
                    editSel.cx = Math.max(0, Math.min(getWidth(), px - editDx));
                    editSel.cy = Math.max(0, Math.min(getHeight(), py - editDy));
                }
                pin(editSel);
                invalidate();
                break;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL:
                if (id == editPointer2) {
                    editPointer2 = -1;
                } else if (id == editPointer || action == MotionEvent.ACTION_CANCEL) {
                    if (editSel != null && !editResizing && editPointer2 < 0) {
                        editSel.cx = snap(editSel.cx);
                        editSel.cy = snap(editSel.cy);
                        pin(editSel);
                    }
                    editPointer = editPointer2 = -1;
                    editResizing = false;
                }
                invalidate();
                break;
            default:
                break;
        }
        return true;
    }

    private void setRadius(Ctl c, float r) {
        float min = baseUnit * 0.25f, max = baseUnit * 2f;
        c.r = Math.max(min, Math.min(max, r));
    }

    // ---- drawing
    @Override
    protected void onDraw(Canvas canvas) {
        if (editMode) drawGrid(canvas);
        drawClimbHud(canvas);
        if (!editMode) drawPerfHud(canvas);
        int a = editMode ? 230 : (int) (255 * opacity);
        for (Ctl c : controls) {
            if (!shown(c)) continue;
            switch (c.kind) {
                case K_STICK: drawStick(canvas, c, a); break;
                default: drawButton(canvas, c, a); break;
            }
            if (editMode && c == editSel) {
                stroke.setColor(0xFFFFCC26);
                stroke.setAlpha(255);
                canvas.drawCircle(c.cx, c.cy, c.r * 1.05f, stroke);
                icons.draw(canvas, "editor_resize_handle", 0xFFFFFFFF, "↘", handleX(c), handleY(c), baseUnit * 0.22f, 255, false);
            }
        }
        if (editMode) drawEditBar(canvas);
    }

    private void drawButton(Canvas canvas, Ctl c, int a) {
        String name = c.icon;
        String overlay = null;
        switch (c.id) {
            case "a": {
                String n = TouchIcons.at(TouchIcons.A_ACTIONS, state[HUD_A]);
                if (n != null) name = n;
                else overlay = "A";
                break;
            }
            case "b": {
                String n = TouchIcons.at(TouchIcons.B_ACTIONS, state[HUD_B]);
                if (n != null) name = n;
                break;
            }
            case "zr": {
                String n = TouchIcons.at(TouchIcons.ZR_ACTIONS, state[HUD_ZR]);
                if (n != null) name = n;
                break;
            }
            case "x": case "y": case "r": {
                int slot = c.id.equals("x") ? HUD_X : c.id.equals("y") ? HUD_Y : HUD_R;
                String n = TouchIcons.at(TouchIcons.ITEMS, state[slot]);
                if (n != null) name = n;
                else overlay = c.label;
                break;
            }
            case "zl":
                if (hudFlag(HUD_TARGETING)) name = "zl_target_active";
                break;
            default:
                break;
        }
        boolean pressed = c.pressed;
        boolean hasIcon = icons.get(name) != null;
        icons.draw(canvas, name, c.color, overlay != null ? overlay : c.label, c.cx, c.cy, c.r, a, pressed);
        if (hasIcon && overlay != null) icons.label(canvas, overlay, c.cx, c.cy, c.r, a);
    }

    private void drawStick(Canvas canvas, Ctl c, int a) {
        boolean active = stickPointer >= 0;
        float bx = active ? stickOx : c.cx, by = active ? stickOy : c.cy;
        int alpha = active ? a : a * 2 / 3;
        if (icons.get("stick_base") != null) icons.draw(canvas, "stick_base", 0, null, bx, by, c.r, alpha, false);
        else {
            fill.setColor(TouchIcons.SKY);
            fill.setAlpha(alpha / 3);
            canvas.drawCircle(bx, by, c.r, fill);
            stroke.setColor(TouchIcons.NAVY);
            stroke.setAlpha(alpha);
            canvas.drawCircle(bx, by, c.r, stroke);
        }
        float kx = bx + stickX * c.r * 0.8f, ky = by - stickY * c.r * 0.8f;
        icons.draw(canvas, "stick_knob", TouchIcons.BONE, "", kx, ky, c.r * 0.42f, alpha, false);
    }

    private void drawGrid(Canvas canvas) {
        fill.setColor(0xFF000000);
        fill.setAlpha(90);
        canvas.drawRect(0, 0, getWidth(), getHeight(), fill);
        float g = grid();
        stroke.setColor(0xFF9FD8FF);
        for (int i = 0; i * g <= getWidth(); i++) {
            stroke.setAlpha(i % 4 == 0 ? 90 : 35);
            canvas.drawLine(i * g, 0, i * g, getHeight(), stroke);
        }
        for (int i = 0; i * g <= getHeight(); i++) {
            stroke.setAlpha(i % 4 == 0 ? 90 : 35);
            canvas.drawLine(0, i * g, getWidth(), i * g, stroke);
        }
    }

    private void drawEditBar(Canvas canvas) {
        float r = baseUnit * 0.42f, cy = baseUnit * 0.6f, cx = getWidth() / 2f, step = baseUnit * 1.15f;
        editReset.set(cx - 1.5f * step - r, cy - r, cx - 1.5f * step + r, cy + r);
        editRemove.set(cx - 0.5f * step - r, cy - r, cx - 0.5f * step + r, cy + r);
        editAdd.set(cx + 0.5f * step - r, cy - r, cx + 0.5f * step + r, cy + r);
        editDone.set(cx + 1.5f * step - r, cy - r, cx + 1.5f * step + r, cy + r);
        boolean canRemove = editSel != null && editSel.removable(), canAdd = anyRemoved();
        icons.draw(canvas, "editor_reset", TouchIcons.BONE, "↺", editReset.centerX(), cy, r, 255, false);
        icons.draw(canvas, "editor_remove", TouchIcons.RED, "🗑", editRemove.centerX(), cy, r, canRemove ? 255 : 90, false);
        icons.draw(canvas, "editor_add", TouchIcons.SKY, "+", editAdd.centerX(), cy, r, canAdd ? 255 : 90, false);
        icons.draw(canvas, "editor_done", TouchIcons.GREEN, "✓", editDone.centerX(), cy, r, 255, false);
        if (editAddOpen) drawAddPanel(canvas);
        text.setColor(0xFFFFFFFF);
        text.setAlpha(255);
        text.setTextSize(baseUnit * 0.22f);
        canvas.drawText(getContext().getString(R.string.layout_edit_hint), cx, cy + r + baseUnit * 0.35f, text);
    }

    // the controls taken off the screen, in a grid: a touch puts one back
    private void drawAddPanel(Canvas canvas) {
        editPanelSlots.clear();
        editPanelCtls.clear();
        for (Ctl c : controls) if (c.removed) editPanelCtls.add(c);
        int n = editPanelCtls.size();
        if (n == 0) return;
        float cell = baseUnit * 1.1f, r = baseUnit * 0.42f;
        int cols = Math.min(n, 6), rows = (n + cols - 1) / cols;
        float w = cols * cell + baseUnit * 0.4f, h = rows * cell + baseUnit * 0.9f;
        float left = (getWidth() - w) / 2f, top = (getHeight() - h) / 2f;
        editPanel.set(left, top, left + w, top + h);
        fill.setColor(0xE61B2A4A);
        canvas.drawRoundRect(editPanel, baseUnit * 0.2f, baseUnit * 0.2f, fill);
        text.setColor(0xFFFFFFFF);
        text.setAlpha(255);
        text.setTextSize(baseUnit * 0.24f);
        canvas.drawText(getContext().getString(R.string.layout_add_title), getWidth() / 2f, top + baseUnit * 0.45f, text);
        for (int i = 0; i < n; i++) {
            Ctl c = editPanelCtls.get(i);
            float x = left + baseUnit * 0.2f + (i % cols + 0.5f) * cell, y = top + baseUnit * 0.7f + (i / cols + 0.5f) * cell;
            editPanelSlots.add(new RectF(x - cell / 2, y - cell / 2, x + cell / 2, y + cell / 2));
            icons.draw(canvas, c.icon, c.color, c.label, x, y, r, 255, false);
        }
    }
}
