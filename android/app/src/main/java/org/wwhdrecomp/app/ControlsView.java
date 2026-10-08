package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.util.SparseArray;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.List;

/**
 * On-screen Wii U GamePad: sticks, buttons and the GamePad touch screen, drawn over the game.
 * Touches that hit no control and land on the GamePad image go to the game as touch-panel input.
 */
final class ControlsView extends View {
    interface Listener {
        void onControlsChanged();
        void onMenu();
        /** the performance overlay was dragged: its top left corner as fractions of the view size */
        void onOverlayMoved(float fx, float fy);
        /** the screen button: switch between the TV picture and the GamePad picture */
        void onScreenToggle();
        /** the layout editor closed: the controls' layout (layoutString), opacity and style */
        void onLayoutEdited(String layout, float opacity, boolean filled);
    }

    private static final int KIND_BUTTON = 0, KIND_STICK = 1, KIND_DPAD = 2, KIND_MENU = 3, KIND_SCREEN = 4;

    private static final class Control {
        int kind, bit;
        String label, key;           // key: in the saved layout
        float px = Float.NaN, py = Float.NaN, size = 1f;  // own place (fractions of the view) and size factor
        boolean hidden;
        float cx, cy, w, h;          // centre and size (buttons: w x h, sticks: w = radius)
        boolean round, pressed;
        boolean latch;               // a tap switches it on, the next off (ZL: targeting stays on)
        float sx, sy;                // stick deflection -1..1 (+y up)
        int dpadBits;
        Control(int kind, int bit, String label, boolean round) {
            this.kind = kind;
            this.bit = bit;
            this.label = label;
            this.round = round;
            key = "BSDMT".charAt(kind) + label;
        }
        boolean hit(float x, float y, float slack) {
            if (kind == KIND_STICK || kind == KIND_DPAD) {
                float r = w * slack;
                return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
            }
            return Math.abs(x - cx) <= w / 2 * slack && Math.abs(y - cy) <= h / 2 * slack;
        }
    }

    private final Listener listener;
    private final List<Control> controls = new ArrayList<>();
    private final SparseArray<Control> pointers = new SparseArray<>();
    private int drcPointer = -1;
    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF tmp = new RectF();

    // GamePad image on screen (letterboxed), for touch input; empty if hidden
    private final RectF drcRect = new RectF();
    private boolean controlsVisible = true;
    private float scale = 1f, opacity = 0.45f;
    // "Flexible" controls: the sticks appear where a thumb lands outside the buttons (left half: left
    // stick; right half: right stick or the swipe camera), drawn or not
    private boolean flexible, showSticks = true, swipeCamera;
    private int swipePointer = -1;
    private float swipeX, swipeY, swipeSpeed = 2f;

    ControlsView(Context c, Listener l) {
        super(c);
        listener = l;
        fill.setStyle(Paint.Style.FILL);
        stroke.setStyle(Paint.Style.STROKE);
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
        controls.add(new Control(KIND_BUTTON, Native.ZL, "ZL", false));
        controls.get(0).latch = true;
        controls.add(new Control(KIND_BUTTON, Native.L, "L", false));
        controls.add(new Control(KIND_BUTTON, Native.ZR, "ZR", false));
        controls.add(new Control(KIND_BUTTON, Native.R, "R", false));
        controls.add(new Control(KIND_DPAD, 0, "", true));
        controls.add(new Control(KIND_STICK, 0, "L", true));
        controls.add(new Control(KIND_STICK, 1, "R", true));
        controls.add(new Control(KIND_BUTTON, Native.X, "X", true));
        controls.add(new Control(KIND_BUTTON, Native.A, "A", true));
        controls.add(new Control(KIND_BUTTON, Native.B, "B", true));
        controls.add(new Control(KIND_BUTTON, Native.Y, "Y", true));
        controls.add(new Control(KIND_BUTTON, Native.MINUS, "−", true));
        controls.add(new Control(KIND_BUTTON, Native.PLUS, "+", true));
        controls.add(new Control(KIND_BUTTON, Native.STICK_L, "L3", true));
        controls.add(new Control(KIND_BUTTON, Native.STICK_R, "R3", true));
        controls.add(new Control(KIND_MENU, 0, "≡", true));
        controls.add(new Control(KIND_SCREEN, 0, "", false));  // 16: TV / GamePad picture (GamePad on demand)
    }

    // ---- "GamePad on demand": the screen button switches between the TV picture (with the controls)
    // and the full GamePad picture (only the screen button; touches go to the GamePad)
    private boolean screenButton, drcMode;

    void setScreenButton(boolean on, boolean drcShown) {
        screenButton = on;
        if (drcShown != drcMode) releaseAll();
        drcMode = on && drcShown;
        invalidate();
    }

    /** whether a control is part of the current screen mode */
    private boolean inMode(Control c) {
        if (c.kind == KIND_SCREEN) return screenButton;
        return !drcMode;
    }

    // ---- layout editor: drag a control to move it, pinch (or Size -/+) to resize it, tap to select it
    // for Hide / Show; a toolbar sets the opacity and style of all controls. The game gets no input.
    private boolean editing, filled;
    private Control sel;
    private int dragPtr = -1, pinchPtr = -1;
    private float dragDx, dragDy, pinchDist, pinchSize;
    private static final String[] TOOLS = {"Size −", "Size +", "Hide", "Opacity −", "Opacity +", "Style", "Reset", "Done"};
    private final RectF[] toolRects = new RectF[TOOLS.length];

    void setFilled(boolean f) {
        filled = f;
        invalidate();
    }

    /** the controls' own places, sizes and hidden flags: "key=px,py,size,hidden;..." */
    String layoutString() {
        StringBuilder b = new StringBuilder();
        for (Control c : controls) {
            if (Float.isNaN(c.px) && c.size == 1f && !c.hidden) continue;
            b.append(c.key).append('=').append(c.px).append(',').append(c.py).append(',').append(c.size).append(',')
                    .append(c.hidden ? 1 : 0).append(';');
        }
        return b.toString();
    }

    void setLayoutString(String s) {
        for (Control c : controls) {
            c.px = c.py = Float.NaN;
            c.size = 1f;
            c.hidden = false;
        }
        if (s != null)
            for (String part : s.split(";")) {
                int eq = part.indexOf('=');
                if (eq < 0) continue;
                String[] v = part.substring(eq + 1).split(",");
                if (v.length < 4) continue;
                for (Control c : controls)
                    if (c.key.equals(part.substring(0, eq))) {
                        try {
                            c.px = Float.parseFloat(v[0]);
                            c.py = Float.parseFloat(v[1]);
                            c.size = Math.max(0.4f, Math.min(3f, Float.parseFloat(v[2])));
                            c.hidden = v[3].equals("1") && c.kind != KIND_MENU && c.kind != KIND_SCREEN;
                        } catch (NumberFormatException ignored) {
                        }
                    }
            }
        layoutControls(getWidth(), getHeight());
        invalidate();
    }

    void setEditing(boolean on) {
        releaseAll();
        editing = on;
        sel = null;
        dragPtr = pinchPtr = -1;
        listener.onControlsChanged();  // nothing pressed while editing
        layoutControls(getWidth(), getHeight());
        invalidate();
    }

    boolean editing() { return editing; }

    private void editLayoutChanged() {
        layoutControls(getWidth(), getHeight());
        invalidate();
    }

    private void tool(int i) {
        switch (i) {
            case 0:
            case 1:
                if (sel != null) {
                    sel.size = Math.max(0.4f, Math.min(3f, sel.size * (i == 0 ? 0.9f : 1.1f)));
                    editLayoutChanged();
                }
                break;
            case 2:
                if (sel != null && sel.kind != KIND_MENU && sel.kind != KIND_SCREEN) {
                    sel.hidden = !sel.hidden;
                    invalidate();
                }
                break;
            case 3:
            case 4:
                opacity = Math.max(0.05f, Math.min(1f, opacity + (i == 3 ? -0.05f : 0.05f)));
                invalidate();
                break;
            case 5:
                filled = !filled;
                invalidate();
                break;
            case 6:
                setLayoutString("");
                opacity = 0.45f;
                filled = false;
                sel = null;
                break;
            default:
                editing = false;
                sel = null;
                listener.onLayoutEdited(layoutString(), opacity, filled);
                layoutControls(getWidth(), getHeight());
                invalidate();
        }
    }

    private boolean editTouch(MotionEvent e) {
        int action = e.getActionMasked(), idx = e.getActionIndex();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = e.getPointerId(idx);
                float x = e.getX(idx), y = e.getY(idx);
                for (int i = 0; i < toolRects.length; i++)
                    if (toolRects[i] != null && toolRects[i].contains(x, y)) {
                        tool(i);
                        return true;
                    }
                if (dragPtr >= 0 && pinchPtr < 0 && sel != null) {  // a second finger: resize the dragged control
                    int di = e.findPointerIndex(dragPtr);
                    if (di >= 0) {
                        pinchPtr = id;
                        pinchDist = Math.max(1f, (float) Math.hypot(x - e.getX(di), y - e.getY(di)));
                        pinchSize = sel.size;
                    }
                    return true;
                }
                Control c = controlAt(x, y);
                sel = c;
                if (c != null) {
                    dragPtr = id;
                    dragDx = c.cx - x;
                    dragDy = c.cy - y;
                }
                invalidate();
                return true;
            }
            case MotionEvent.ACTION_MOVE: {
                if (sel == null || dragPtr < 0) return true;
                int di = e.findPointerIndex(dragPtr);
                if (di < 0) return true;
                if (pinchPtr >= 0) {
                    int pi = e.findPointerIndex(pinchPtr);
                    if (pi >= 0) {
                        float d = (float) Math.hypot(e.getX(pi) - e.getX(di), e.getY(pi) - e.getY(di));
                        sel.size = Math.max(0.4f, Math.min(3f, pinchSize * d / pinchDist));
                    }
                } else {
                    sel.px = (e.getX(di) + dragDx) / getWidth();
                    sel.py = (e.getY(di) + dragDy) / getHeight();
                }
                editLayoutChanged();
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                int id = e.getPointerId(idx);
                if (action == MotionEvent.ACTION_CANCEL || id == dragPtr) dragPtr = pinchPtr = -1;
                else if (id == pinchPtr) pinchPtr = -1;
                return true;
            }
            default:
                return true;
        }
    }

    private void drawEditor(Canvas canvas) {
        float d = getResources().getDisplayMetrics().density;
        float h = 40 * d, gap = 8 * d, pad = 14 * d;
        Paint t = new Paint(Paint.ANTI_ALIAS_FLAG);
        t.setTextSize(15 * d);
        t.setFakeBoldText(true);
        t.setTextAlign(Paint.Align.CENTER);
        float total = 0;
        float[] w = new float[TOOLS.length];
        for (int i = 0; i < TOOLS.length; i++) {
            w[i] = t.measureText(toolLabel(i)) + 2 * pad;
            total += w[i] + gap;
        }
        float x = (getWidth() - total + gap) / 2, y = 10 * d;
        Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG);
        for (int i = 0; i < TOOLS.length; i++) {
            boolean off = (i <= 2 && sel == null) || (i == 2 && sel != null && (sel.kind == KIND_MENU || sel.kind == KIND_SCREEN));
            if (toolRects[i] == null) toolRects[i] = new RectF();
            toolRects[i].set(x, y, x + w[i], y + h);
            bg.setColor(i == TOOLS.length - 1 ? 0xE02E7D32 : 0xE0202020);
            canvas.drawRoundRect(toolRects[i], h / 2, h / 2, bg);
            t.setColor(off ? 0xFF808080 : 0xFFFFFFFF);
            canvas.drawText(toolLabel(i), x + w[i] / 2, y + h / 2 - (t.descent() + t.ascent()) / 2, t);
            x += w[i] + gap;
        }
        t.setTextSize(13 * d);
        t.setColor(0xFFFFFFFF);
        t.setShadowLayer(3 * d, 0, 0, 0xFF000000);
        canvas.drawText(String.format(java.util.Locale.ROOT, "Drag to move, pinch or Size to resize, tap to select. Opacity %d%%",
                Math.round(opacity * 100)), getWidth() / 2f, y + h + 22 * d, t);
    }

    private String toolLabel(int i) {
        if (i == 2) return sel != null && sel.hidden ? "Show" : "Hide";
        if (i == 5) return filled ? "Style: filled" : "Style: outline";
        return TOOLS[i];
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

    private String perfVersion;  // the overlay's last line

    private void drawPerfHud(Canvas canvas) {
        PerfStats p = perf;
        if (p == null) return;
        int it = perfItems;
        // measurements, then (below a separator line) the device: chip, GPU and driver, then (below
        // another one, if the device is shown) the app and its version
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
        if (perfVersion == null) perfVersion = "TloZ:WWHD Recompiled: v" + CrashLogs.appVersion(getContext());  // as the release tag
        if (!list.isEmpty()) {  // (no values chosen: no overlay)
            list.add(null);  // with the device shown, the second one
            list.add(perfVersion);
        }
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
        invalidate();
    }

    boolean controlsVisible() { return controlsVisible; }

    /** classic (fixed sticks) or flexible (floating sticks, optionally invisible, swipe camera) */
    void setSwipeSpeed(float s) { swipeSpeed = s; }

    // ---- context buttons (flexible controls): A, B, X, Y, R and ZR appear when the game has something
    // for them and say what (the game's own button cluster is hidden, MainActivity)
    private boolean context;
    private int[] state = new int[12];
    private boolean gamepadMode, l3InUse;
    private boolean zrLatched;  // ZR held by a tap for crouching  // - swaps the screens in GamePad mode; L3: our run / swim mod
    private final Runnable pollState = new Runnable() {
        @Override
        public void run() {
            int[] s = Native.touchState();
            boolean gp = Native.getOption("pro_controller") == 0;
            boolean l3 = MainActivity.instance != null && MainActivity.instance.l3InUse();
            if (!java.util.Arrays.equals(s, state) || gp != gamepadMode || l3 != l3InUse) {
                state = s;
                gamepadMode = gp;
                l3InUse = l3;
                invalidate();
            }
            postDelayed(this, 100);
        }
    };

    void setContext(boolean on) {
        if (on == context) return;
        context = on;
        layoutControls(getWidth(), getHeight());  // the shoulder buttons' shape
        removeCallbacks(pollState);
        if (on) post(pollState);
        invalidate();
    }

    // not in the pause menu: it uses every button (X / Y / R put items on them)
    private boolean contextActive() { return context && flexible && !editing && state[0] != 0 && state[9] == 0; }

    /** whether the context buttons show this control now (always true without them) */
    private boolean shown(Control c) {
        if (c.kind == KIND_DPAD) return dpadMask() != 0 || c.dpadBits != 0;
        if (!contextActive() || c.kind != KIND_BUTTON || c.pressed) return true;
        if (c.bit == Native.A) return state[1] != 0 || state[8] != 0;  // in events A always continues
        if (c.bit == Native.B) return state[2] != 0xFF;
        if (c.bit == Native.X) return state[3] != 0xFF;
        if (c.bit == Native.Y) return state[4] != 0xFF;
        if (c.bit == Native.R) return state[5] != 0xFF;
        if (c.bit == Native.ZR) return state[7] != 0;
        if (c.bit == Native.L) return false;  // the game doesn't read L in play (checked 2026-10-08); the pause menu shows it
        if (c.bit == Native.MINUS) return gamepadMode;  // swaps TV and GamePad pictures; nothing in Pro Controller mode (checked)
        if (c.bit == Native.STICK_L) return l3InUse;  // our faster running / swimming (the game itself: not seen to use L3)
        return true;
    }

    /** the d-pad directions the context buttons offer (all without them): up conducts with the Wind
     *  Waker; down does nothing in play (checked 2026-10-08); left / right (the boat's cannon and
     *  grappling hook) stay until checked on the boat */
    private int dpadMask() {
        int all = Native.UP | Native.DOWN | Native.LEFT | Native.RIGHT;
        if (!contextActive()) return all;
        return (state[10] != 0 ? Native.UP : 0) | Native.LEFT | Native.RIGHT;
    }

    /** the game's icon the context buttons show on this control, or null (then its label) */
    private android.graphics.Bitmap iconOf(Control c) {
        if (!contextActive() || c.kind != KIND_BUTTON) return null;
        String t = null;
        if (c.bit == Native.B && state[6] == 0x35) t = GameIcons.swordTexture(state[2]);  // B: the sword (not other B actions)
        else if (c.bit == Native.A) t = GameIcons.actionTexture(state[1], state[2], state[11]);
        else if (c.bit == Native.ZR) t = GameIcons.actionTexture(state[7], state[2], state[11]);
        else if (c.bit == Native.MINUS) t = "TvDrcIcon_00^t";
        else if (c.bit == Native.STICK_L) t = "LStick_00^t";
        else if (c.bit == Native.STICK_R) t = "RStick_00^t";
        else if (c.bit == Native.PLUS) t = "MapItemIcon_00^l";
        else if (c.bit == Native.X) t = GameIcons.itemTexture(state[3]);
        else if (c.bit == Native.Y) t = GameIcons.itemTexture(state[4]);
        else if (c.bit == Native.R) t = GameIcons.itemTexture(state[5]);
        return t == null ? null : GameIcons.get(getContext(), t, () -> post(this::invalidate));
    }

    private final android.graphics.Rect iconSrc = new android.graphics.Rect();
    private final Paint iconPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);

    /** the pictogram the context buttons draw on this control (Pictograms.java), or null */
    private String pictogramOf(Control c) {
        if (!contextActive() || c.kind != KIND_BUTTON) return null;
        if (c.bit == Native.A) return Pictograms.forAction(state[1]);
        if (c.bit == Native.ZR) return Pictograms.forAction(state[7]);
        if (c.bit == Native.ZL) return "target";
        if (c.bit == Native.STICK_R) return "eye";
        if (c.bit == Native.PLUS) return "menu";
        if (c.bit == Native.MINUS) return "swap";
        return null;
    }

    /** the label the context buttons give this control */
    private String labelOf(Control c) {
        if (!contextActive() || c.kind != KIND_BUTTON) return c.label;
        String s = null;
        if (c.bit == Native.A) s = GameText.action(state[1]);
        else if (c.bit == Native.B) s = state[6] != 0 ? GameText.action(state[6]) : null;
        else if (c.bit == Native.X) s = GameText.item(state[3]);
        else if (c.bit == Native.Y) s = GameText.item(state[4]);
        else if (c.bit == Native.R) s = GameText.item(state[5]);
        else if (c.bit == Native.ZR) s = GameText.action(state[7]);
        return s != null ? s : c.label;
    }

    void setStyle(boolean flexible, boolean showSticks, boolean swipeCamera) {
        if (flexible != this.flexible || swipeCamera != this.swipeCamera) releaseAll();
        this.flexible = flexible;
        this.showSticks = showSticks;
        this.swipeCamera = swipeCamera;
        layoutControls(getWidth(), getHeight());
        invalidate();
    }

    // ---- the menu button hides after a while without touches and comes back on the next touch
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
    }

    @Override
    protected void onDetachedFromWindow() {
        removeCallbacks(hideMenu);
        super.onDetachedFromWindow();
    }

    int buttons() {
        int b = 0;
        if (editing) return 0;
        for (Control c : controls) {
            if (c.kind == KIND_BUTTON && c.pressed) b |= c.bit;
            if (c.kind == KIND_DPAD) b |= c.dpadBits;
        }
        return b;
    }
    float stickX(int i) { return controls.get(5 + i).sx; }
    float stickY(int i) { return controls.get(5 + i).sy; }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        layoutControls(w, h);
    }

    // positions in units of u = shorter side / 7 (landscape)
    private void layoutControls(int W, int H) {
        if (W == 0 || H == 0) return;
        float u = Math.min(W, H) / 7f * scale;
        float m = u * 0.25f;  // edge margin
        float colL = m + u * 1.6f, colR = W - m - u * 1.6f;
        set(0, colL, m + u * 0.4f, u * 1.6f, u * 0.65f);               // ZL
        set(1, colL, m + u * 1.25f, u * 1.6f, u * 0.65f);              // L
        set(2, colR, m + u * 0.4f, u * 1.6f, u * 0.65f);               // ZR
        set(3, colR, m + u * 1.25f, u * 1.6f, u * 0.65f);              // R
        set(4, colL, H * 0.5f - u * 0.1f, u * 0.95f, 0);               // D-pad
        if (flexible) {  // floating: only the size; the centre is where the thumb lands
            controls.get(5).w = controls.get(6).w = u * 1.05f;
        } else {
            set(5, colL, H - m - u * 1.15f, u * 1.05f, 0);             // left stick
            set(6, colR, H - m - u * 1.15f, u * 1.05f, 0);             // right stick
        }
        float fx = colR, fy = H * 0.5f - u * 0.1f, d = u * 0.78f, r = u * 0.72f;
        set(7, fx, fy - d, r, r);                                      // X (top)
        set(8, fx + d, fy, r, r);                                      // A (right)
        set(9, fx, fy + d, r, r);                                      // B (bottom)
        set(10, fx - d, fy, r, r);                                     // Y (left)
        set(11, W * 0.5f - u * 1.1f, H - m - u * 0.35f, u * 0.6f, u * 0.6f);  // -
        set(12, W * 0.5f + u * 1.1f, H - m - u * 0.35f, u * 0.6f, u * 0.6f);  // +
        set(13, colL + u * 1.55f, H - m - u * 0.35f, u * 0.55f, u * 0.55f);   // L3
        set(14, colR - u * 1.55f, H - m - u * 0.35f, u * 0.55f, u * 0.55f);   // R3
        set(15, W * 0.5f, H - m - u * 0.35f, u * 0.6f, u * 0.6f);             // menu
        set(16, W * 0.5f, m + u * 0.35f, u * 0.8f, u * 0.55f);                // screen button
        // context buttons: the shoulder buttons are round, the size of the face buttons
        for (int i = 0; i < 4; i++) {
            Control s = controls.get(i);
            s.round = context && flexible;
            if (s.round) s.w = s.h = u * 0.72f;
        }
        // the player's own places and sizes (layout editor)
        for (Control c : controls) {
            c.w *= c.size;
            c.h *= c.size;
            if (!Float.isNaN(c.px) && !(flexible && c.kind == KIND_STICK)) {
                c.cx = c.px * W;
                c.cy = c.py * H;
            }
        }
        text.setTextSize(u * 0.34f);
        stroke.setStrokeWidth(Math.max(2f, u * 0.04f));
    }

    private void set(int i, float cx, float cy, float w, float h) {
        Control c = controls.get(i);
        c.cx = cx;
        c.cy = cy;
        c.w = w;
        c.h = h;
    }

    private Control controlAt(float x, float y) {
        // exact hits first, then a little slack (thumbs are imprecise)
        for (float slack : new float[] {1f, 1.35f})
            for (Control c : controls) {
                if (!controlsVisible && c.kind != KIND_MENU && !editing) continue;
                if (flexible && c.kind == KIND_STICK) continue;  // floating: not at a fixed place
                if (!inMode(c) && !(editing && c.kind != KIND_SCREEN)) continue;
                if (c.hidden && !editing) continue;
                if (!shown(c)) continue;
                if (c.hit(x, y, slack)) return c;
            }
        return null;
    }

    private void updateStick(Control c, float x, float y) {
        float dx = (x - c.cx) / c.w, dy = -(y - c.cy) / c.w;
        float len = (float) Math.hypot(dx, dy);
        if (len > 1) { dx /= len; dy /= len; }
        c.sx = dx;
        c.sy = dy;
    }

    private void updateDpad(Control c, float x, float y) {
        float dx = x - c.cx, dy = y - c.cy, dead = c.w * 0.25f;
        int b = 0;
        if (dx < -dead && Math.abs(dy) < Math.abs(dx) * 2.4f) b |= Native.LEFT;
        if (dx > dead && Math.abs(dy) < Math.abs(dx) * 2.4f) b |= Native.RIGHT;
        if (dy < -dead && Math.abs(dx) < Math.abs(dy) * 2.4f) b |= Native.UP;
        if (dy > dead && Math.abs(dx) < Math.abs(dy) * 2.4f) b |= Native.DOWN;
        c.dpadBits = b & dpadMask();
    }

    private void release(Control c) {
        c.pressed = false;
        c.sx = c.sy = 0;
        c.dpadBits = 0;
    }

    private void releaseAll() {
        zrLatched = false;
        for (Control c : controls) release(c);
        pointers.clear();
        swipePointer = -1;
        if (drcPointer >= 0) Native.setTouch(false, 0, 0);
        drcPointer = -1;
    }

    private boolean inDrc(float x, float y) { return !drcRect.isEmpty() && drcRect.contains(x, y); }

    private void touchDrc(boolean down, float x, float y) {
        float tx = (x - drcRect.left) / drcRect.width(), ty = (y - drcRect.top) / drcRect.height();
        Native.setTouch(down, Math.max(0, Math.min(1, tx)), Math.max(0, Math.min(1, ty)));
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editing) return editTouch(e);
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
                Control c = controlAt(x, y);
                if (c != null) {
                    if (c.kind == KIND_MENU) {
                        if (!menuWasHidden) listener.onMenu();  // a touch on the hidden button only shows it
                        return true;
                    }
                    if (c.kind == KIND_SCREEN) {
                        listener.onScreenToggle();
                        return true;
                    }
                    // ZR latches for crouching (context buttons: its action is Crouch) and while ZL (targeting)
                    // is latched (the shield stays up); otherwise (fast forward held in cutscenes) it stays a
                    // normal button. A latched ZR is released by a tap.
                    boolean zlLatched = controls.get(0).pressed;
                    boolean crouchLatch = c.bit == Native.ZR && (zrLatched || zlLatched || (contextActive() && state[7] == 0x0F));
                    if (c.latch || crouchLatch) {  // switches on the tap; lifting the finger changes nothing
                        c.pressed = !c.pressed;
                        if (c.bit == Native.ZR) zrLatched = c.pressed;
                        if (c.bit == Native.ZL && !c.pressed && zrLatched) {  // targeting ends: the shield goes down
                            zrLatched = false;
                            for (Control z : controls)
                                if (z.kind == KIND_BUTTON && z.bit == Native.ZR) z.pressed = false;
                        }
                        changed = true;
                        break;
                    }
                    pointers.put(id, c);
                    if (c.kind == KIND_STICK) updateStick(c, x, y);
                    else if (c.kind == KIND_DPAD) updateDpad(c, x, y);
                    else c.pressed = true;
                    changed = true;
                } else if (drcPointer < 0 && inDrc(x, y)) {
                    drcPointer = id;
                    touchDrc(true, x, y);
                } else if (flexible && controlsVisible && !drcMode) {
                    // a floating stick where the thumb landed (one per side), or the swipe camera
                    boolean left = x < getWidth() / 2f;
                    Control s = controls.get(left ? 5 : 6);
                    if (!left && swipeCamera) {
                        if (swipePointer < 0) {
                            swipePointer = id;
                            swipeX = x;
                            swipeY = y;
                        }
                    } else if (pointers.indexOfValue(s) < 0) {
                        s.cx = x;
                        s.cy = y;
                        s.sx = s.sy = 0;
                        pointers.put(id, s);
                        changed = true;
                    }
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
                    if (id == swipePointer) {
                        float k = swipeSpeed / getResources().getDisplayMetrics().density;
                        if (k > 0) Native.cameraSwipe((x - swipeX) * k, (y - swipeY) * k);
                        swipeX = x;
                        swipeY = y;
                        continue;
                    }
                    Control c = pointers.get(id);
                    if (c == null) continue;
                    if (c.kind == KIND_STICK) updateStick(c, x, y);
                    else if (c.kind == KIND_DPAD) updateDpad(c, x, y);
                    else if (c.kind == KIND_BUTTON) {
                        // sliding between buttons (e.g. across the face buttons) moves the press
                        Control now = controlAt(x, y);
                        if (now != null && now != c && now.kind == KIND_BUTTON && !now.latch) {
                            release(c);
                            now.pressed = true;
                            pointers.put(id, now);
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
                if (id == swipePointer) swipePointer = -1;
                Control c = pointers.get(id);
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
        if (changed) {
            listener.onControlsChanged();
            invalidate();
        }
        return true;
    }

    @Override
    protected void onDraw(Canvas canvas) {
        drawClimbHud(canvas);
        drawPerfHud(canvas);
        int a0 = (int) (255 * opacity);
        for (Control c : controls) {
            if (c.kind == KIND_SCREEN && !screenButton) continue;
            if (!editing) {
                if (!inMode(c)) continue;
                if (!controlsVisible && c.kind != KIND_MENU && c.kind != KIND_SCREEN) continue;
                if (c.kind == KIND_MENU && !menuShown) continue;
                if (c.hidden || !shown(c)) continue;
                // floating sticks: only while a thumb holds them, and only if they are shown
                if (flexible && c.kind == KIND_STICK && (!showSticks || pointers.indexOfValue(c) < 0)) continue;
            } else if (flexible && c.kind == KIND_STICK) continue;  // floating: no place to edit
            int a = editing && c.hidden ? a0 / 3 : a0;
            boolean active = c.pressed || c.dpadBits != 0 || c.sx != 0 || c.sy != 0 || (editing && c == sel);
            // style: outline (a faint fill, the outline carries the shape) or filled (a solid shape)
            fill.setColor(active ? 0xFFFFFFFF : filled ? 0xFF303030 : 0xFF202020);
            fill.setAlpha(active ? Math.min(255, a + 60) : filled ? a : a / 4);
            stroke.setColor(0xFFFFFFFF);
            stroke.setAlpha(filled && !active ? a / 3 : a);
            text.setColor(active ? 0xFF000000 : 0xFFFFFFFF);
            text.setAlpha(Math.min(255, a + 40));
            switch (c.kind) {
                case KIND_STICK: {
                    canvas.drawCircle(c.cx, c.cy, c.w, fill);
                    canvas.drawCircle(c.cx, c.cy, c.w, stroke);
                    float kx = c.cx + c.sx * c.w * 0.6f, ky = c.cy - c.sy * c.w * 0.6f;
                    fill.setColor(0xFFFFFFFF);
                    fill.setAlpha(a);
                    canvas.drawCircle(kx, ky, c.w * 0.42f, fill);
                    break;
                }
                case KIND_DPAD: {
                    float s = c.w * 0.36f;
                    int mask = editing ? -1 : dpadMask();
                    if ((mask & Native.UP) != 0) drawDpadArm(canvas, c, 0, -1, s, (c.dpadBits & Native.UP) != 0, a);
                    if ((mask & Native.DOWN) != 0) drawDpadArm(canvas, c, 0, 1, s, (c.dpadBits & Native.DOWN) != 0, a);
                    if ((mask & Native.LEFT) != 0) drawDpadArm(canvas, c, -1, 0, s, (c.dpadBits & Native.LEFT) != 0, a);
                    if ((mask & Native.RIGHT) != 0) drawDpadArm(canvas, c, 1, 0, s, (c.dpadBits & Native.RIGHT) != 0, a);
                    break;
                }
                case KIND_SCREEN: {  // a tablet outline: the GamePad picture (filled while it is shown)
                    tmp.set(c.cx - c.w / 2, c.cy - c.h / 2, c.cx + c.w / 2, c.cy + c.h / 2);
                    canvas.drawRoundRect(tmp, c.h / 3, c.h / 3, fill);
                    canvas.drawRoundRect(tmp, c.h / 3, c.h / 3, stroke);
                    float iw = c.w * 0.5f, ih = c.h * 0.42f;
                    tmp.set(c.cx - iw / 2, c.cy - ih / 2, c.cx + iw / 2, c.cy + ih / 2);
                    Paint p = drcMode ? fill : stroke;
                    int keep = fill.getColor();
                    if (drcMode) fill.setColor(0xFFFFFFFF);
                    canvas.drawRect(tmp, drcMode ? fill : stroke);
                    fill.setColor(keep);
                    break;
                }
                default: {
                    if (c.round) {
                        float r = Math.min(c.w, c.h) / 2;
                        canvas.drawCircle(c.cx, c.cy, r, fill);
                        canvas.drawCircle(c.cx, c.cy, r, stroke);
                    } else {
                        tmp.set(c.cx - c.w / 2, c.cy - c.h / 2, c.cx + c.w / 2, c.cy + c.h / 2);
                        canvas.drawRoundRect(tmp, c.h / 2, c.h / 2, fill);
                        canvas.drawRoundRect(tmp, c.h / 2, c.h / 2, stroke);
                    }
                    android.graphics.Bitmap icon = iconOf(c);
                    if (icon != null) {
                        float s = Math.min(c.w, c.h) * 0.8f;
                        iconSrc.set(0, 0, icon.getWidth(), icon.getHeight());
                        tmp.set(c.cx - s / 2, c.cy - s / 2, c.cx + s / 2, c.cy + s / 2);
                        iconPaint.setAlpha(Math.min(255, a + 70));
                        canvas.drawBitmap(icon, iconSrc, tmp, iconPaint);
                        break;
                    }
                    if (Pictograms.draw(canvas, pictogramOf(c), c.cx, c.cy, Math.min(c.w, c.h) * 0.62f, text.getColor(), c.pressed)) break;
                    String lbl = labelOf(c);
                    float ts = text.getTextSize();
                    float fit = Math.min(c.w, c.h) * (c.round ? 0.85f : 1.6f);
                    if (lbl.length() > 2 && text.measureText(lbl) > fit) text.setTextSize(ts * fit / text.measureText(lbl));
                    canvas.drawText(lbl, c.cx, c.cy - (text.descent() + text.ascent()) / 2, text);
                    text.setTextSize(ts);
                }
            }
        }
        if (editing) drawEditor(canvas);
    }

    private void drawDpadArm(Canvas canvas, Control c, int dx, int dy, float s, boolean on, int a) {
        if (editing && c == sel) on = true;
        float x = c.cx + dx * s * 1.25f, y = c.cy + dy * s * 1.25f;
        tmp.set(x - s * 0.62f, y - s * 0.62f, x + s * 0.62f, y + s * 0.62f);
        fill.setColor(on ? 0xFFFFFFFF : filled ? 0xFF303030 : 0xFF202020);
        fill.setAlpha(on ? Math.min(255, a + 60) : filled ? a : a / 4);
        canvas.drawRoundRect(tmp, s * 0.2f, s * 0.2f, fill);
        canvas.drawRoundRect(tmp, s * 0.2f, s * 0.2f, stroke);
        // context buttons: what the direction does (up conducts, left / right: the boat's cannon and hook)
        if (contextActive()) {
            // the game's own icon, else our pictogram
            String gt = dy < 0 ? "CollectIcon118_15^l" : dx < 0 ? "IconCannon_00^q" : dx > 0 ? "IconSalvage_00^q" : null;
            android.graphics.Bitmap gi = gt == null ? null : GameIcons.get(getContext(), gt, () -> post(this::invalidate));
            if (gi != null) {
                float is = s * 1.05f;
                iconSrc.set(0, 0, gi.getWidth(), gi.getHeight());
                RectF dst = new RectF(x - is / 2, y - is / 2, x + is / 2, y + is / 2);
                iconPaint.setAlpha(Math.min(255, a + 70));
                canvas.drawBitmap(gi, iconSrc, dst, iconPaint);
                return;
            }
            String pg = dy < 0 ? "note" : dx < 0 ? "cannon" : dx > 0 ? "hook" : null;
            int col = on ? 0xFF000000 : 0xFFFFFFFF;
            Pictograms.draw(canvas, pg, x, y, s * 0.95f, (Math.min(255, a + 40) << 24) | (col & 0xFFFFFF), false);
        }
    }
}
