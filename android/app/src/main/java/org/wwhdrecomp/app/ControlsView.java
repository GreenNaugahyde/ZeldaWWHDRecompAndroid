package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.RectF;
import android.os.SystemClock;
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
        /** the mic button (singing the songs, SongListener) pressed / let go */
        default void onSongMic(boolean down) {}
        /** the time switch under the mic button tapped (3/4 -> 4/4 -> 6/4) */
        default void onSongBeat() {}
        /** the time it shows: 3, 4 or 6 */
        default int songBeat() { return 3; }
        /** the take to show on the song panel (SongListener.take), or null */
        default SongListener.Take songTake() { return null; }
        /** the count-in now: 1.. the time's beats, 0 none */
        default int songCount() { return 0; }
        /** practising the songs (the panel shows with its close mark) */
        default boolean songPractice() { return false; }
        /** the practice panel's close mark tapped */
        default void onSongPracticeEnd() {}
        /** a play button on the game's song list tapped: Link plays that song (melody number) */
        default void onSongListPlay(int melody) {}
        /** the 3D view's zoom (Graphics > Field of view, 1 = the game's) */
        default float viewZoom() { return 1f; }
        /** a pinch changed it (done: the fingers let go) */
        default void onViewZoom(float z, boolean done) {}
        /** the singing panel shows / is gone (its own zoom; the picture moves down below it) */
        default void onSongView(boolean panel) {}
        void onMenu();
        /** the performance overlay was dragged: its top left corner as fractions of the view size */
        void onOverlayMoved(float fx, float fy);
        /** the screen button: switch between the TV picture and the GamePad picture */
        void onScreenToggle();
        void onOptionsPage();
        /** a sideways drag could change the page (the items page, the page right of it): the game's pages show and
         * the touch is not on one of the game's buttons (x, y on the game's 16:9 GamePad screen) */
        boolean pageSwipeFrom(float x, float y);
        /** a drag across the game's pages: they follow (dx layout units to the right, Native.setItemPageDrag) */
        void onGamePageDrag(float dx);
        /** let go: dx as above, v in layout units per second; the game's arrow finishes the change, or they glide back */
        void onGamePageDragEnd(float dx, float v);
        /** the options page follows the finger (dx layout units to the right since the drag began) */
        void onPageDrag(float dx);
        /** let go: dx as above, v in layout units per second */
        void onPageDragEnd(float dx, float v);
        /** the layout editor closed: the controls' layout (layoutString), opacity and style */
        void onLayoutEdited(String layout, float opacity, boolean filled);
    }

    // button shapes: the style's own, round, oval (a pill like L / ZL), square, rectangle (both slightly rounded),
    // hexagon (flat top and bottom), diamond (a square on its tip); the last two with rounded corners
    private static final int SHAPE_DEFAULT = 0, SHAPE_ROUND = 1, SHAPE_OVAL = 2, SHAPE_SQUARE = 3, SHAPE_RECT = 4, SHAPE_HEX = 5,
            SHAPE_DIAMOND = 6, SHAPES = 7;
    private static final int[] SHAPE_NAMES = {R.string.edit_shape_default, R.string.edit_shape_round, R.string.edit_shape_oval,
            R.string.edit_shape_square, R.string.edit_shape_rect, R.string.edit_shape_hex, R.string.edit_shape_diamond};

    private static final int KIND_BUTTON = 0, KIND_STICK = 1, KIND_DPAD = 2, KIND_MENU = 3, KIND_SCREEN = 4;

    private static final class Control {
        int kind, bit;
        String label, key;           // key: in the saved layout
        float px = Float.NaN, py = Float.NaN, size = 1f;  // own place (fractions of the view) and size factor
        boolean hidden;
        int shape;                   // buttons: SHAPE_*, the player's own (layout editor); 0: the style's
        float cx, cy, w, h;          // centre and size (buttons: w x h, sticks: w = radius)
        boolean round, pressed;
        final boolean round0;        // round by default
        float corner = 0.5f;         // buttons not round: corner radius / height (0.5: a pill like L)
        float alpha = Float.NaN;     // own opacity (layout editor); NaN: all controls' opacity
        boolean latch;               // a tap switches it on, the next off (ZL: targeting stays on)
        float sx, sy;                // stick deflection -1..1 (+y up)
        float hx = Float.NaN, hy;    // left stick: its place before the thumb pulled it along
        int dpadBits;
        Control(int kind, int bit, String label, boolean round) {
            this.kind = kind;
            this.bit = bit;
            this.label = label;
            this.round = round0 = round;
            key = "BSDMT".charAt(kind) + label;
        }
        boolean hit(float x, float y, float slack) {
            if (kind == KIND_STICK || kind == KIND_DPAD) {
                float r = w * slack;
                return (x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r;
            }
            float ex = Math.abs(x - cx) / (w / 2 * slack), ey = Math.abs(y - cy) / (h / 2 * slack);
            if (shape == SHAPE_DIAMOND) return ex + ey <= 1;
            if (shape == SHAPE_HEX) return ey <= 1 && ex + ey / 2 <= 1;
            return ex <= 1 && ey <= 1;
        }
    }

    private final Listener listener;
    private final List<Control> controls = new ArrayList<>();
    private final SparseArray<Control> pointers = new SparseArray<>();
    private int drcPointer = -1;
    // a touch on the items page off the game's buttons, held back from the game until it shows its way: to the
    // right it pulls in the options page (MainActivity.onPageDrag), otherwise it goes to the game as it began
    private int pagePointer = -1;
    private boolean pageDragging, pageFlipping;
    private float pageX0, pageY0, pageLastX, pageV;
    private long pageLastT;
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
        controls.add(new Control(KIND_BUTTON, Native.PLUS, "+", true));
        controls.add(new Control(KIND_BUTTON, Native.STICK_L, "L3", true));
        controls.add(new Control(KIND_BUTTON, Native.STICK_R, "R3", true));
        controls.add(new Control(KIND_MENU, 0, "≡", true));
        controls.add(new Control(KIND_SCREEN, 0, "", false));  // 15: TV / GamePad picture (GamePad on demand)
        controls.add(new Control(KIND_BUTTON, Native.JUMP, "J", false));  // 16: Jump anywhere (its own input, not L)
        controls.add(new Control(KIND_BUTTON, Native.MOON, "MJ", false));  // 17: the moon jump
    }

    // ---- the full GamePad picture (+, or the screen button of "GamePad on demand"): the TV picture
    // with the controls, or the GamePad picture with only + and the screen button (touches go to the GamePad)
    private boolean screenButton, drcMode;

    void setScreenButton(boolean on, boolean drcShown) {
        screenButton = on;
        if (drcShown != drcMode) releaseAll();
        drcMode = drcShown;
        invalidate();
    }

    // booting, the title screen, the file select and a new game's opening: no controls but the menu
    // button (touches there: A on the title, the GamePad picture on the file select, + in the opening)
    private int front = Native.FRONT_BOOT;

    void setFrontScreen(int f) {
        if (f == front) return;
        if (f != 0) releaseAll();
        front = f;
        invalidate();
    }

    // the options are a page of the GamePad inventory (its arrow left of the items, MainActivity.menuInInventory):
    // no menu button while playing; the title, the file select and booting keep it
    private boolean menuInInventory;

    void setMenuInInventory(boolean on) {
        if (on == menuInInventory) return;
        menuInInventory = on;
        invalidate();
    }

    // Controls > On-screen controls off: the menu button is the way to the menu in play too (+ isn't shown)
    private boolean menuAlone;

    void setMenuAlone(boolean on) {
        if (on == menuAlone) return;
        menuAlone = on;
        invalidate();
    }

    /** whether a control is part of the current screen mode */
    private boolean inMode(Control c) {
        if (front != 0) return c.kind == KIND_MENU;
        if (c.kind == KIND_MENU && menuInInventory && !menuAlone) return false;
        if (c.kind == KIND_SCREEN) return screenButton;
        return !drcMode || (c.kind == KIND_BUTTON && c.bit == Native.PLUS);
    }

    // ---- layout editor: drag a control to move it (its centre locks on the grid while Grid is on), pinch (or Size -/+) to resize it,
    // tap to select it for Hide / Show and (buttons) Shape; a toolbar sets the opacity and style of all controls. The game gets no input.
    // It shows every control of the current style (Classic / Flexible, each with its own saved layout), whatever the
    // game shows now and whichever mods are on.
    private boolean editing, filled, grid = true;
    private Control sel;
    private int dragPtr = -1, pinchPtr = -1;
    private float dragDx, dragDy, pinchDist, pinchSize;
    private static final int[] TOOLS = {R.string.edit_size_minus, R.string.edit_size_plus, R.string.edit_hide, R.string.edit_shape, R.string.edit_opacity,
            R.string.edit_style_outline, R.string.edit_grid_on, R.string.edit_presets, R.string.edit_reset, R.string.edit_done};
    private final RectF[] toolRects = new RectF[TOOLS.length];

    float editOpacity() { return opacity; }
    boolean filled() { return filled; }

    /** a saved layout into the editor (LayoutPresets); Done keeps it */
    void setEditLayout(String layout, float opacity, boolean filled) {
        this.opacity = opacity;
        this.filled = filled;
        sel = null;
        setLayoutString(layout);
    }

    void setFilled(boolean f) {
        filled = f;
        invalidate();
    }

    /** the controls' own places, sizes, hidden flags, shapes and opacities: "key=px,py,size,hidden,shape,alpha;..."
     *  (shape and alpha: since 2026-10-23; alpha NaN: all controls' opacity) */
    String layoutString() {
        StringBuilder b = new StringBuilder();
        for (Control c : controls) {
            if (Float.isNaN(c.px) && c.size == 1f && !c.hidden && c.shape == SHAPE_DEFAULT && Float.isNaN(c.alpha)) continue;
            b.append(c.key).append('=').append(c.px).append(',').append(c.py).append(',').append(c.size).append(',')
                    .append(c.hidden ? 1 : 0).append(',').append(c.shape).append(',').append(c.alpha).append(';');
        }
        return b.toString();
    }

    /** The arc layout (StarterQuestions) for a W x H screen, as a layout string: the swipe camera's room at the bottom
     *  right, around it two quarter arcs. Inner, from the top: Jump, A1 (Y), A, B; outer: crouch (ZR) above Jump,
     *  A2 (X), the moon jump, A3 (R). R3 between + and B, ZL centred on the left edge, the d-pad at the top left, L
     *  hidden (unused in play). In the units of layoutControls, so the arcs stay round on any screen shape. */
    static String arcLayout(int W, int H, float scale) {
        float u = Math.min(W, H) / 7f * scale, m = u * 0.25f;
        float cx = W - m - u * 0.5f, cy = H - m - u * 0.5f, r1 = u * 2.9f, r2 = r1 + u * 1.05f;
        StringBuilder b = new StringBuilder();
        java.util.function.BiConsumer<String, float[]> put = (key, xy) -> b.append(key).append('=').append(xy[0] / W).append(',')
                .append(xy[1] / H).append(",1.0,0,0,NaN;");
        java.util.function.BiFunction<Float, Float, float[]> arc = (r, deg) -> new float[] {
                cx + r * (float) Math.cos(Math.toRadians(deg)), cy - r * (float) Math.sin(Math.toRadians(deg))};
        put.accept("BJ", arc.apply(r1, 90f));
        put.accept("BY", arc.apply(r1, 112.5f));
        put.accept("BA", arc.apply(r1, 135f));
        float[] bb = arc.apply(r1, 157.5f);
        put.accept("BB", bb);
        put.accept("BZR", arc.apply(r2, 90f));
        put.accept("BX", arc.apply(r2, 112.5f));
        put.accept("BMJ", arc.apply(r2, 135f));
        put.accept("BR", arc.apply(r2, 157.5f));
        float plusY = H - m - u * 0.35f;
        put.accept("BR3", new float[] {(W * 0.5f + bb[0]) / 2, plusY});
        put.accept("BZL", new float[] {m + u * 0.5f, H * 0.5f});
        put.accept("D", new float[] {m + u * 1.6f, m + u * 1.15f});
        b.append("BL=NaN,NaN,1.0,1,0,NaN;");
        return b.toString();
    }

    void setLayoutString(String s) {
        for (Control c : controls) {
            c.px = c.py = Float.NaN;
            c.size = 1f;
            c.hidden = false;
            c.shape = SHAPE_DEFAULT;
            c.alpha = Float.NaN;
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
                            if (v.length > 4 && c.kind == KIND_BUTTON) c.shape = Math.max(0, Math.min(SHAPES - 1, Integer.parseInt(v[4])));
                            if (v.length > 5) {
                                float al = Float.parseFloat(v[5]);
                                c.alpha = Float.isNaN(al) ? al : Math.max(0.05f, Math.min(1f, al));
                            }
                        } catch (NumberFormatException ignored) {
                        }
                    }
            }
        layoutControls(getWidth(), getHeight());
        invalidate();
    }

    void setEditing(boolean on) {
        releaseAll();
        sliderOpen = false;
        sliderPtr = -1;
        grid = MainActivity.instance == null || MainActivity.instance.prefs.getBoolean("controls_grid", true);
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
                if (sel != null && sel.kind == KIND_BUTTON) {
                    sel.shape = (sel.shape + 1) % SHAPES;
                    editLayoutChanged();
                }
                break;
            case 4:  // the opacity slider under the toolbar, open or closed
                sliderOpen = !sliderOpen;
                sliderPtr = -1;
                invalidate();
                break;
            case 5:
                filled = !filled;
                invalidate();
                break;
            case 6:
                grid = !grid;
                if (MainActivity.instance != null) MainActivity.instance.prefs.edit().putBoolean("controls_grid", grid).apply();
                invalidate();
                break;
            case 7:  // saved layouts (LayoutPresets): save this one, load one into the editor, export / import
                if (MainActivity.instance != null) MainActivity.instance.layoutPresets().show();
                break;
            case 8:
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

    // ---- the opacity slider (Opacity in the toolbar): the selected control's own opacity, with none selected all controls'
    private boolean sliderOpen;
    private int sliderPtr = -1;
    private final RectF sliderRect = new RectF();
    private static final float OPACITY_MIN = 0.05f;

    private float shownOpacity() { return sel != null && !Float.isNaN(sel.alpha) ? sel.alpha : opacity; }

    private void slideTo(float x) {
        float f = (x - sliderRect.left) / sliderRect.width();
        float v = Math.round((OPACITY_MIN + Math.max(0f, Math.min(1f, f)) * (1f - OPACITY_MIN)) * 100) / 100f;
        if (sel != null) sel.alpha = Math.abs(v - opacity) < 0.005f ? Float.NaN : v;  // at all controls': follows them again
        else opacity = v;
        invalidate();
    }

    private void drawSlider(Canvas canvas, float top, float d) {
        float w = Math.min(getWidth() - 32 * d, 420 * d), h = 44 * d, x0 = (getWidth() - w) / 2;
        sliderRect.set(x0, top, x0 + w, top + h);
        Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);
        p.setColor(0xE0202020);
        canvas.drawRoundRect(sliderRect, h / 2, h / 2, p);
        float pad = h / 2, ty = top + h / 2, l = x0 + pad, r = x0 + w - pad;
        float f = (shownOpacity() - OPACITY_MIN) / (1f - OPACITY_MIN), kx = l + f * (r - l);
        // the track: from faint to solid, filled up to the knob
        p.setStrokeWidth(6 * d);
        p.setStrokeCap(Paint.Cap.ROUND);
        p.setColor(0x50FFFFFF);
        canvas.drawLine(l, ty, r, ty, p);
        p.setColor(0xFFFFC107);
        canvas.drawLine(l, ty, kx, ty, p);
        p.setColor(0xFFFFFFFF);
        canvas.drawCircle(kx, ty, 12 * d, p);
        // the hit area: the whole bar, the knob's reach at the ends included
        sliderRect.set(l, top, r, top + h);
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
                if (sliderOpen && sliderPtr < 0) {
                    tmp.set(sliderRect);
                    tmp.inset(-24 * getResources().getDisplayMetrics().density, 0);
                    if (tmp.contains(x, y)) {
                        sliderPtr = id;
                        slideTo(x);
                        return true;
                    }
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
                if (sliderPtr >= 0) {
                    int si = e.findPointerIndex(sliderPtr);
                    if (si >= 0) slideTo(e.getX(si));
                }
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
                    float x = e.getX(di) + dragDx, y = e.getY(di) + dragDy, g = gridCell();
                    if (grid) {
                        x = snap(x, getWidth() / 2f, g);
                        y = snap(y, getHeight() / 2f, g);
                    }
                    sel.px = x / getWidth();
                    sel.py = y / getHeight();
                }
                editLayoutChanged();
                return true;
            }
            case MotionEvent.ACTION_UP:
            case MotionEvent.ACTION_POINTER_UP:
            case MotionEvent.ACTION_CANCEL: {
                int id = e.getPointerId(idx);
                if (action == MotionEvent.ACTION_CANCEL || id == sliderPtr) sliderPtr = -1;
                if (action == MotionEvent.ACTION_CANCEL || id == dragPtr) dragPtr = pinchPtr = -1;
                else if (id == pinchPtr) pinchPtr = -1;
                return true;
            }
            default:
                return true;
        }
    }

    // the editor's grid: half the layout unit (layoutControls' u), its lines through the screen's centre so that
    // places mirror left / right
    private float gridCell() { return Math.min(getWidth(), getHeight()) / 14f * scale; }

    private static float snap(float v, float origin, float cell) {
        return origin + Math.round((v - origin) / cell) * cell;
    }

    private void drawGrid(Canvas canvas) {
        float g = gridCell(), W = getWidth(), H = getHeight(), d = getResources().getDisplayMetrics().density;
        if (g < 4) return;
        Paint p = new Paint();
        p.setStrokeWidth(Math.max(1f, d * 0.75f));
        int nx = (int) Math.ceil(W / 2 / g), ny = (int) Math.ceil(H / 2 / g);
        for (int i = -nx; i <= nx; i++) {  // every second line (a full unit) stronger, the centre lines strongest
            p.setColor(i == 0 ? 0x60FFFFFF : (i & 1) == 0 ? 0x38FFFFFF : 0x18FFFFFF);
            canvas.drawLine(W / 2 + i * g, 0, W / 2 + i * g, H, p);
        }
        for (int i = -ny; i <= ny; i++) {
            p.setColor(i == 0 ? 0x60FFFFFF : (i & 1) == 0 ? 0x38FFFFFF : 0x18FFFFFF);
            canvas.drawLine(0, H / 2 + i * g, W, H / 2 + i * g, p);
        }
        if (sel != null) {  // the selected control's grid lines
            p.setColor(0xC0FFC107);
            p.setStrokeWidth(Math.max(1.5f, d * 1.5f));
            canvas.drawLine(sel.cx, 0, sel.cx, H, p);
            canvas.drawLine(0, sel.cy, W, sel.cy, p);
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
        for (int pass = 0; pass < 2; pass++) {  // longer labels (other languages, narrow phones): smaller to fit across
            total = 0;
            for (int i = 0; i < TOOLS.length; i++) {
                w[i] = t.measureText(toolLabel(i)) + 2 * pad;
                total += w[i] + gap;
            }
            if (pass > 0 || total - gap <= getWidth() - 2 * gap) break;
            float k = Math.max(0.5f, (getWidth() - 2 * gap - TOOLS.length * (2 * pad + gap)) / (total - TOOLS.length * (2 * pad + gap)));
            t.setTextSize(t.getTextSize() * k);
        }
        float x = (getWidth() - total + gap) / 2, y = 10 * d;
        Paint bg = new Paint(Paint.ANTI_ALIAS_FLAG);
        for (int i = 0; i < TOOLS.length; i++) {
            boolean off = (i <= 3 && sel == null) || (i == 2 && sel != null && (sel.kind == KIND_MENU || sel.kind == KIND_SCREEN))
                    || (i == 3 && sel != null && sel.kind != KIND_BUTTON);
            if (toolRects[i] == null) toolRects[i] = new RectF();
            toolRects[i].set(x, y, x + w[i], y + h);
            bg.setColor(i == TOOLS.length - 1 ? 0xE02E7D32 : 0xE0202020);
            canvas.drawRoundRect(toolRects[i], h / 2, h / 2, bg);
            t.setColor(off ? 0xFF808080 : 0xFFFFFFFF);
            canvas.drawText(toolLabel(i), x + w[i] / 2, y + h / 2 - (t.descent() + t.ascent()) / 2, t);
            x += w[i] + gap;
        }
        float ty = y + h;
        if (sliderOpen) {
            drawSlider(canvas, ty + 8 * d, d);
            ty += 52 * d;
        }
        t.setTextSize(13 * d);
        t.setColor(0xFFFFFFFF);
        t.setShadowLayer(3 * d, 0, 0, 0xFF000000);
        canvas.drawText(getContext().getString(R.string.edit_help), getWidth() / 2f, ty + 22 * d, t);
        canvas.drawText(getContext().getString(flexible ? R.string.edit_layout_flexible : R.string.edit_layout_classic),
                getWidth() / 2f, ty + 42 * d, t);
        if (sliderOpen)
            canvas.drawText(getContext().getString(sel != null ? R.string.edit_opacity_one : R.string.edit_opacity_all),
                    getWidth() / 2f, ty + 62 * d, t);
    }

    private String toolLabel(int i) {
        if (i == 2) return getContext().getString(sel != null && sel.hidden ? R.string.edit_show : R.string.edit_hide);
        if (i == 3) return getContext().getString(R.string.edit_shape,
                getContext().getString(SHAPE_NAMES[sel != null && sel.kind == KIND_BUTTON ? sel.shape : SHAPE_DEFAULT]));
        if (i == 4) return getContext().getString(R.string.edit_opacity, Math.round(shownOpacity() * 100));
        if (i == 5) return getContext().getString(filled ? R.string.edit_style_filled : R.string.edit_style_outline);
        if (i == 6) return getContext().getString(grid ? R.string.edit_grid_on : R.string.edit_grid_off);
        return getContext().getString(TOOLS[i]);
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

    // "Res: 0.85× 1088×612  Dyn ≥0.5× GPU 93%": the resolution in effect, live (dynamic resolution moves it)
    private static String resLine(PerfStats p) {
        StringBuilder b = new StringBuilder("Res: ").append(num(p.resScale, "%.2f")).append("× ")
                .append(Math.round(p.resW)).append('×').append(Math.round(p.resH));
        if (p.dynMin > 0) {
            b.append("  Dyn ≥").append(num(p.dynMin, "%.2f")).append('×');
            if (p.dynBusy >= 0) b.append(" GPU ").append(num(p.dynBusy * 100, "%.0f")).append('%');
        }
        return b.toString();
    }

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
        if ((it & (PERF_FRAME | PERF_SETTINGS)) != 0) list.add(resLine(p));
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
        // default: below the top edge's system overlays and the menu button, at the left; else where the user put it
        Control menu = controls.get(14);
        float x = perfFx < 0 ? 12 * density : perfFx * getWidth();
        float y = perfFy >= 0 ? perfFy * getHeight()
                : menu.cx - menu.w / 2 < x + bw ? Math.max(40 * density, menu.cy + menu.h / 2 + 8 * density) : 40 * density;
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
    private int[] state = new int[34];  // Native.touchState (mods.h)
    private boolean l3InUse;
    private int jumpBits;  // jumpInputs() as last polled (a change redraws)
    private boolean zrLatched;  // ZR held by a tap for crouching
    private boolean zrCrouch;   // ... latched while its action was Crouch: it shows Link standing (stand up)

    // A latched ZL (targeting) when the locked-on enemy is defeated (Native.touchState [30..32]): it lets go,
    // or (Controls > Defeated target: Next target) lets go for a moment and presses again, which locks the
    // next enemy in reach (the game locks only on a new press); with none in reach it stays let go
    private int afterDefeat;  // 0 let go, 1 the next target
    private int defeats = -1;
    private int relockGen;  // a tap on ZL meanwhile cancels the press again
    private static final int RELOCK_GAP_MS = 120, RELOCK_CHECK_MS = 400;  // (logic steps at 30 per second)
    void setAfterDefeat(int mode) { afterDefeat = mode; }

    private void targetDefeated(int[] s) {
        Control zl = controls.get(0);
        if (!zl.pressed || s[31] != 0) return;  // not latched, or already on another target
        releaseZl();
        if (afterDefeat != 1) return;
        int gen = ++relockGen;
        postDelayed(() -> {  // the targets in reach then: in the step the lock ends the game's list is still empty
            if (gen != relockGen || zl.pressed || state.length <= 32 || state[32] <= 0) return;
            zl.pressed = true;
            listener.onControlsChanged();
            invalidate();
            postDelayed(() -> {
                if (gen == relockGen && zl.pressed && state.length > 31 && state[31] == 0) releaseZl();  // it locked nothing
            }, RELOCK_CHECK_MS);
        }, RELOCK_GAP_MS);
    }

    private boolean bPressed() {
        for (Control c : controls)
            if (c.kind == KIND_BUTTON && c.bit == Native.B && c.pressed) return true;
        return false;
    }

    private void releaseZl() {
        controls.get(0).pressed = false;
        if (zrLatched) {  // the shield goes down, as when ZL is tapped off
            zrLatched = false;
            for (Control z : controls)
                if (z.kind == KIND_BUTTON && z.bit == Native.ZR) z.pressed = false;
        }
        listener.onControlsChanged();
        invalidate();
    }

    // ---- item buttons by tap (Controls > Item buttons: Tap; context buttons only; notes/touch-item-use.md). The bow,
    // boomerang, hookshot and grappling hook take a press to aim (first person) and fire on its release. A tap on such
    // an item's button holds it (latched: Link aims and draws; ItemRing.passThrough keeps the ring shut); the button
    // then shows Shoot, and a tap on it lets go (it fires) and presses again SHOOT_GAP_MS later once Link aims with
    // nothing out (the next arrow; the hookshot / hook back first). The aim ending (Return, ZL let go, the boomerang
    // thrown, Link on the rope) lets go. ZL while aiming shows the view a tap switches to: Link (third person: ZL
    // latched) or the eye (first person: ZL lets go, which ends the aim in the game, then the item is pressed again).
    // With an item in Link's hand B is Put away (A's Return / Put away are hidden): A Return while aiming, then A Put
    // away, which the game offers only while the stick is idle (it is held still meanwhile); not offered within
    // PUT_AWAY_MS (moving by controller...): the game's B after all (its sword swap)
    private boolean itemTap, aimFocus;  // aimFocus: only the aiming buttons while latched (aimOnly)
    private Control aimBtn;          // the item button latched
    private Control tapCandidate;    // an aiming item's button under a finger: a tap latches it
    private long tapDownAt, aimSince, shootAt, reaimAt;
    private boolean aimSeen, reaim;  // Link aimed since the latch; ZL let go for first person: press again
    private long putAwaySince = -1, lastAPulse;
    private boolean stowBusy;
    private long stowSince = -1;     // the Deku Leaf (on the ground, not waving) or the sword (no swing, no target) in Link's
                                     // hand since: LEAF_STOW_MS / SWORD_STOW_MS later it goes
    private static final int DEKU_LEAF = 0x34, SWORD = 0x103;
    private static final long LEAF_STOW_MS = 3000, SWORD_STOW_MS = 5000;
    private boolean padPlay;         // a controller in use (MainActivity.inputSource): A puts the item away, as on the Wii U
    private boolean viewWas;         // the Picto Box's or the telescope's view was open: closed, the item goes away
    private static final int ST0_AIMS = 0x1000 | 0x4000 | 0x20000 | 0x80000, ST0_ITEM_OUT = 0x40000;
    private static final long TAP_MAX_MS = 380, SHOOT_GAP_MS = 100, AIM_WAIT_MS = 1000, REAIM_GAP_MS = 80, PUT_AWAY_MS = 1200;

    void setItemTap(boolean on) {
        if (!on) dropAim();
        itemTap = on;
        invalidate();
    }

    void setAimFocus(boolean on) {
        aimFocus = on;
        invalidate();
    }

    private boolean tapMode() { return itemTap && contextActive(); }

    /** aiming (latched, or as the game says) by tap with Only aiming buttons on: the aimed item (Shoot), B (Put away), ZL and ZR (arrow types)
     *  stay; the other item buttons (a tap on one lets go of a drawn arrow), A, R3, +, the d-pad and the jump buttons go */
    private boolean aimOnly() { return aimFocus && tapMode() && (aimBtn != null || aiming()); }

    /** the button of the item Link aims with (latched, or the one holding the item in his hand) */
    private boolean aimButton(Control c) {
        if (c == aimBtn) return true;
        int slot = slotOf(c);
        return slot >= 0 && state.length > 29 && state[3 + slot] == state[29];
    }

    /** the bow (with fire / ice / light arrows), boomerang, hookshot, grappling hook */
    private static boolean aimItem(int item) {
        return item == 0x27 || item == 0x35 || item == 0x36 || item == 0x2D || item == 0x2F || item == 0x25;
    }

    private static int slotOf(Control c) { return c.bit == Native.X ? 0 : c.bit == Native.Y ? 1 : c.bit == Native.R ? 2 : -1; }

    /** aiming one of them (first or third person) */
    private boolean aiming() { return (state[12] & ST0_AIMS) != 0; }

    /** an item in Link's hand (not the sword, the Wind Waker or the Deku Leaf: B keeps the sword there, the leaf and
     *  the sword go away by themselves, handStow), with an icon: B puts it away; not in the Picto
     *  Box's or the telescope's view: B goes back there (then the item goes away by itself, pollState) */
    private boolean itemInHand() {
        return state.length > 29 && !pictoMode() && !telescope() && state[29] >= 0 && state[29] < 0xFF && state[29] != 0x22 && state[29] != DEKU_LEAF && GameIcons.itemTexture(state[29]) != null;
    }

    /** aiming the Picto Box or looking through the telescope: B (the game's Back) closes the view and the item then
     *  goes away (pollState), so B shows Put away */
    private boolean viewStows() { return pictoAim() && !pictoChoice() && state[6] == 0x07 || telescope() && state[6] != 0; }

    private void dropAim() {
        reaim = false;
        if (aimBtn == null) return;
        aimBtn.pressed = false;
        aimBtn = null;
        listener.onControlsChanged();
        invalidate();
    }

    private void latchAim(Control c) {
        aimBtn = c;
        if (ring != null) ring.passThrough(c.bit);
        c.pressed = true;
        aimSince = SystemClock.uptimeMillis();
        aimSeen = false;
        shootAt = 0;
        startAimTick();
    }

    private void startAimTick() {
        removeCallbacks(aimTick);
        post(aimTick);
    }

    private void startPutAway() {
        reaim = false;
        if (controls.get(0).pressed && aiming()) releaseZl();  // third person: letting go of ZL ends the aim
        putAwaySince = SystemClock.uptimeMillis();
        lastAPulse = 0;
        startAimTick();
    }

    /** B's Put away, a step: true if the buttons changed */
    private boolean putAwayStep(long now) {
        if (!itemInHand() || !contextActive()) {  // put away (or out of play)
            putAwaySince = -1;
            return true;
        }
        // A offers something else (facing someone to talk to, a postbox): wait for its Put away rather than B, which
        // would swing the sword at them
        if (state[1] != 0 && state[1] != 0x07 && state[1] != 0x08) {
            putAwaySince = now;
            return false;
        }
        if (now - putAwaySince > PUT_AWAY_MS) {  // not offered: the game's B
            putAwaySince = -1;
            pulse(Native.B);
            return true;
        }
        // A: Return while aiming (the latched item stays held: letting go would shoot), then Put away
        if ((state[1] == 0x07 || state[1] == 0x08) && now - lastAPulse > TITLE_A_MS + 150) {
            lastAPulse = now;
            pulse(Native.A);
        }
        return false;
    }

    private int leafPointer = -1;    // a finger on the Deku Leaf's button while gliding
    private Control leafBtn;
    private static final long LEAF_WAVE_MS = 450;

    // the finger still on the Deku Leaf's button while gliding: the game's own press, a wave in the air
    private final Runnable leafWave = new Runnable() {
        @Override
        public void run() {
            if (leafPointer < 0 || !leafFlying()) return;
            leafBtn.pressed = true;
            listener.onControlsChanged();
            invalidate();
        }
    };

    /** gliding with the Deku Leaf (Link's hand then reads 0x102, not the leaf) */
    private boolean leafFlying() { return context && (state[13] & ST1_LEAF_FLY) != 0; }

    /** the Deku Leaf or the sword in Link's hand: by tap it goes away by itself (handStow), A's Put away is hidden;
     *  not while a controller is in use (its A puts it away) */
    private boolean stowsItself() { return !padPlay && state.length > 29 && (state[29] == DEKU_LEAF || state[29] == SWORD); }

    /** a controller in use (true) or playing by touch again (false): the hand's item goes away by itself only by touch;
     *  back to touch its time starts anew (pollState only looks again when the game's state changes) */
    void setPadPlay(boolean on) {
        if (on == padPlay) return;
        padPlay = on;
        removeCallbacks(handStow);
        stowSince = -1;
        if (!on && tapMode() && stowsItself()) {
            stowSince = SystemClock.uptimeMillis();
            stowBusy = false;
            if (aAction() == 0x08) postDelayed(handStow, state[29] == SWORD ? SWORD_STOW_MS : LEAF_STOW_MS);
        }
        invalidate();
    }

    // the Deku Leaf or the sword held while Link stood still for LEAF_STOW_MS / SWORD_STOW_MS: A, the game's Put away
    private final Runnable handStow = new Runnable() {
        @Override
        public void run() {
            if (stowSince < 0 || !tapMode() || !stowsItself() || aAction() != 0x08) return;
            long left = stowSince + (state[29] == SWORD ? SWORD_STOW_MS : LEAF_STOW_MS) - SystemClock.uptimeMillis();
            if (left > 0) postDelayed(this, left);  // a swing since (B) started the time again
            else pulse(Native.A);
        }
    };

    private final Runnable aimTick = new Runnable() {
        @Override
        public void run() {
            long now = SystemClock.uptimeMillis();
            boolean changed = false;
            if (putAwaySince >= 0) changed = putAwayStep(now);
            Control c = aimBtn;
            if (c != null) {
                boolean aim = aiming(), out = (state[12] & ST0_ITEM_OUT) != 0;
                if (aim) aimSeen = true;
                if (!tapMode()) {
                    dropAim();
                } else if (reaim) {  // ZL let go: the game ends the aim; then the item again, for first person
                    if (c.pressed && !aim) {
                        c.pressed = false;
                        reaimAt = now;
                        changed = true;
                    } else if (!c.pressed && now - reaimAt >= REAIM_GAP_MS) {
                        reaim = false;
                        latchAim(c);
                        changed = true;
                    }
                } else if (aimSeen && !aim && !out || !aimSeen && now - aimSince > AIM_WAIT_MS) {
                    dropAim();  // the aim ended, or it didn't start
                } else if (!c.pressed && aim && !out && now - shootAt >= SHOOT_GAP_MS) {  // after a shot: held again
                    if (ring != null) ring.passThrough(c.bit);
                    c.pressed = true;
                    changed = true;
                }
            }
            if (changed) {
                listener.onControlsChanged();
                invalidate();
            }
            removeCallbacks(this);
            if (aimBtn != null || putAwaySince >= 0) postDelayed(this, 33);
        }
    };
    private final Runnable pollState = new Runnable() {
        @Override
        public void run() {
            int[] s = Native.touchState();
            if (s.length > 32 && s[30] != defeats) {
                if (defeats >= 0) targetDefeated(s);
                defeats = s[30];
            }
            boolean l3 = MainActivity.instance != null && MainActivity.instance.l3InUse();
            int l = MainActivity.instance != null ? MainActivity.instance.jumpInputsInUse() : 0;
            boolean as = context && Native.getOption("album_screen") != 0;
            if (!java.util.Arrays.equals(s, state) || l3 != l3InUse || l != jumpBits || as != albumScreen) {
                albumScreen = as;
                state = s;
                // the Picto Box's or the telescope's view closed (B, R): the game leaves the item in Link's hand
                // (A: Put away); by tap it goes away at once, as B's Put away
                boolean view = pictoMode() || telescope();
                if (viewWas && !view && tapMode() && itemInHand() && aimBtn == null && putAwaySince < 0) startPutAway();
                viewWas = view;
                // the first glide with the Deku Leaf by tap: how its button closes / waves it (once, GameHint)
                if (leafFlying() && tapMode() && controlsVisible && !editing) offerHint(HINT_LEAF_GLIDE);
                // the Deku Leaf held LEAF_STOW_MS (walking counts; flying and waving start again), the sword
                // SWORD_STOW_MS (walking counts; B, ZL and a target start again): put away as soon as Link stands
                // still (the game offers A's Put away only then)
                removeCallbacks(handStow);
                if (!tapMode() || !stowsItself()) stowSince = -1;
                else {
                    long now = SystemClock.uptimeMillis();
                    boolean sword = state[29] == SWORD;
                    boolean busy = sword ? controls.get(0).pressed || state[31] != 0 || bPressed()
                            : (state[13] & (ST1_LEAF_FLY | ST1_LEAF_FAN)) != 0;
                    if (stowSince < 0 || busy || stowBusy) stowSince = now;  // also as the flight / wave / swing ends
                    stowBusy = busy;
                    if (aAction() == 0x08) postDelayed(handStow, Math.max(0, stowSince + (sword ? SWORD_STOW_MS : LEAF_STOW_MS) - now));
                }
                if (pictoMode() && zrLatched) {  // a ZR latched for crouching would switch the Picto Box modes back and forth
                    zrLatched = false;
                    for (Control z : controls)
                        if (z.kind == KIND_BUTTON && z.bit == Native.ZR) z.pressed = false;
                    listener.onControlsChanged();
                }
                l3InUse = l3;
                jumpBits = l;
                if (cameraLaid != cameraLayout()) layoutControls(getWidth(), getHeight());
                invalidate();
            }
            boolean scene = contextActive() && state[8] != 0;
            boolean msg = (singing() || conductPads() || scene) && Native.msgWindow()[0] != 0;
            if (msg != msgShown) {
                msgShown = msg;
                invalidate();
            }
            // a cutscene without text (a door, too): no buttons until the game shows a message; once it showed one, the
            // gaps between two messages keep them (QUIET_SCENE_MS), as does a dialogue's start (its box opens a moment
            // later); not while Link conducts or the scene waits for a song. A fade / wipe (scene change): none either
            long t = SystemClock.uptimeMillis();
            boolean conducts = minigame != 0 || songWanted >= 0 || state[29] == 0x22 || (state[13] & ST1_CONDUCT) != 0;
            if (!scene) sceneText = false;
            if (msg) sceneText = true;
            if (!scene || msg || conducts) textSeenAt = t;
            boolean grace = sceneText || state[8] == 1;
            boolean quiet = scene && !conducts && !msg && (!grace || t - textSeenAt >= QUIET_SCENE_MS)
                    || context && flexible && !editing && state[33] != 0;
            if (quiet != quietScene) {
                quietScene = quiet;
                invalidate();
            }
            int mg = minigameOn ? Native.minigameMode() : 0;
            if (mg != minigame) {
                minigameSince = SystemClock.uptimeMillis();
                hintAsked = false;
                minigame = mg;
                if (mg == 0) minigamePointer = minigamePinch = minigamePointer2 = -1;
                resetPads();  // (each time Link takes the baton out: the centre, 3/4)
                listener.onControlsChanged();
                invalidate();
            }
            // a talk: its buttons stay away TALK_HOLD_MS after it (the event mode drops for a moment between two parts of
            // a talk: A showed Next then)
            if (talkNow()) talkSeenAt = t;
            boolean held = talking() && !talkNow();
            if (held != talkHeld) {
                talkHeld = held;
                invalidate();
            }
            String talkLog = contextActive() ? state[8] + " msg " + msg + " text " + sceneText + " mg " + minigame + " quiet " + quietScene + " held " + held : "";
            if (!talkLog.equals(lastTalkLog)) {  // (what the buttons go by in an event: for the gaps of a talk)
                if (!talkLog.isEmpty() && (state[8] != 0 || !lastTalkLog.isEmpty() && !lastTalkLog.startsWith("0 ")))
                    android.util.Log.i("wwhd", "touch talk: event " + talkLog);
                lastTalkLog = talkLog;
            }
            noteNewAction();
            if (padsLaid != conductPads()) {  // (B's place while conducting by the note pads)
                layoutControls(getWidth(), getHeight());
                invalidate();
            }
            // its first time: how it's played (once the game shows it: not during a state's loading picture)
            if (minigame != 0 && !hintAsked && SystemClock.uptimeMillis() - minigameSince > 1200) {
                hintAsked = true;
                int hm = minigame == MINIGAME_CONDUCT && conductPadsOn ? HINT_CONDUCT_PAD : minigame;
                if (controlsVisible && hint.offer(hm)) releaseAll();
            }
            if (minigame != 0) {
                float[] m = Native.minigameMarks();
                if (m.length > 0 || minigameMarks.length > 0) {
                    minigameMarks = m;
                    invalidate();
                }
            }
            if (zooming()) {
                if (state[23] == 0) zoomPointer = pinchA = pinchB = -1;  // the zoom bar is gone (left the Picto Box / telescope)
                float ly = zoomLy;
                updateZoomStick();
                if (ly != zoomLy) listener.onControlsChanged();
            }
            postDelayed(this, zooming() || (minigame != 0 && minigameMarks.length > 0) ? 33 : 100);  // zooming: follow the handle closely
        }
    };

    // ---- minigames played by touch (mods/minigames.cpp): while one runs, a finger on the TV picture (not on a
    // control) goes to the game: it picks Sploosh Kaboom's square, slides a picture puzzle's piece, ...
    private int minigame;  // Native.minigameMode()
    private final GameHint hint = new GameHint(this);

    /** a controller / the keyboard while a hint shows (MainActivity.pushInput): true while the pad is the hint's */
    boolean hintPad(int buttons, float stickY) { return hint.onPad(buttons, stickY); }

    /** back from the background (MainActivity.onResume): leaving let the game run (for its state), a hint
     *  still open waits again */
    void resumed() {
        if (hint.showing()) Native.setGamePace(2);
    }
    private long minigameSince;
    private boolean hintAsked;
    private static final int MINIGAME_FIGURE = 9;  // looking at a figure: two fingers pinch
    private static final int MINIGAME_CONDUCT = 10;  // conducting: two fingers, each a stick
    private static final int MINIGAME_WIND = 11;  // the wind compass: a finger on it drags its blue arrow (elsewhere: the stick)
    private static final float WIND_R = 190;  // layout units around the compass's centre
    private int minigamePinch = -1, minigamePointer2 = -1;
    private float minigamePinch0;
    private static final int MINIGAME_CHOICE = 6;  // from here on (choices, number entry, shops, figures, conducting): the buttons stay
    private int minigamePointer = -1;
    private float[] minigameMarks = new float[0];  // debug marks (Native.minigameMarks)

    private boolean minigameOn = true;  // the option (Controls > Minigames by touch)

    // a talk held by touch (Controls > Minigames & hints > Hold to go on): while the finger stays down, the game side
    // (mods/minigames.cpp talk_hold_step) presses A each time the text waits for it, up to a question; with its fast
    // forward on, the talk also runs fast forward while held
    private boolean talkHold = true;
    private int talkPointer = -1;

    void setTalkHold(boolean on, boolean ff) {
        talkHold = on;
        Native.setOption("talk_hold_ff", on && ff ? 1 : 0);
        if (!on && talkPointer >= 0) {
            talkPointer = -1;
            Native.setOption("talk_hold", 0);
        }
    }

    void setMinigameTouch(boolean on) {
        minigameOn = on;
        if (!on) {
            minigame = 0;
            minigamePointer = -1;
        }
    }

    private boolean minigameTouchable(float x, float y) {
        if (minigame == 0 || drcMode || tvRect.isEmpty() || !tvRect.contains(x, y)) return false;
        if (minigame == MINIGAME_CONDUCT && conductPadsOn) return false;  // (the note pads instead)
        if (minigame != MINIGAME_WIND) return true;
        int[] w = Native.windCompass();
        float k = layoutScale();
        return w.length == 3 && w[0] != 0 && Math.hypot((x - tvRect.centerX()) / k - w[1], (tvRect.centerY() - y) / k - w[2]) < WIND_R;
    }

    private void minigameTouch(int action, float x, float y) {
        float k = layoutScale();
        Native.minigameTouch(action, (x - tvRect.centerX()) / k, (tvRect.centerY() - y) / k);
    }

    private void drawMinigameMarks(Canvas canvas) {
        if (minigame == 0 || minigameMarks.length < 2 || tvRect.isEmpty()) return;
        float k = layoutScale(), r = 4 * getResources().getDisplayMetrics().density;
        int fc = fill.getColor();
        for (int i = 0; i + 1 < minigameMarks.length; i += 2) {
            fill.setColor(i == 0 ? 0xFFFF3030 : 0xFFFFE020);
            canvas.drawCircle(tvRect.centerX() + minigameMarks[i] * k, tvRect.centerY() - minigameMarks[i + 1] * k, r, fill);
        }
        fill.setColor(fc);
    }

    // ---- the 3D view's zoom (Graphics > Field of view, Native "view_zoom"): two fingers that land on the
    // picture together (within VIEW_PINCH_MS, near each other) pinch it, wherever they are; while singing
    // any two free fingers do. Not in minigames (their own touches) or the Picto Box / telescope (theirs).
    private static final long VIEW_PINCH_MS = 350;
    private static final float VIEW_ZOOM_MAX = 2f;
    private int viewPinchA = -1, viewPinchB = -1, freeFinger = -1;
    private long freeFingerT;
    private float viewPinchDist0, viewZoom0, viewZoomNow = Float.NaN;

    /** a finger landed on no control: the second of two pinches the view (true: it is taken) */
    private boolean viewPinchStart(int id, float x, float y) {
        boolean minigameOwn = minigame != 0 && !singing();
        if (front != 0 || drcMode || minigameOwn || pinchMode() || albumSwipe() || tvRect.isEmpty() || !tvRect.contains(x, y)
                || viewPinchA >= 0) return false;
        long now = SystemClock.uptimeMillis();
        float[] f = freeFinger >= 0 && freeFinger != id ? fingerAt.get(freeFinger) : null;
        boolean together = f != null && (singing()
                || now - freeFingerT < VIEW_PINCH_MS && Math.hypot(x - f[0], y - f[1]) < getWidth() * 0.45f);
        if (!together) {
            freeFinger = id;
            freeFingerT = now;
            return false;
        }
        // what the first finger began (a floating stick, the swipe camera, conducting) lets go
        int first = freeFinger;
        Control held = pointers.get(first);
        if (held != null) {
            release(held);
            pointers.remove(first);
        }
        if (swipePointer == first) swipePointer = -1;
        if (minigamePointer == first) {
            Native.minigameTouch(2, Float.NaN, Float.NaN);  // (cancelled: no target)
            minigamePointer = -1;
        }
        viewPinchA = first;
        viewPinchB = id;
        viewPinchDist0 = Math.max(1f, (float) Math.hypot(x - f[0], y - f[1]));
        viewZoom0 = listener.viewZoom();
        freeFinger = -1;
        return true;
    }

    private void viewPinchMove() {
        float[] a = fingerAt.get(viewPinchA), b = fingerAt.get(viewPinchB);
        if (a == null || b == null) return;
        float d = Math.max(1f, (float) Math.hypot(a[0] - b[0], a[1] - b[1]));
        // fingers apart: closer (the game's view at most), together: wider
        float z = Math.max(1f, Math.min(VIEW_ZOOM_MAX, viewZoom0 * viewPinchDist0 / d));
        if (z < 1.03f) z = 1f;
        viewZoomNow = z;
        listener.onViewZoom(z, false);
    }

    private void viewPinchEnd() {
        if (viewPinchA < 0) return;
        viewPinchA = viewPinchB = -1;
        if (!Float.isNaN(viewZoomNow)) listener.onViewZoom(viewZoomNow, true);
        viewZoomNow = Float.NaN;
    }

    // the singing panel covers the picture's upper middle: the 3D picture's centre moves down into the
    // free part below it while it shows (MainActivity.onSongView)
    private boolean songViewShown;

    private void updateSongView(boolean panel) {
        if (panel == songViewShown) return;
        songViewShown = panel;
        listener.onSongView(panel);
    }

    // ---- zooming the Picto Box / telescope: the game's zoom bar (hud.h) has a handle the left stick
    // moves (zooming in moves it up). Classic view: a finger on the bar drags the handle. Large Picto Box
    // view (the bar hidden): two fingers pinch. Either way the stick is pushed until the handle is where
    // the finger / the pinch wants it.
    private boolean pictoBig;
    private int zoomPointer = -1;
    private int pinchA = -1, pinchB = -1, pinchFirst = -1;  // pinching fingers; a free finger that may become one
    private float pinchDist0, pinchHandle0;
    private float zoomTarget, zoomLy;  // layout units (720 high, y up, 0 at the centre); stick

    void setPictoBig(boolean on) {
        pictoBig = on;
        if (cameraLaid != cameraLayout()) layoutControls(getWidth(), getHeight());
    }

    // ---- the Picto Box's buttons at fixed places (layoutControls)
    private boolean cameraLaid;

    private boolean cameraLayout() { return pictoBig && !editing && contextActive() && pictoMode(); }

    // ---- the Picto Box's album with the large view: a finger swiped sideways browses the photos (as a
    // d-pad press, one per swipe distance; leftwards = the next photo)
    private int albumPointer = -1, albumBits;
    private float albumX;
    private final Runnable albumRelease = new Runnable() {
        @Override
        public void run() {
            albumBits = 0;
            listener.onControlsChanged();
        }
    };

    private boolean albumSwipe() { return pictoBig && album() && (state[1] != 0x17 || chooser()); }

    private void albumMove(float x) {
        float step = 60 * getResources().getDisplayMetrics().density;
        if (Math.abs(x - albumX) < step || albumBits != 0) return;
        albumBits = x < albumX ? Native.RIGHT : Native.LEFT;
        albumX = x;
        listener.onControlsChanged();
        postDelayed(albumRelease, 120);
    }

    private boolean zooming() { return zoomPointer >= 0 || pinchA >= 0; }

    /** the large Picto Box view's zoom (Picto Box or telescope on screen): free fingers pinch */
    private boolean pinchMode() { return context && pictoBig && state[23] != 0 && !tvRect.isEmpty(); }

    private float pinchDistance(MotionEvent e) {
        int a = e.findPointerIndex(pinchA), b = e.findPointerIndex(pinchB);
        if (a < 0 || b < 0) return pinchDist0;
        return (float) Math.hypot(e.getX(a) - e.getX(b), e.getY(a) - e.getY(b));
    }

    // spreading the fingers to twice their distance zooms over half the bar
    private void pinchTo(float dist) {
        float range = state[26] - state[25];
        float t = pinchHandle0 + range * 0.5f * (float) (Math.log(Math.max(dist, 1f) / Math.max(pinchDist0, 1f)) / Math.log(2));
        zoomTarget = Math.max(state[25], Math.min(state[26], t));
        updateZoomStick();
    }

    private void endPinch() {
        pinchA = pinchB = -1;
        zoomLy = 0;
    }

    private float layoutScale() { return tvRect.height() / 720f; }

    private boolean inZoomBar(float x, float y) {
        if (!context || pictoBig || state[23] == 0 || tvRect.isEmpty()) return false;
        float lx = (x - tvRect.centerX()) / layoutScale(), ly = (tvRect.centerY() - y) / layoutScale();
        return lx >= state[27] && lx <= state[28] && ly >= state[25] - 40 && ly <= state[26] + 40;
    }

    private void zoomTo(float y) {
        float ly = (tvRect.centerY() - y) / layoutScale();
        zoomTarget = Math.max(state[25], Math.min(state[26], ly));
        updateZoomStick();
    }

    private void updateZoomStick() {
        if (!zooming()) { zoomLy = 0; return; }
        float e = zoomTarget - state[24];  // the handle moves up when zooming in (stick up)
        zoomLy = Math.abs(e) < 5 ? 0 : Math.signum(e) * Math.max(0.35f, Math.min(1f, Math.abs(e) / 40f));
    }

    /** Jump anywhere's / the moon jump's input while it is on (MainActivity; the classic style has no state poll) */
    private int jumpInputs() { return MainActivity.instance != null ? MainActivity.instance.jumpInputsInUse() : 0; }

    void setContext(boolean on) {
        if (on == context) return;
        context = on;
        layoutControls(getWidth(), getHeight());  // the shoulder buttons' shape
        removeCallbacks(pollState);
        if (on) post(pollState);
        if (on) GameText.prepare(getContext(), this::invalidate);  // the actions' names in the game's words
        invalidate();
    }

    // not in the pause menu: it uses every button (X / Y / R put items on them)
    private boolean contextActive() { return context && flexible && !editing && state[0] != 0 && state[9] == 0; }

    /** whether the context buttons show this control now (always true without them) */
    private boolean shown(Control c) {
        if (editing) return true;  // the editor: all of the style's controls
        // singing while Link conducts (the mic button), or conducting by the note pads: only + and B (the game's Back) stay
        if (singing() || conductPads()) {
            if (c.kind == KIND_DPAD) return c.dpadBits != 0;
            if (c.kind != KIND_BUTTON) return pointers.indexOfValue(c) >= 0;
            if (c.pressed || c.bit == Native.PLUS) return true;
            // A while the game's message box shows (after a song: the Ballad of Gales on land, a scene's text)
            if (c.bit == Native.A) return msgShown;
            return c.bit == Native.B && (!contextActive() || state[6] != 0 && !dimmed(18));
        }
        // a cutscene without text: nothing to press (the game's own A does nothing there)
        if (quietScene && !editing && (c.kind == KIND_BUTTON || c.kind == KIND_DPAD) && !c.pressed)
            return c.bit == Native.ZR && fastForward();  // but fast forward, while it is on
        // a talk or a scene's text: A goes on (B does the same in the game's message box, d_msg.cpp; it showed the sword),
        // ZR fast forwards while that is on; the items, the d-pad, + and the jumps do nothing there. A tap anywhere is A
        // there (talkTap), so A itself goes too
        if (talking() && (c.kind == KIND_BUTTON || c.kind == KIND_DPAD) && !c.pressed)
            return c.bit == Native.A && !talkTap() || c.bit == Native.ZR && fastForward();
        // a choice, number entry or a shop's item choice (all inside a talk): A takes it, B leaves the shop. Not in a choice
        // (any, not only a shop's): a tap on a line answers (choice_step), no A, no B (B's cancel didn't take in Zunari's yes / no). The chosen item up close and the shop's
        // later lines up to its goodbye (all minigame mode 12): no A, a tap on the item is A, elsewhere B (turns the page
        // as A does). The
        // game reads no item, d-pad, jump or + there (+ showed the GamePad picture, empty in a talk). Selling to Beedle
        // (spoils, the Complimentary ID, the Fill-Up Coupon) is X / Y / R facing him before the talk: normal play
        if (shopTalk() && (c.kind == KIND_BUTTON || c.kind == KIND_DPAD) && !c.pressed)
            return c.bit == Native.A && minigame != MINIGAME_CHOICE && minigame != MINIGAME_SHOP && minigame != MINIGAME_SHOP_HELD || c.bit == Native.B && minigame != MINIGAME_CHOICE && minigame != MINIGAME_SHOP_HELD
                    || c.bit == Native.ZR && fastForward();
        // a minigame played by touch: only B (the game's Cancel / Quit; not the sword) and + stay
        if (minigame != 0 && minigame < MINIGAME_CHOICE && contextActive()) {
            if (c.kind == KIND_DPAD) return c.dpadBits != 0;
            if (c.kind == KIND_BUTTON && !c.pressed && c.bit != Native.PLUS)
                return c.bit == Native.B && state[6] != 0 && state[6] != 0x3E && state[6] != 0x35 && !dimmed(18);
        }
        // gliding with the Deku Leaf by tap: only its button (closes / waves it), B (the sword) and ZL stay
        if (tapMode() && leafFlying()) {
            if (c.kind == KIND_DPAD) return c.dpadBits != 0;
            if (c.kind == KIND_BUTTON && !c.pressed)
                return c.bit == Native.B || c.bit == Native.ZL || slotOf(c) >= 0 && state[3 + slotOf(c)] == DEKU_LEAF;
        }
        if (aimOnly() && c.kind == KIND_DPAD) return c.dpadBits != 0;
        if (aimOnly() && c.kind == KIND_BUTTON && !c.pressed && !aimButton(c)) {
            if (c.bit == Native.ZR) return state[7] != 0 && !dimmed(19);
            return c.bit == Native.B || c.bit == Native.ZL;
        }
        // Jump anywhere's and the moon jump's own buttons: while it is on; the context buttons not in events, the Picto
        // Box or the telescope
        if (c.kind == KIND_BUTTON && (c.bit & (Native.JUMP | Native.MOON)) != 0 && !c.pressed)
            return (c.bit & jumpInputs()) != 0 && (!contextActive() || state[8] == 0 && !pictoMode() && !telescope()
                    && !(swimming() && c.bit == Native.JUMP) && !climbing());  // Jump anywhere doesn't jump out of water (mods/jump.cpp); the moon jump does
        if (c.kind == KIND_DPAD) return dpadMask() != 0 || c.dpadBits != 0;
        // the left stick only zooms in the Picto Box / telescope: dragging the zoom bar does that
        if (c.kind == KIND_STICK && c == controls.get(5) && pinchMode() && pointers.indexOfValue(c) < 0) return false;
        if (!contextActive() || c.kind != KIND_BUTTON || c.pressed) return true;
        // item buttons by tap: B puts the item in Link's hand away, so A's Return / Put away go
        if (tapMode() && itemInHand() && c.bit == Native.B) return true;
        if (tapMode() && itemInHand() && c.bit == Native.A && (aAction() == 0x07 || aAction() == 0x08)) return false;
        if (tapMode() && stowsItself() && c.bit == Native.A && aAction() == 0x08) return false;  // goes away by itself
        if (tapMode() && c.bit == Native.A && leafFlying()) return false;  // the leaf's button closes it
        if (c.bit == Native.A) return aAction() != 0 || state[8] != 0;  // in events A always continues; 0x44 (album) does nothing
        if (c.bit == Native.B && (swimming() || climbing())) return false;  // no sword in water, on a ladder (the game still shows it there)
        if (c.bit == Native.B) return state[6] != 0 && state[6] != 0x3E && !dimmed(18);  // 0x3E: the game hides B
        if (c.bit == Native.X) return itemShown(0);
        if (c.bit == Native.Y) return itemShown(1);
        if (c.bit == Native.R) return itemShown(2);
        if (c.bit == Native.ZR) return state[7] != 0 && !dimmed(19) || fastForward();
        // the game doesn't read L in play (checked 2026-10-08; the pause menu shows it); Jump anywhere and the moon jump
        // have their own buttons (above)
        if (c.bit == Native.L) return false;
        if (c.bit == Native.ZL && pictoMode()) return false;  // no targeting in the Picto Box and its album
        if ((c.bit == Native.STICK_R || c.bit == Native.PLUS) && pictoMode()) return false;  // no first person, no menu there
        // no menu while Link aims an item (bow, boomerang, hookshot, grappling hook) or looks in first person
        if (c.bit == Native.PLUS && (state[12] & (ST0_AIMS | ST0_FIRST_PERSON)) != 0) return false;
        if ((c.bit == Native.ZL || c.bit == Native.STICK_R || c.bit == Native.PLUS) && telescope()) return false;  // no targeting, no first person, no menu
        if (c.bit == Native.STICK_R && (swimming() || climbing())) return false;  // no first person in water, on a ladder
        if (c.bit == Native.STICK_L) return l3InUse;  // our faster running / swimming (the game itself: not seen to use L3)
        return true;
    }

    // Link's mode (mPlayerStatus, mods/touch_state.cpp; names from the GameCube decompilation)
    private static final int ST0_HANG = 0x100, ST0_SHIP = 0x10000, ST0_ROPE = 0x800000, ST0_SWIM = 0x100000,
            ST0_TELESCOPE = 0x200000, ST0_LADDER = 0x4000000, ST0_HANG2 = 0x2000000, ST0_CRAWL = 0x08000000;
    private static final int ST0_FIRST_PERSON = 0x2000;
    private static final int ST0_BOW_AIM = 0x1000;  // aiming the bow in first person (ZL: third person)
    private static final int ST1_CONDUCT = 0x1, ST1_PICTO = 0x8, ST1_LEAF_FLY = 0x20, ST1_LEAF_FAN = 0x40;
    private static final int[] SHIP_ITEMS = {0x78, 0x77, 0x20, 0x22, 0x25, 0x2D, 0x21, 0x23, 0x26, 0x2A, 0x2C, 0x27, 0x35, 0x36,
            0x31, 0x2F, 0x50, 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58, 0x59};

    /** X / Y / R (0..2): a command the game puts there (album: Delete), or an item the game allows now;
     *  the game's own button (hidden for us, still kept up to date) also has to be there */
    private boolean itemShown(int i) {
        if (command(i)) return true;
        // the Picto Box's / telescope's own button in their view: B (the game's Back) already closes it
        if (pictoMode() || telescope()) return false;
        return usable(state[3 + i]) && state[15 + i] != 0 && !dimmed(15 + i);  // -1: not seen yet
    }

    /** the game shows none of its own buttons (X / Y / R / B / ZR all at alpha 0; -1: not seen yet) */
    private boolean clusterHidden() {
        boolean seen = false;
        for (int i = 15; i < 20; i++) {
            if (state[i] > 0) return false;
            seen |= state[i] == 0;
        }
        return seen;
    }

    /** the game dims its own button (state[15..19], alpha 60 while swimming): not usable now */
    private boolean dimmed(int i) { return state[i] >= 0 && state[i] < 128; }

    /** the game shows a command text on X / Y / R (0..2) instead of the item */
    private boolean command(int i) { return state[20 + i] > 0 && state[15 + i] != 0; }

    /** ZR fast forwards this event (Options: fast forward of cutscenes / dialogues, mods/turbo.cpp) */
    private boolean fastForward() {
        return contextActive() && state[8] != 0 && MainActivity.instance != null && MainActivity.instance.ffInUse(state[8]);
    }

    /** Link swims (the game dims the items itself; A only shows for an action it offers) */
    private boolean swimming() { return context && (state[12] & ST0_SWIM) != 0; }

    /** Link is on a ladder, climbs ivy or hangs from a ledge (touch state 36, daPy_lk_c::mModeFlg: LADDER, CLIMB, HANG; HANG
     *  but in the boat: getting ready to sail): no sword, first person, Wind Waker or jumps there (the game still shows them) */
    private boolean climbing() {
        if (!context || state.length <= 36) return false;
        int mode = state[36];
        return (mode & (MODE_LADDER | MODE_CLIMB | MODE_HANG)) != 0 && (mode & MODE_IN_SHIP) == 0;
    }
    private static final int MODE_HANG = 0x20, MODE_IN_SHIP = 0x2000, MODE_CLIMB = 0x10000, MODE_LADDER = 0x400000;

    /** looking through the telescope: only its zoom, R (puts it away) and B are of use */
    private boolean telescope() { return context && (state[12] & ST0_TELESCOPE) != 0; }

    /** the Picto Box (aiming, or its album: ZR 0x49 / 0x48 switches between them) */
    private boolean pictoMode() {
        return context && ((state[13] & ST1_PICTO) != 0 || state[7] == 0x48 || state[7] == 0x49 || albumScreen);
    }

    /** Link carries something (A throws it or ZR drops it; with a Bokoblin's club B shows the club) */
    private boolean carrying() {
        return context && state[6] != 0x37 && (state[1] == 0x0E || state[7] == 0x09);
    }

    /** B's action: carrying something (A throws it), B throws it too (it breaks; ZR sets it down whole)
     *  although the game says sword: Throw */
    private int bAction() {
        if (minigame == MINIGAME_WIND) return 0x07;  // the wind compass: B goes back (the game's bytes there jump around)
        if (context && state[1] == 0x0E && state[6] == 0x35) return 0x0E;
        return state[6];
    }

    /** A's action: aiming the Picto Box, A takes the photo (the game's byte is mostly empty there; its
     *  own Picto Box screen shows A anyway); 0x44 (album) does nothing: none */
    private int aAction() {
        if (minigame == MINIGAME_WIND) return 0x17;  // the wind compass: A takes the direction (the game's A byte is empty)
        if (context && state[7] == 0x49 && state[1] == 0) return 0x20;
        if (context && state[8] != 0 && state[1] == 0) return 0x19;  // talks, events: A continues (Next; the game's byte is empty)
        return state[1] == 0x44 ? 0 : state[1];
    }

    /** a yes / no question of the Picto Box (keep the photo? delete it?): A chooses, B says no */
    private boolean pictoChoice() {
        return context && ((state[13] & ST1_PICTO) != 0 || album() || state[7] == 0x49) && state[1] == 0x17 && state[6] == 0x07 && !chooser();
    }

    /** aiming the Picto Box (ZR opens the album) */
    private boolean pictoAim() { return context && state[7] == 0x49; }

    /** the Picto Box's album (ZR goes back to the Picto Box), or the album as the chooser of a picto shown to someone */
    private boolean album() { return context && (state[7] == 0x48 || albumScreen); }

    /** the album as the game's picto chooser (the Picto Box shown to Carlov, Lenzo, a postbox): A chooses, B goes back;
     *  the game's touch state has no sign of it (no ZR action, no event), its album layout drawn tells it (pollState) */
    private boolean chooser() { return context && albumScreen && state[7] != 0x48; }
    private boolean albumScreen;

    /** whether the game lets Link use this item (X / Y / R) now: the HUD's rules (dMeter_xyAlpha in the
     *  decompilation; the game dims the item buttons it doesn't allow) */
    private boolean usable(int item) {
        if (item == 0xFF) return false;
        int st0 = state[12], st1 = state[13];
        if ((st1 & ST1_PICTO) != 0) return item == 0x23 || item == 0x26;  // aiming the Picto Box
        if ((st0 & ST0_TELESCOPE) != 0) return item == 0x20;
        if ((st0 & ST0_ROPE) != 0) return item == 0x25;                  // on the grappling hook's rope
        if ((st0 & (ST0_SWIM | ST0_HANG | ST0_HANG2 | ST0_CRAWL | ST0_LADDER)) != 0) return false;
        if (state[14] == 1) return item == 0x78 || item == 0x77 || item == 0x20;  // minigame 1: sail and telescope only
        if ((st0 & ST0_SHIP) != 0) {
            for (int i : SHIP_ITEMS)
                if (i == item) return true;
            return false;
        }
        return true;
    }

    /** the d-pad directions the context buttons offer (all without them): up conducts with the Wind
     *  Waker; down does nothing in play (checked 2026-10-08); left / right the boat's cannon and salvage
     *  arm. In the Flexible style with the item ring and no controller connected these are in the ring,
     *  so the d-pad goes in play (up stays only while no X / Y / R shows to open the ring); the Classic
     *  style and a connected controller keep it */
    private int dpadMask() {
        int all = Native.UP | Native.DOWN | Native.LEFT | Native.RIGHT;
        if (!contextActive()) return dpadInRing() && !editing && state[0] != 0 && state[9] == 0 ? 0 : all;
        if (album() && (state[1] != 0x17 || chooser())) return pictoBig ? 0 : Native.LEFT | Native.RIGHT;  // the album: browse the photos (large view: by swiping)
        if (pictoMode() || telescope() || swimming() || climbing()) return 0;  // no Wind Waker in water, on a ladder (the game still shows it there)
        if (clusterHidden()) return 0;  // the game's buttons are faded out (fading in after a door, events)
        // up: the Wind Waker
        boolean ringOpens = dpadInRing() && (itemShown(0) || itemShown(1) || itemShown(2));
        return state[10] != 0 && !ringOpens ? Native.UP : 0;
    }

    /** the game's icon the context buttons show on this control, or null (then its label) */
    private android.graphics.Bitmap iconOf(Control c) {
        // Jump anywhere's and the moon jump's own buttons (every style): Link jumping (GameIcons.linkPoses)
        if (c.kind == KIND_BUTTON && (c.bit == Native.JUMP || c.bit == Native.MOON) && GameIcons.linkPoses)
            return GameIcons.get(getContext(), c.bit == Native.JUMP ? GameIcons.LINK_JUMP : "#moon:" + GameIcons.LINK_JUMP,
                    () -> post(this::invalidate));
        if (!contextActive() || c.kind != KIND_BUTTON) return null;
        String t = null;
        if (c.bit == Native.B && pictoChoice()) t = "#cross";  // keep the photo / delete it? B: no
        // a choice / number entry: A chooses, B cancels (number entry); the shop's items: B leaves.
        // (the game's bytes there: Next on A, the sword on B)
        else if (c.bit == Native.A && shopTalk()) t = actionIcon(minigame == MINIGAME_SHOP_HELD ? 0x19 : 0x17);  // (held up: its price, Next)
        else if (c.bit == Native.B && shopTalk())  // (the shop's items: Link walking out of its door)
            t = minigame != MINIGAME_SHOP ? "#cross" : GameIcons.linkPoses ? "#exit:" + GameIcons.LINK_WALKAWAY + "*" : actionIcon(0x07);
        else if (c.bit == Native.B && minigame == MINIGAME_CONDUCT) t = "#putaway:" + GameIcons.itemTexture(0x22);  // ends conducting: the baton down
        else if (c.bit == Native.B && tapMode() && (itemInHand() || viewStows())) t = actionIcon(0x08);  // item buttons by tap: Put away
        else if (c == aimBtn && tapMode() && slotOf(c) >= 0) t = "#shoot:" + GameIcons.itemTexture(state[3 + slotOf(c)]);
        // item buttons by tap, aiming: ZL shows the view a tap switches to: Link from behind with the item (third
        // person); latched the crosshair (first person, a pictogram)
        else if (c.bit == Native.ZL && tapMode() && aiming()) t = c.pressed ? null : thirdPerson();
        else if (c.bit == Native.B) t = actionIcon(bAction());
        else if (c.bit == Native.A) t = actionIcon(aAction());
        else if (c.bit == Native.ZR && fastForward()) t = "#ff";
        else if (c.bit == Native.ZR && zrLatched && zrCrouch && GameIcons.linkPoses) t = GameIcons.LINK_RISE + "*";  // crouched: stand up
        else if (c.bit == Native.ZR) t = actionIcon(state[7]);
        // aiming the bow: ZL held (latched) is the third person (the target); else it shows Link, to switch there
        // (the mode bit is the same in both)
        else if (c.bit == Native.ZL && (state[12] & ST0_BOW_AIM) != 0 && !c.pressed) t = thirdPerson();
        else if (c.bit == Native.ZL) t = c.pressed ? "#target:1" : "#target:0";  // targeting: arrows closing in, latched closer
        else if (c.bit == Native.STICK_L) t = "LStick_00^t";
        // R3 (the game's first person look) shows the view a press switches to: the Picto Box's finder, in first person Link
        else if (c.bit == Native.STICK_R)
            t = (state[12] & ST0_FIRST_PERSON) != 0 ? (GameIcons.linkPoses ? GameIcons.LINK_BACK : "MapLinkFace_00^l") : "#finder";
        else if (c.bit == Native.PLUS) t = "MapItemIcon_00^l";
        else if (c.bit == Native.X) t = itemIcon(0);
        else if (c.bit == Native.Y) t = itemIcon(1);
        else if (c.bit == Native.R) t = itemIcon(2);
        if (GameIcons.LINK_ROLL.equals(t)) t = GameIcons.LINK_ROLL + rollFrame();
        else if (t != null && GameIcons.pictures(t) > 0) t = t.replace("*", Integer.toString(loopFrame(t, GameIcons.pictures(t))));  // its picture now
        return t == null ? null : GameIcons.get(getContext(), t, () -> post(this::invalidate));
    }

    /** the third person view's icon: Link from behind holding the item he aims (touch state 29; the bow when not known),
     *  GameIcons.linkPoses; else his face */
    private String thirdPerson() {
        if (!GameIcons.linkPoses) return "MapLinkFace_00^l";
        String item = state.length > 29 && state[29] >= 0 && state[29] < 0xFF ? GameIcons.itemTexture(state[29]) : null;
        if (item == null && (state[12] & ST0_BOW_AIM) != 0) item = GameIcons.itemTexture(0x27);
        return item == null ? GameIcons.LINK_BACK : "#third:" + item;
    }

    // the roll's pictures play the first time Roll shows up, then again when it shows up ROLL_AGAIN_MS after
    // that, ROLL_FRAME_MS each; else its resting picture (GameIcons.LINK_ROLL_REST: Link diving in). A redraw is
    // asked for while it plays only
    private static final long ROLL_FRAME_MS = 60, ROLL_AGAIN_MS = 10 * 60 * 1000;
    private final Runnable rollTick = () -> { tickAt = Long.MAX_VALUE; invalidate(); };
    private long tickAt = Long.MAX_VALUE;

    /** a redraw ms from now, unless one comes sooner (the animated icons share it) */
    private void tickIn(long ms) {
        long now = android.os.SystemClock.uptimeMillis(), at = now + ms;
        if (at >= tickAt && tickAt > now) return;  // (one that is overdue was lost: post anew)
        removeCallbacks(rollTick);
        tickAt = at;
        postDelayed(rollTick, ms);
    }
    private static long rollStart = -1;  // (static: across the views while the app runs; uptime, the phone's sleep not counted)
    private boolean rollDrawn, rollWasShown;  // this draw / the one before showed the roll (onDraw)

    // Link's animated icons but the roll (GameIcons.pictures: carrying, sidling, opening, pushing, picking up): the
    // animation plays from the start when its icon shows up, keeps its last picture LOOP_PAUSE_MS, then plays again
    private static final long LOOP_PAUSE_MS = 2000;
    private final java.util.Map<String, Long> loopStart = new java.util.HashMap<>();
    private final java.util.Set<String> loopDrawn = new java.util.HashSet<>(), loopWasShown = new java.util.HashSet<>();

    private int loopFrame(String anim, int frames) {
        long now = android.os.SystemClock.uptimeMillis();
        if (!loopWasShown.contains(anim) && loopDrawn.add(anim)) loopStart.put(anim, now);  // (A, B: one start)
        loopDrawn.add(anim);
        long play = ROLL_FRAME_MS * frames, t = (now - loopStart.get(anim)) % (play + LOOP_PAUSE_MS);
        if (t >= play) {  // the pause: the next redraw when it plays again
            tickIn(play + LOOP_PAUSE_MS - t);
            return frames - 1;
        }
        tickIn(ROLL_FRAME_MS - t % ROLL_FRAME_MS);
        return (int) (t / ROLL_FRAME_MS);
    }

    private int rollFrame() {
        long now = android.os.SystemClock.uptimeMillis();
        if ((rollStart < 0 || now - rollStart >= ROLL_AGAIN_MS) && !rollWasShown && !rollDrawn) rollStart = now;
        rollDrawn = true;
        long t = now - rollStart;
        if (t >= ROLL_FRAME_MS * GameIcons.LINK_ROLL_FRAMES) return GameIcons.LINK_ROLL_REST;
        tickIn(ROLL_FRAME_MS - t % ROLL_FRAME_MS);
        return (int) (t / ROLL_FRAME_MS);
    }

    /** an action's icon; put away with an item in Link's hand (touch state 29: daPy_lk_c::mEquipItem, the item's number;
     *  the sword 0x103 and others above 0xFF have none): that item and the down arrow, not the sword. The Wind Waker
     *  (0x22) points down like the sword; other items stay upright */
    private String actionIcon(int code) {
        // carrying a pot, barrel, ...: Link throwing it (A, B) / setting it down whole (ZR) (not a Bokoblin's club:
        // B shows it, 0x37)
        if (GameIcons.linkPoses && (code == 0x0E || code == 0x09) && carrying())
            return "#carry:" + (code == 0x0E ? GameIcons.LINK_THROW + "*:135" : GameIcons.LINK_DROP + "*:0");
        // look / check at a postbox (postbox.cpp): the postbox swallowing a letter (its own model and animation)
        if ((code == 0x01 || code == 0x0A || code == 0x02) && postboxNear) return GameIcons.POSTBOX + "*";
        // Open: Link opening a door or a chest (the game's attention entry tells which; touch state 34)
        if (GameIcons.linkPoses && code == 0x0B && state.length > 34 && (state[34] == 5 || state[34] == 6))
            return (state[34] == 5 ? GameIcons.LINK_DOOR : GameIcons.LINK_CHEST) + "*";
        // Jump with the sword in hand, locked on, stick idle or forward: the jump attack (touch state 35), Link leaping
        // with the sword (not the side hop's / back flip's jump)
        if (GameIcons.linkPoses && code == 0x12 && state.length > 35 && state[35] != 0) return GameIcons.LINK_JATTACK + "*";
        // the boat (touch state 37: the Swift Sail or the sail): Set sail (0x14, the decompilation's はる; HD's text
        // "Set sail", also 0x2C Cruise), the mast rising with the sail; Put away while sailing (no item in Link's hand,
        // the sail's 0x77 / 0x78 aside), it going down; Stop (the sail down, the boat still moving): it drifting, an arrow
        // into a bar; the Swift Sail's Fast / Normal: sailing, two arrows / one
        boolean swift = state.length > 37 && state[37] != 0;
        // Get in / Get out: Link climbing into the boat from its side / jumping out of it
        if (GameIcons.linkPoses && (code == 0x1C || code == 0x1D)) return (code == 0x1C ? GameIcons.LINK_GETIN : GameIcons.LINK_GETOUT) + "*";
        if (code == 0x13 && (state[12] & ST0_SHIP) != 0) return "#stop:" + GameIcons.DRIFTING + "*";
        if (code == 0x14 || code == 0x2C) return (swift ? GameIcons.SAIL_UP_SWIFT : GameIcons.SAIL_UP) + "*";
        if (code == 0x08 && (state[12] & ST0_SHIP) != 0 && (state[29] < 0 || state[29] >= 0xFF || state[29] == 0x77 || state[29] == 0x78))
            return (swift ? GameIcons.SAIL_DOWN_SWIFT : GameIcons.SAIL_DOWN) + "*";
        if (code == 0x41 || code == 0x42) return "#sailing:" + GameIcons.SAILING + "*:" + (code == 0x41 ? 2 : 1);
        // Let go on a ladder, on a wall of vines, hanging from a ledge (touch state 36: Link's mode flags): Link letting go
        // and falling
        if (GameIcons.linkPoses && code == 0x06 && climbing()) {
            int mode = state[36];
            return ((mode & MODE_LADDER) != 0 ? GameIcons.LINK_LADDER : (mode & MODE_CLIMB) != 0 ? GameIcons.LINK_IVY : GameIcons.LINK_HANG) + "*";
        }
        if (code == 0x08 && state.length > 29 && state[29] == 0x22) return "#putaway:" + GameIcons.itemTexture(0x22);
        if (code == 0x08 && state.length > 29 && state[29] >= 0 && state[29] < 0xFF) {
            String item = GameIcons.itemTexture(state[29]);
            if (item != null) return "#stow:" + item;
        }
        return GameIcons.actionIcon(code, state[2], state[11]);
    }

    /** X / Y / R (0..2): the item's icon, or the command's (the album's Delete; others: none yet, then text) */
    private String itemIcon(int i) {
        if (command(i) && album()) return "#delete";
        if (command(i) && pictoAim()) {  // turns the camera around (selfie): the Picto Box in a turning arrow
            String box = GameIcons.itemTexture(state[3 + i] == 0x23 || state[3 + i] == 0x26 ? state[3 + i] : 0x26);
            return "#turn:" + box;
        }
        return GameIcons.itemTexture(state[3 + i]);  // other commands (Picto Box aim: X leaves it): the item
    }

    private final android.graphics.Rect iconSrc = new android.graphics.Rect();
    private final Paint iconPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);

    /** the pictogram the context buttons draw on this control (Pictograms.java), or null */
    private String pictogramOf(Control c) {
        if (c.kind == KIND_BUTTON && c.bit == Native.JUMP) return "jump";  // (in every style: no Wii U letter to show)
        if (c.kind == KIND_BUTTON && c.bit == Native.MOON) return "moonjump";
        if (!contextActive() || c.kind != KIND_BUTTON) return null;
        if (c.bit == Native.ZL) return "target";
        if (c.bit == Native.STICK_R) return "eye";
        if (c.bit == Native.PLUS) return "menu";
        return null;
    }

    /** the label the context buttons give this control */
    private String labelOf(Control c) {
        if (!contextActive() || c.kind != KIND_BUTTON) return c.label;
        String s = null;
        if (c.bit == Native.A) s = GameText.action(aAction());
        else if (c.bit == Native.B && tapMode() && (itemInHand() || viewStows())) s = GameText.action(0x08);
        else if (c == aimBtn && tapMode()) s = getContext().getString(state[3 + Math.max(0, slotOf(c))] == 0x2D ? R.string.touch_throw : R.string.touch_shoot);
        else if (c.bit == Native.B) s = state[6] != 0 ? GameText.action(bAction()) : null;
        else if (c.bit == Native.X) s = command(0) && album() ? GameText.action(0x45) : command(0) && pictoAim() ? getContext().getString(R.string.touch_selfie) : GameText.item(state[3]);
        else if (c.bit == Native.Y) s = command(1) && album() ? GameText.action(0x45) : GameText.item(state[4]);
        else if (c.bit == Native.R) s = command(2) && album() ? GameText.action(0x45) : GameText.item(state[5]);
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

    // a touch on the title screen holds A (in the opening: + , the game's skip) while the finger is
    // down, at least TITLE_A_MS (a few frames)
    private static final long TITLE_A_MS = 120;
    private int titlePointer = -1, titleBit;
    private long titleAUntil;

    /** a short press of `bit` (a button outside these controls, e.g. the file select's Back) */
    void pulse(int bit) {
        titleBit = bit;
        titleAUntil = SystemClock.uptimeMillis() + TITLE_A_MS;
        listener.onControlsChanged();
        postDelayed(listener::onControlsChanged, TITLE_A_MS + 10);
    }

    int buttons() {
        int b = 0;
        if (editing) return 0;
        for (Control c : controls) {
            if (c.kind == KIND_BUTTON && c.pressed) b |= c.bit;
            if (c.kind == KIND_DPAD) b |= c.dpadBits;
        }
        if (titlePointer >= 0 || SystemClock.uptimeMillis() < titleAUntil) b |= titleBit;
        return b | albumBits;
    }
    float stickX(int i) { return i == 0 && putAwaySince >= 0 ? 0 : controls.get(5 + i).sx; }
    float stickY(int i) { return i == 0 && putAwaySince >= 0 ? 0 : i == 0 && zooming() ? zoomLy : controls.get(5 + i).sy; }

    @Override
    protected void onSizeChanged(int w, int h, int ow, int oh) {
        layoutControls(w, h);
    }

    // positions in units of u = shorter side / 7 (landscape)
    private void layoutControls(int W, int H) {
        if (W == 0 || H == 0) return;
        float u = Math.min(W, H) / 7f * scale;
        float m = u * 0.25f;  // edge margin
        for (Control c : controls) {
            c.round = c.round0;
            c.corner = 0.5f;
        }
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
        set(11, W * 0.5f, H - m - u * 0.35f, u * 0.6f, u * 0.6f);             // +
        set(12, colL + u * 1.55f, H - m - u * 0.35f, u * 0.55f, u * 0.55f);   // L3
        set(13, colR - u * 1.55f, H - m - u * 0.35f, u * 0.55f, u * 0.55f);   // R3
        set(14, m + u * 0.3f, m + u * 0.4f, u * 0.6f, u * 0.6f);              // menu (left of ZL)
        set(15, W * 0.5f, m + u * 0.35f, u * 0.8f, u * 0.55f);                // screen button
        // Jump anywhere and the moon jump: the context buttons put Jump where L is (L does nothing in play there) and the
        // moon jump beside it; the classic ones beside ZL and L
        if (context && flexible) {
            set(16, colL, m + u * 1.25f, u * 1.6f, u * 0.65f);
            set(17, colL + u * 1.75f, m + u * 1.25f, u * 1.6f, u * 0.65f);
        } else {
            set(16, colL + u * 1.75f, m + u * 0.4f, u * 1.6f, u * 0.65f);
            set(17, colL + u * 1.75f, m + u * 1.25f, u * 1.6f, u * 0.65f);
        }
        // context buttons: the shoulder buttons (and the jumps) are round, the size of the face buttons
        for (int i : new int[] {0, 1, 2, 3, 16, 17}) {
            Control s = controls.get(i);
            s.round = context && flexible;
            if (s.round) s.w = s.h = u * 0.72f;
        }
        // the player's own places, shapes and sizes (layout editor); a shape keeps about the area of the style's button
        for (Control c : controls) {
            if (c.kind == KIND_BUTTON && c.shape != SHAPE_DEFAULT) {
                float s = (float) Math.sqrt(c.w * c.h);
                c.round = c.shape == SHAPE_ROUND;
                c.corner = c.shape == SHAPE_OVAL ? 0.5f : 0.22f;
                switch (c.shape) {
                    case SHAPE_ROUND: c.w = c.h = s; break;
                    case SHAPE_OVAL: c.w = s * 1.56f; c.h = s * 0.64f; break;
                    case SHAPE_SQUARE: c.w = c.h = s * 0.9f; break;
                    case SHAPE_HEX: c.w = s * 1.07f; c.h = s * 0.93f; break;
                    case SHAPE_DIAMOND: c.w = c.h = s * 1.25f; break;
                    default: c.w = s * 1.3f; c.h = s * 0.78f;
                }
            }
            c.w *= c.size;
            c.h *= c.size;
            if (!Float.isNaN(c.px) && !(flexible && c.kind == KIND_STICK)) {
                c.cx = c.px * W;
                c.cy = c.py * H;
            }
        }
        // the Picto Box with the large view: fixed places as in a phone's camera app (the player's own
        // places don't apply). Landscape: the shutter (A) big at the right edge, the selfie / delete (X)
        // above it, the album / back to the camera (ZR) below it; portrait: the shutter at the bottom
        // centre, the album left of it, the selfie right of it. Back / no (B) opposite the shutter.
        cameraLaid = cameraLayout();
        if (cameraLaid) {
            float small = u * 0.72f, big = u * 1.15f;
            if (W >= H) {
                float x = W - m - u * 0.85f, y = H * 0.5f;
                set(8, x, y, big, big);
                set(7, x, y - u * 1.6f, small, small);
                set(2, x, y + u * 1.6f, small, small);
                set(9, m + u * 0.6f, y, small, small);
            } else {
                float x = W * 0.5f, y = H - m - u * 1.6f;
                set(8, x, y, big, big);
                set(7, x + u * 1.9f, y, small, small);
                set(2, x - u * 1.9f, y, small, small);
                set(9, x, m + u * 0.6f, small, small);
            }
        }
        // the note pads (conducting): B (puts the baton away) moves up to the face buttons' middle (X / Y / A are
        // hidden then), clear of the pad's up sign
        padsLaid = conductPads();
        if (padsLaid) {
            Control b = controls.get(9);
            b.cx = colR;
            b.cy = H * 0.5f - u * 0.1f - u * 0.25f;
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
        c.hx = Float.NaN;
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
        if (len > 1) {
            dx /= len;
            dy /= len;
            if (c == controls.get(5) && stickFollows) {
                // the left stick follows a thumb that slides out of its range: the thumb stays on the
                // edge, so a slighter push only needs a short slide back (not to the first touch point)
                if (Float.isNaN(c.hx)) { c.hx = c.cx; c.hy = c.cy; }
                c.cx = x - dx * c.w;
                c.cy = y + dy * c.w;
            }
        }
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
        if (!Float.isNaN(c.hx)) {  // the pulled-along left stick goes back to its place
            c.cx = c.hx;
            c.cy = c.hy;
            c.hx = Float.NaN;
        }
    }

    private void releaseAll() {
        zrLatched = false;
        aimBtn = tapCandidate = null;
        reaim = false;
        putAwaySince = -1;
        leafPointer = -1;
        removeCallbacks(leafWave);
        for (Control c : controls) release(c);
        pointers.clear();
        titlePointer = -1;
        if (talkPointer >= 0) Native.setOption("talk_hold", 0);
        talkPointer = -1;
        swipePointer = -1;
        zoomPointer = pinchFirst = albumPointer = -1;
        endPinch();
        viewPinchEnd();
        freeFinger = -1;
        if (minigamePointer >= 0) Native.minigameTouch(2, Float.NaN, Float.NaN);  // (cancelled: no target)
        if (minigamePointer2 >= 0) Native.minigameTouch(12, Float.NaN, Float.NaN);
        minigamePointer = minigamePinch = minigamePointer2 = -1;
        notePointer = timePointer = -1;
        padNote = 0;
        if (drcPointer >= 0) Native.setTouch(false, 0, 0);
        drcPointer = -1;
        if (pagePointer >= 0 && pageDragging) listener.onPageDragEnd(0, 0);
        if (pagePointer >= 0 && pageFlipping) listener.onGamePageDragEnd(0, 0);
        pagePointer = -1;
    }

    /** pixels of this view per layout unit of the game's GamePad screen (1280 across its 16:9 screen) */
    float layoutUnitPx() {
        pictureRect(pic);
        return pic.width() * (pictureU(1) - pictureU(0)) / 1280f;
    }

    /** a rectangle in the game's layout units (the 16:9 screen: x -640..640, y -360..360 downwards) on this view */
    RectF layoutToView(float l, float t, float r, float b) {
        pictureRect(pic);
        return new RectF(pic.left + pictureU(0.5f + l / 1280f) * pic.width(), pic.top + pictureV(0.5f + t / 720f) * pic.height(),
                pic.left + pictureU(0.5f + r / 1280f) * pic.width(), pic.top + pictureV(0.5f + b / 720f) * pic.height());
    }

    /** pixels across the GamePad picture in the game's layout units (1280 across its 16:9 screen) */
    private float layoutUnits(float px) {
        pictureRect(pic);
        float w = pic.width() * (pictureU(1) - pictureU(0));
        return w > 0 ? px * 1280f / w : 0;
    }

    /** a point of this view on the game's 16:9 GamePad screen (0..1) */
    private float[] gameXY(float x, float y) {
        pictureRect(pic);
        float tx = (x - pic.left) / pic.width(), ty = (y - pic.top) / pic.height();
        float pa = Native.drcPictureAspect(), kx = pa > 16f / 9f ? pa / (16f / 9f) : 1f, ky = pa > 0.1f && pa < 16f / 9f ? (16f / 9f) / pa : 1f;
        return new float[] {(tx - 0.5f) * kx + 0.5f, (ty - 0.5f) * ky + 0.5f};
    }

    private boolean inDrc(float x, float y) { return !drcRect.isEmpty() && drcRect.contains(x, y); }

    // the picture sits in drcRect in its own shape (16:9, or the screen's while the game draws it so,
    // as on a display of its own: DrcDisplay); aspect.cpp maps it to the game's 16:9 touch screen
    private void pictureRect(RectF out) {
        float pa = Native.drcPictureAspect();
        if (!(pa > 0.1f)) pa = 16f / 9f;
        out.set(drcRect);
        float pw = drcRect.width(), ph = drcRect.height();
        if (pw / ph > pa) out.inset((pw - ph * pa) / 2, 0);
        else out.inset(0, (ph - pw / pa) / 2);
    }

    private final RectF pic = new RectF();

    /** where a finger last touched the GamePad picture, on the game's 16:9 screen (0..1; MainActivity: which chart square it opened) */
    static volatile float fingerX = -1, fingerY = -1;
    /** the finger on the GamePad picture is down, and where it went down (as fingerX / fingerY; MainActivity.slotHold) */
    static volatile boolean fingerDown;
    static volatile float fingerStartX = -1, fingerStartY = -1;

    private void touchDrc(boolean down, float x, float y) {
        pictureRect(pic);
        float tx = (x - pic.left) / pic.width(), ty = (y - pic.top) / pic.height();
        if (down) {
            float pa = Native.drcPictureAspect(), kx = pa > 16f / 9f ? pa / (16f / 9f) : 1f, ky = pa > 0.1f && pa < 16f / 9f ? (16f / 9f) / pa : 1f;
            fingerX = (tx - 0.5f) * kx + 0.5f;  // (pictureU / pictureV backwards)
            fingerY = (ty - 0.5f) * ky + 0.5f;
            if (!fingerDown) {
                fingerStartX = fingerX;
                fingerStartY = fingerY;
            }
        }
        fingerDown = down;
        Native.setDrcTouch(down, Math.max(0, Math.min(1, tx)), Math.max(0, Math.min(1, ty)));
    }

    /** the picture's u, v (0..1) of a point on the game's 16:9 GamePad screen (the picture may be wider: Fill) */
    static float pictureU(float x) {
        float pa = Native.drcPictureAspect(), k = pa > 16f / 9f ? pa / (16f / 9f) : 1f;
        return (x - 0.5f) / k + 0.5f;
    }

    static float pictureV(float y) {
        float pa = Native.drcPictureAspect(), k = pa > 0.1f && pa < 16f / 9f ? (16f / 9f) / pa : 1f;
        return (y - 0.5f) / k + 0.5f;
    }

    // ---- the controller's highlight on the GamePad picture: a touch target of the game (MainActivity)
    private boolean focusOn, focusPressed;
    private final RectF focusGame = new RectF(), focusRect = new RectF();

    /** x, y (centre), w, h on the game's 16:9 GamePad screen (0..1); on = false hides it */
    void setFocus(boolean on, float x, float y, float w, float h, boolean pressed) {
        if (on == focusOn && pressed == focusPressed && focusGame.left == x - w / 2 && focusGame.top == y - h / 2
                && focusGame.right == x + w / 2 && focusGame.bottom == y + h / 2) return;
        focusOn = on;
        focusPressed = pressed;
        focusGame.set(x - w / 2, y - h / 2, x + w / 2, y + h / 2);
        invalidate();
    }

    private boolean drawGameCursor(Canvas canvas, RectF r, float unit) {
        if (!GameUi.MenuCursor.draw(getContext(), canvas, r, 40 * unit, () -> post(this::invalidate))) return false;
        postInvalidateDelayed(250);
        return true;
    }

    // ---- a Back button (the game's Back icon) on the GamePad picture's Options / Save while no controller is
    // connected: their A / B hints are hidden (MainActivity.setMenuTouch) and they have no Back of their own; a tap = B
    private boolean menuBack;
    private final Paint menuBackPaint = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final RectF menuBackRect = new RectF();
    private static final float MENU_BACK_X = 0.957f, MENU_BACK_Y = 0.85f, MENU_BACK_H = 0.12f;  // on the 16:9 screen, right of the description box

    void setMenuBack(boolean on) {
        if (on == menuBack) return;
        menuBack = on;
        invalidate();
    }

    private boolean placeMenuBack() {
        if (!menuBack || !drcMode || drcRect.isEmpty()) return false;
        pictureRect(pic);
        float h = MENU_BACK_H * (pictureV(1) - pictureV(0)) * pic.height(), cx = pic.left + pictureU(MENU_BACK_X) * pic.width(), cy = pic.top + pictureV(MENU_BACK_Y) * pic.height();
        menuBackRect.set(cx - h / 2, cy - h / 2, cx + h / 2, cy + h / 2);
        return true;
    }

    private void drawMenuBack(Canvas canvas) {
        if (!placeMenuBack()) return;
        android.graphics.Bitmap bm = GameIcons.get(getContext(), "Back_00^t", () -> post(this::invalidate));
        if (bm == null) return;
        float k = Math.min(menuBackRect.width() / bm.getWidth(), menuBackRect.height() / bm.getHeight());
        float w = bm.getWidth() * k, h = bm.getHeight() * k;
        tmp.set(menuBackRect.centerX() - w / 2, menuBackRect.centerY() - h / 2, menuBackRect.centerX() + w / 2, menuBackRect.centerY() + h / 2);
        canvas.drawBitmap(bm, null, tmp, menuBackPaint);
    }

    // ---- the options page's arrow on the GamePad inventory's items page, where the game's own left arrow
    // (L_MenuArrowL_00, shown on the page right of it) would be: a tap opens the options (MainActivity.onOptionsPage)
    private boolean optionsArrow;
    private final RectF optionsArrowGame = new RectF(), optionsArrowRect = new RectF();
    private GameUi.MenuArrow arrowIcon;

    /** x, y (centre), w, h on the game's 16:9 GamePad screen (0..1); on = false hides it */
    void setOptionsArrow(boolean on, float x, float y, float w, float h) {
        if (on == optionsArrow && optionsArrowGame.left == x - w / 2 && optionsArrowGame.top == y - h / 2
                && optionsArrowGame.right == x + w / 2 && optionsArrowGame.bottom == y + h / 2) return;
        optionsArrow = on;
        optionsArrowGame.set(x - w / 2, y - h / 2, x + w / 2, y + h / 2);
        invalidate();
    }

    private float optionsArrowAlpha = 1;

    void setOptionsArrowAlpha(float a) {
        if (Math.abs(a - optionsArrowAlpha) < 0.01f) return;
        optionsArrowAlpha = a;
        invalidate();
    }

    private boolean placeOptionsArrow() {
        if (!optionsArrow || !drcMode || drcRect.isEmpty()) return false;
        pictureRect(pic);
        optionsArrowRect.set(pic.left + pictureU(optionsArrowGame.left) * pic.width(), pic.top + pictureV(optionsArrowGame.top) * pic.height(),
                pic.left + pictureU(optionsArrowGame.right) * pic.width(), pic.top + pictureV(optionsArrowGame.bottom) * pic.height());
        return true;
    }

    private void drawOptionsArrow(Canvas canvas) {
        if (!placeOptionsArrow()) return;
        if (arrowIcon == null) {
            arrowIcon = new GameUi.MenuArrow(getContext(), true);
            arrowIcon.setCallback(this);
        }
        arrowIcon.setAlpha(Math.round(255 * optionsArrowAlpha));
        arrowIcon.setBounds(Math.round(optionsArrowRect.left), Math.round(optionsArrowRect.top), Math.round(optionsArrowRect.right), Math.round(optionsArrowRect.bottom));
        arrowIcon.draw(canvas);
    }

    // ---- play buttons on the game's song list (its GamePad screen while Link conducts, shown by +): one on each
    // learned song's title line, outside the names (left column: left of them, right column: right of them; each
    // column's buttons in one line, by its longest name), a tap
    // plays that song (MainActivity.onSongListPlay). The rows (W_SignBase_00..05 of the baton layout, N_BatonSignA..F:
    // Requiem, Ballad, Command, Passing, Earth, Wind) sit at fixed places, their titles (T_BatonSign_*) centred 100 above
    private int songListLearned;  // the learned songs' bits while the list can show, 0 none
    private static final float[][] SONG_ROWS = {{-315, 171}, {315, 171}, {-315, -49}, {315, -49}, {-315, -269}, {315, -269}};  // layout units, y up
    static final int[] SONG_ROW_MELODY = {0, 1, 2, 5, 3, 4};
    private static final float SONG_TITLE_UP = 100, SONG_TITLE_K = 0.8f, SONG_TITLE_MAX = 566;  // the title's layout units per GameFont unit, its widest (longer names squeezed; measured)
    private static final float SONG_PLAY_R = 30, SONG_PLAY_GAP = 18;
    private final RectF songPlayRect = new RectF();
    private final android.graphics.Path playPath = new android.graphics.Path();

    void setSongList(int learned) {
        if (learned == songListLearned) return;
        songListLearned = learned;
        invalidate();
    }

    private boolean placeSongPlay(int row) {
        if (songListLearned == 0 || !drcMode || drcRect.isEmpty() || editing) return false;
        int melody = SONG_ROW_MELODY[row];
        if ((songListLearned & (1 << melody)) == 0) return false;
        float side = SONG_ROWS[row][0] < 0 ? -1 : 1, y = -(SONG_ROWS[row][1] + SONG_TITLE_UP);
        float half = 0;  // the column's longest name: its buttons in one line
        boolean font = GameFont.ready(getContext(), this::invalidate);
        for (int r = 0; r < SONG_ROWS.length; r++) {
            if (SONG_ROWS[r][0] * side < 0) continue;
            String name = GameText.songName(SONG_ROW_MELODY[r]);
            half = Math.max(half, font && GameFont.covers(name) ? Math.min(GameFont.width(name) * SONG_TITLE_K, SONG_TITLE_MAX) / 2 : SONG_TITLE_MAX / 2);
        }
        float x = SONG_ROWS[row][0] + side * (half + SONG_PLAY_GAP + SONG_PLAY_R);
        songPlayRect.set(layoutToView(x - SONG_PLAY_R, y - SONG_PLAY_R, x + SONG_PLAY_R, y + SONG_PLAY_R));
        float dx = Math.max(0, -songPlayRect.left) - Math.max(0, songPlayRect.right - getWidth());  // (a long name: kept on the screen)
        songPlayRect.offset(dx, 0);
        return true;
    }

    // ---- a story scene waiting for one song (learning it, a sage's): a play button for it, a tap plays it
    // (MainActivity.onSongListPlay). On the message window's box (the game's notes to copy show in it) at its top right,
    // sized like the game's own icons there; without the box at the TV picture's bottom, right of the + button
    private int songWanted = -1;
    private final RectF songWantedRect = new RectF();
    private static final float WANTED_R = 26, WANTED_IN = 40;  // layout units: radius, the centre's inset from the corner

    void setSongWanted(int melody) {
        if (melody == songWanted) return;
        songWanted = melody;
        invalidate();
    }

    private boolean placeSongWanted() {
        if (songWanted < 0 || drcMode || editing) return false;
        RectF tv = tvRect.isEmpty() ? new RectF(0, 0, getWidth(), getHeight()) : tvRect;
        int[] w = Native.msgWindow();
        float cx, cy, r;
        if (w != null && w.length == 5 && w[0] != 0) {  // layout units: 720 across the picture's height, +y up
            float unit = tv.height() / 720f;
            r = WANTED_R * unit;
            cx = tv.centerX() + (w[1] + w[3] - WANTED_IN) * unit;
            cy = tv.centerY() - (w[2] + w[4] - WANTED_IN) * unit;
        } else {
            float d = getResources().getDisplayMetrics().density;
            r = 22 * d;
            cx = tv.centerX() + 70 * d + r;
            cy = tv.bottom - 28 * d - r;
        }
        songWantedRect.set(cx - r, cy - r, cx + r, cy + r);
        return true;
    }

    private void drawSongWanted(Canvas canvas) {
        if (!placeSongWanted()) return;
        postInvalidateDelayed(100);  // (the box comes, goes and moves)
        float r = songWantedRect.height() / 2, cx = songWantedRect.centerX(), cy = songWantedRect.centerY();
        android.graphics.Bitmap base = GameIcons.get(getContext(), GameIcons.BASE_ROUND, () -> post(this::invalidate));
        if (base != null) {
            float bs = r * 2 * 1.14f;
            baseSrc.set(0, 0, base.getWidth(), base.getHeight());
            tmp.set(cx - bs / 2, cy - bs / 2, cx + bs / 2, cy + bs / 2);
            iconPaint.setAlpha(255);
            canvas.drawBitmap(base, baseSrc, tmp, iconPaint);
        } else {
            songBg.setColor(0xE0FFFFFF);
            canvas.drawCircle(cx, cy, r, songBg);
        }
        float t = r * 0.42f;
        playPath.reset();
        playPath.moveTo(cx - t * 0.7f, cy - t);
        playPath.lineTo(cx + t * 1.0f, cy);
        playPath.lineTo(cx - t * 0.7f, cy + t);
        playPath.close();
        songPaint.setColor(GAME_INK);
        canvas.drawPath(playPath, songPaint);
    }

    private int songPlayAt(float x, float y) {
        for (int i = 0; i < SONG_ROWS.length; i++) {
            if (!placeSongPlay(i)) continue;
            float cx = songPlayRect.centerX(), cy = songPlayRect.centerY(), r = songPlayRect.width() * 0.7f;  // (a little larger to hit)
            if ((x - cx) * (x - cx) + (y - cy) * (y - cy) <= r * r) return i;
        }
        return -1;
    }

    private void drawSongPlay(Canvas canvas) {
        for (int i = 0; i < SONG_ROWS.length; i++) {
            if (!placeSongPlay(i)) continue;
            float r = songPlayRect.width() / 2, cx = songPlayRect.centerX(), cy = songPlayRect.centerY();
            android.graphics.Bitmap base = GameIcons.get(getContext(), GameIcons.BASE_ROUND, () -> post(this::invalidate));
            if (base != null) {
                float bs = r * 2 * 1.14f;
                baseSrc.set(0, 0, base.getWidth(), base.getHeight());
                tmp.set(cx - bs / 2, cy - bs / 2, cx + bs / 2, cy + bs / 2);
                iconPaint.setAlpha(255);
                canvas.drawBitmap(base, baseSrc, tmp, iconPaint);
            } else {
                songBg.setColor(0xE0FFFFFF);
                canvas.drawCircle(cx, cy, r, songBg);
            }
            float t = r * 0.42f;  // a play triangle, its weight a little right of the centre
            playPath.reset();
            playPath.moveTo(cx - t * 0.7f, cy - t);
            playPath.lineTo(cx + t * 1.0f, cy);
            playPath.lineTo(cx - t * 0.7f, cy + t);
            playPath.close();
            songPaint.setColor(GAME_INK);
            canvas.drawPath(playPath, songPaint);
        }
    }

    @Override
    protected boolean verifyDrawable(android.graphics.drawable.Drawable who) { return who == arrowIcon || super.verifyDrawable(who); }

    private void drawFocus(Canvas canvas) {
        if (!focusOn || !drcMode || drcRect.isEmpty()) return;
        pictureRect(pic);
        focusRect.set(pic.left + pictureU(focusGame.left) * pic.width(), pic.top + pictureV(focusGame.top) * pic.height(),
                pic.left + pictureU(focusGame.right) * pic.width(), pic.top + pictureV(focusGame.bottom) * pic.height());
        // layout units (1280x720 on the 16:9 screen) in pixels
        float unit = pic.height() * (pictureV(1) - pictureV(0)) / 720f;
        if (focusPressed) focusRect.inset(focusRect.width() * 0.04f, focusRect.height() * 0.04f);  // a touch: pressed in a little
        if (drawGameCursor(canvas, focusRect, unit)) return;
        float d = getResources().getDisplayMetrics().density;
        focusRect.inset(-3 * d, -3 * d);
        float sw = stroke.getStrokeWidth();
        int sc = stroke.getColor();
        stroke.setStrokeWidth(5 * d);
        stroke.setColor(0x90000000);
        canvas.drawRoundRect(focusRect, 8 * d, 8 * d, stroke);
        stroke.setStrokeWidth(3 * d);
        stroke.setColor(focusPressed ? 0xFFFFFFFF : 0xFFFFD21F);
        canvas.drawRoundRect(focusRect, 8 * d, 8 * d, stroke);
        stroke.setStrokeWidth(sw);
        stroke.setColor(sc);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (editing) return editTouch(e);
        if (hint.showing()) return hint.onTouch(e, tvRect);
        int action = e.getActionMasked();
        int idx = e.getActionIndex();
        if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) && pageBarTouch(e.getX(idx), e.getY(idx)))
            return true;
        // the mic button: held while singing; its finger is nothing else
        if (micPointer >= 0) {
            boolean up = action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_CANCEL
                    || action == MotionEvent.ACTION_POINTER_UP && e.getPointerId(idx) == micPointer;
            if (up) {
                micPointer = -1;
                listener.onSongMic(false);
                if (action != MotionEvent.ACTION_CANCEL) return true;
            }
        }
        if ((action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) && placeMic()) {
            if (songPanel.hitsClose(e.getX(idx), e.getY(idx))) {  // practice: the close mark
                listener.onSongPracticeEnd();
                invalidate();
                return true;
            }
            float mx = e.getX(idx) - micRect.centerX(), my = e.getY(idx) - micRect.centerY(), mr = micRect.width() * 0.6f;
            if (micPointer < 0 && mx * mx + my * my <= mr * mr) {
                micPointer = e.getPointerId(idx);
                listener.onSongMic(true);
                return true;
            }
            float bx = e.getX(idx) - beatRect.centerX(), by = e.getY(idx) - beatRect.centerY(), br = beatRect.width() * 0.65f;
            if (bx * bx + by * by <= br * br) {  // the time switch: a tap (also while the mic button is held)
                listener.onSongBeat();
                invalidate();
                return true;
            }
        }
        // the note pads: their fingers are nothing else
        if (action == MotionEvent.ACTION_DOWN || action == MotionEvent.ACTION_POINTER_DOWN) {
            if (padDown(e.getPointerId(idx), e.getX(idx), e.getY(idx))) return true;
        } else if (action == MotionEvent.ACTION_MOVE) {
            for (int i = 0; i < e.getPointerCount(); i++) padMove(e.getPointerId(i), e.getX(i), e.getY(i));
        } else if (action == MotionEvent.ACTION_UP || action == MotionEvent.ACTION_POINTER_UP) {
            if (padUp(e.getPointerId(idx))) return true;
        }
        boolean changed = false;
        boolean menuWasHidden = touched();
        switch (action) {
            case MotionEvent.ACTION_DOWN:
            case MotionEvent.ACTION_POINTER_DOWN: {
                int id = e.getPointerId(idx);
                float x = e.getX(idx), y = e.getY(idx);
                fingerAt.put(id, new float[] {x, y});
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
                }
                if (placeMenuBack() && menuBackRect.contains(x, y)) {
                    pulse(Native.B);
                    return true;
                }
                if (placeOptionsArrow() && optionsArrowRect.contains(x, y)) {
                    listener.onOptionsPage();
                    return true;
                }
                if (placeSongWanted() && Math.hypot(x - songWantedRect.centerX(), y - songWantedRect.centerY()) <= songWantedRect.width() * 0.75f) {
                    listener.onSongListPlay(songWanted);
                    return true;
                }
                int songRow = songPlayAt(x, y);
                if (songRow >= 0) {
                    listener.onSongListPlay(SONG_ROW_MELODY[songRow]);
                    return true;
                }
                // the title screen: a touch anywhere (the menu and screen buttons aside) is A, to the file select;
                // the opening: + (the game's skip: the first shows the prompt, the second skips)
                if (titlePointer < 0 && (front == Native.FRONT_TITLE || front == Native.FRONT_OPENING)) {
                    titlePointer = id;
                    titleBit = front == Native.FRONT_TITLE ? Native.A : Native.PLUS;
                    titleAUntil = SystemClock.uptimeMillis() + TITLE_A_MS;
                    postDelayed(listener::onControlsChanged, TITLE_A_MS + 10);  // a quick tap lets go of A then
                    changed = true;
                    break;
                }
                if (c == null && titlePointer < 0 && talkPointer < 0 && controlsVisible && talkTap() && talkHold) {
                    // held: a tap of A, then the game side goes on each time the text waits (setTalkHold)
                    talkPointer = id;
                    Native.setOption("talk_hold", 1);
                    pulse(Native.A);
                    changed = true;
                    break;
                }
                if (c == null && titlePointer < 0 && controlsVisible && talkTap()) {  // a talk's text: a touch anywhere goes on (A)
                    titlePointer = id;
                    titleBit = Native.A;
                    titleAUntil = SystemClock.uptimeMillis() + TITLE_A_MS;
                    postDelayed(listener::onControlsChanged, TITLE_A_MS + 10);
                    changed = true;
                    break;
                }
                if (c == null && viewPinchStart(id, x, y)) break;
                if (c != null && tapMode() && c.kind == KIND_BUTTON) {  // item buttons by tap (above)
                    if (c == aimBtn) {  // Shoot
                        if (c.pressed) {
                            c.pressed = false;
                            shootAt = SystemClock.uptimeMillis();
                        }
                        changed = true;
                        break;
                    }
                    if (c.bit == Native.B && itemInHand()) {  // Put away
                        if (putAwaySince < 0) startPutAway();
                        changed = true;
                        break;
                    }
                    // gliding with the Deku Leaf: a tap on its button closes the leaf (the game's A, Cancel), held
                    // LEAF_WAVE_MS it is the game's own press (a wave in the air: leafWave); falling, a press opens it
                    // again (the game's own)
                    if (slotOf(c) >= 0 && state[3 + slotOf(c)] == DEKU_LEAF && leafFlying() && leafPointer < 0) {
                        leafPointer = id;
                        leafBtn = c;
                        removeCallbacks(leafWave);
                        postDelayed(leafWave, LEAF_WAVE_MS);
                        changed = true;
                        break;
                    }
                    if (c.bit == Native.ZL && aimBtn != null && c.pressed && aiming()) reaim = true;  // to first person
                    int slot = slotOf(c);
                    if (slot >= 0) {
                        if (aimBtn != null) dropAim();  // another item
                        if (aimItem(state[3 + slot]) && !command(slot)) {
                            tapCandidate = c;
                            tapDownAt = SystemClock.uptimeMillis();
                        }
                    }
                }
                if (c != null) {
                    // ZR latches for crouching (context buttons: its action is Crouch) and while ZL (targeting)
                    // is latched (the shield stays up); otherwise (fast forward held in cutscenes) it stays a
                    // normal button. A latched ZR is released by a tap.
                    boolean zlLatched = controls.get(0).pressed;
                    // not in the Picto Box and its album: there ZR switches between them (a tap)
                    boolean crouchLatch = c.bit == Native.ZR && !pictoMode() && (zrLatched || zlLatched || (contextActive() && state[7] == 0x0F));
                    if (c.latch || crouchLatch) {  // switches on the tap; lifting the finger changes nothing
                        c.pressed = !c.pressed;
                        if (c.bit == Native.ZL) relockGen++;
                        if (c.bit == Native.ZR) {
                            zrCrouch = c.pressed && state.length > 7 && state[7] == 0x0F;
                            zrLatched = c.pressed;
                        }
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
                    if (c.bit == Native.B && stowSince >= 0) stowSince = SystemClock.uptimeMillis();  // a swing: the sword stays SWORD_STOW_MS more
                    changed = true;
                } else if (controlsVisible && !drcMode && albumSwipe()) {
                    if (albumPointer < 0) {
                        albumPointer = id;
                        albumX = x;
                    }
                } else if (controlsVisible && !drcMode && pinchMode()) {
                    if (pinchA < 0 && pinchFirst >= 0 && pinchFirst != id) {  // a second free finger: pinch
                        pinchA = pinchFirst;
                        pinchB = id;
                        if (swipePointer == pinchFirst) swipePointer = -1;  // the first finger stops looking around
                        Control look = pointers.get(pinchFirst);
                        if (look != null) {
                            release(look);
                            pointers.remove(pinchFirst);
                        }
                        pinchDist0 = pinchDistance(e);
                        pinchHandle0 = state[24];
                        zoomTarget = pinchHandle0;
                        removeCallbacks(pollState);
                        post(pollState);
                    } else if (pinchFirst < 0) {
                        pinchFirst = id;
                        Control look = controls.get(6);
                        if (x < getWidth() / 2f || !flexible) {
                            // no left stick here (it only zooms)
                        } else if (swipeCamera) {  // the camera as usual
                            if (swipePointer < 0) {
                                swipePointer = id;
                                swipeX = x;
                                swipeY = y;
                            }
                        } else if (pointers.indexOfValue(look) < 0) {
                            look.cx = x;
                            look.cy = y;
                            look.sx = look.sy = 0;
                            pointers.put(id, look);
                        }
                    }
                    changed = true;
                } else if (zoomPointer < 0 && controlsVisible && !drcMode && inZoomBar(x, y)) {
                    zoomPointer = id;
                    zoomTo(y);
                    removeCallbacks(pollState);
                    post(pollState);
                    changed = true;
                } else if (drcPointer < 0 && pagePointer < 0 && inDrc(x, y) && drcMode
                        && listener.pageSwipeFrom(gameXY(x, y)[0], gameXY(x, y)[1])) {
                    pagePointer = id;
                    pageDragging = pageFlipping = false;
                    pageX0 = pageLastX = x;
                    pageY0 = y;
                    pageV = 0;
                    pageLastT = e.getEventTime();
                } else if (drcPointer < 0 && inDrc(x, y)) {
                    drcPointer = id;
                    touchDrc(true, x, y);
                } else if (minigamePointer >= 0 && minigamePointer2 < 0 && minigame == MINIGAME_CONDUCT && minigameTouchable(x, y)) {
                    // conducting: a second finger (one holds the beat, the other swings the baton)
                    minigamePointer2 = id;
                    minigameTouch(10, x, y);
                } else if (minigamePointer >= 0 && minigamePinch < 0 && minigame == MINIGAME_FIGURE && minigameTouchable(x, y)) {
                    // a second finger while looking at a figure: pinching zooms
                    minigamePinch = id;
                    float[] f = fingerAt.get(minigamePointer);
                    minigamePinch0 = f == null ? 0 : (float) Math.hypot(x - f[0], y - f[1]);
                } else if (minigamePointer < 0 && front == 0 && minigameTouchable(x, y)) {
                    minigamePointer = id;
                    minigameTouch(0, x, y);
                } else if (flexible && controlsVisible && !drcMode && front == 0) {
                    // a floating stick where the thumb landed (one per side), or the swipe camera
                    boolean left = x < getWidth() / 2f;
                    Control s = controls.get(left ? 5 : 6);
                    if (!left && swipeCamera) {
                        if (swipePointer < 0) {
                            swipePointer = id;
                            swipeX = x;
                            swipeY = y;
                        }
                    } else if (pointers.indexOfValue(s) < 0 && shown(s)) {
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
                    float[] at = fingerAt.get(id);
                    if (at != null) {
                        at[0] = x;
                        at[1] = y;
                    }
                    if (id == perfDragPointer) {
                        perfDrag(x, y);
                        continue;
                    }
                    if (id == drcPointer) {
                        touchDrc(true, x, y);
                        continue;
                    }
                    if (id == zoomPointer) {
                        zoomTo(y);
                        changed = true;
                        continue;
                    }
                    if (id == pinchA || id == pinchB) {
                        pinchTo(pinchDistance(e));
                        changed = true;
                        continue;
                    }
                    if (id == viewPinchA || id == viewPinchB) {
                        viewPinchMove();
                        continue;
                    }
                    if (id == albumPointer) {
                        albumMove(x);
                        continue;
                    }
                    if (id == pagePointer) {
                        float dx = x - pageX0, dy = y - pageY0;
                        if (pageFlipping) {  // the game's pages follow the finger
                            long t = e.getEventTime();
                            if (t > pageLastT) pageV = pageV * 0.5f + 0.5f * (x - pageLastX) * 1000f / (t - pageLastT);
                            pageLastX = x;
                            pageLastT = t;
                            listener.onGamePageDrag(layoutUnits(dx));
                            continue;
                        }
                        if (!pageDragging) {
                            if (Math.hypot(dx, dy) < android.view.ViewConfiguration.get(getContext()).getScaledTouchSlop()) continue;
                            if (dx > 0 && dx > 1.5f * Math.abs(dy) && optionsArrow) {
                                pageDragging = true;
                            } else if (Math.abs(dx) > 1.5f * Math.abs(dy)) {
                                pageFlipping = true;
                                pageLastX = x;
                                pageLastT = e.getEventTime();
                                listener.onGamePageDrag(layoutUnits(dx));
                                continue;
                            } else {  // not ours: the game gets the touch (where it began, then here)
                                pagePointer = -1;
                                drcPointer = id;
                                touchDrc(true, pageX0, pageY0);
                                postDelayed(() -> { if (drcPointer == id) touchDrc(true, x, y); }, 40);
                                continue;
                            }
                        }
                        long t = e.getEventTime();
                        if (t > pageLastT) pageV = pageV * 0.5f + 0.5f * (x - pageLastX) * 1000f / (t - pageLastT);
                        pageLastX = x;
                        pageLastT = t;
                        listener.onPageDrag(layoutUnits(dx));
                        continue;
                    }
                    if (id == minigamePinch || (id == minigamePointer && minigamePinch >= 0)) {
                        float[] f0 = fingerAt.get(minigamePointer), f1 = fingerAt.get(minigamePinch);
                        if (f0 != null && f1 != null && minigamePinch0 > 1)
                            Native.minigameTouch(3, (float) Math.hypot(f1[0] - f0[0], f1[1] - f0[1]) / minigamePinch0, 0);
                        continue;
                    }
                    if (id == minigamePointer) {
                        minigameTouch(1, x, y);
                        continue;
                    }
                    if (id == minigamePointer2) {
                        minigameTouch(11, x, y);
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
                        // sliding between buttons (e.g. across the face buttons) moves the press; not
                        // while the button holds the item ring open (the finger picks there)
                        Control now = controlAt(x, y);
                        if (ring != null && ring.holds(c.bit)) now = null;
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
                if (id == pagePointer) {
                    pagePointer = -1;
                    if (pageDragging) {
                        listener.onPageDragEnd(layoutUnits(e.getX(idx) - pageX0), layoutUnits(pageV));
                    } else if (pageFlipping) {
                        listener.onGamePageDragEnd(layoutUnits(e.getX(idx) - pageX0), layoutUnits(pageV));
                    } else {  // a tap: to the game after all (held long enough for it to see)
                        float tx = pageX0, ty = pageY0;
                        touchDrc(true, tx, ty);
                        postDelayed(() -> touchDrc(false, tx, ty), 80);
                    }
                }
                if (minigamePinch >= 0 && (id == minigamePinch || id == minigamePointer)) {  // the pinch ends both
                    Native.minigameTouch(4, 0, 0);
                    minigamePinch = minigamePointer = -1;
                }
                if (id == minigamePointer) {
                    minigameTouch(2, e.getX(idx), e.getY(idx));
                    minigamePointer = -1;
                }
                if (id == minigamePointer2) {
                    minigameTouch(12, e.getX(idx), e.getY(idx));
                    minigamePointer2 = -1;
                }
                if (id == swipePointer) swipePointer = -1;
                if (id == zoomPointer) {
                    zoomPointer = -1;
                    zoomLy = 0;
                    changed = true;
                }
                if (id == pinchA || id == pinchB) {  // the other finger may pinch again with a new one
                    pinchFirst = id == pinchA ? pinchB : pinchA;
                    endPinch();
                    changed = true;
                } else if (id == pinchFirst) {
                    pinchFirst = -1;
                }
                if (id == albumPointer) albumPointer = -1;
                if (id == viewPinchA || id == viewPinchB) viewPinchEnd();  // (the other finger does nothing more)
                if (id == freeFinger) freeFinger = -1;
                if (id == titlePointer) {
                    titlePointer = -1;
                    changed = true;
                }
                if (id == talkPointer) {
                    talkPointer = -1;
                    Native.setOption("talk_hold", 0);
                }
                if (id == leafPointer) {  // the Deku Leaf's button while gliding (above)
                    leafPointer = -1;
                    removeCallbacks(leafWave);
                    if (leafBtn.pressed) leafBtn.pressed = false;  // waved
                    else if (leafFlying()) pulse(Native.A);       // a tap: closed
                    changed = true;
                }
                Control c = pointers.get(id);
                if (c != null) {
                    // a tap on an aiming item's button (the ring not opened): held, Link aims (item buttons by tap)
                    if (c == tapCandidate && tapMode() && SystemClock.uptimeMillis() - tapDownAt < TAP_MAX_MS
                            && (ring == null || !ring.holds(c.bit))) latchAim(c);
                    else release(c);
                    if (c == tapCandidate) tapCandidate = null;
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

    // the item ring (ItemRing): drawn over everything; the finger holding its button picks
    private ItemRing ring;
    private final android.util.SparseArray<float[]> fingerAt = new android.util.SparseArray<>();

    void setItemRing(ItemRing r) { ring = r; }

    // a controller is connected (MainActivity polls it): its d-pad is the game's, so ours stays
    private boolean padConnected;

    private String[] slotNames = {"X", "Y", "R"};

    /** item button i's name (X, Y, R order), as on the HUD */
    String slotName(int i) { return slotNames[i]; }

    /** the item buttons' names (X, Y, R order; MainActivity.applySlotLabels) on their buttons where no item shows */
    void setSlotLabels(String[] l) {
        slotNames = l.clone();
        int[] bits = {Native.X, Native.Y, Native.R};
        for (Control c : controls)
            for (int k = 0; k < 3; k++)
                if (c.kind == KIND_BUTTON && c.bit == bits[k]) c.label = l[k];
        invalidate();
    }

    void setPadConnected(boolean on) {
        if (on == padConnected) return;
        padConnected = on;
        invalidate();
    }

    /** the d-pad's uses are in the item ring (see dpadMask) */
    boolean dpadInRing() { return flexible && !padConnected && ring != null && ring.enabled(); }

    /** where the finger holding this button is now, or null */
    float[] fingerOn(int bit) {
        for (int k = 0; k < pointers.size(); k++) {
            Control c = pointers.valueAt(k);
            if (c.kind == KIND_BUTTON && c.bit == bit) return fingerAt.get(pointers.keyAt(k));
        }
        return null;
    }

    @Override
    public void draw(Canvas canvas) {
        super.draw(canvas);
        hint.draw(canvas, tvRect);
        if (ring != null) ring.draw(canvas);
    }

    @Override
    protected void onDraw(Canvas canvas) {
        rollWasShown = rollDrawn;
        rollDrawn = false;
        loopWasShown.clear();
        loopWasShown.addAll(loopDrawn);
        loopDrawn.clear();
        drawClimbHud(canvas);
        drawMinigameMarks(canvas);
        drawPerfHud(canvas);
        if (editing && grid) drawGrid(canvas);
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
            drawControl(canvas, c, a0);
        }
        if (editing) drawEditor(canvas);
        else drawFocus(canvas);
        drawMenuBack(canvas);
        drawOptionsArrow(canvas);
        drawSongPlay(canvas);
        drawSongWanted(canvas);
        drawSongs(canvas);
        drawPageBar(canvas);
        drawPostNotice(canvas);
        drawPads(canvas);
        drawMic(canvas);
        updateSongView(placeMic() && songConducting);
    }

    // ---- songs sung into the microphone (SongListener): a mark while it listens (a note per note heard so far),
    // and the song recognised, at the top of the TV picture
    private boolean songListening;
    private int songNotes;
    private String banner;
    private long bannerUntil;
    private final Paint songPaint = new Paint(Paint.ANTI_ALIAS_FLAG), songBg = new Paint(Paint.ANTI_ALIAS_FLAG);
    private static final long BANNER_MS = 2500;

    void setSongListening(boolean on, int notes, boolean button, boolean held, boolean conducting) {
        songListening = on;
        songNotes = notes;
        micShown = button;
        micHeld = held;
        songConducting = conducting;
        if (!button && micPointer >= 0) micPointer = -1;
        invalidate();
    }

    /** a touch hint (GameHint: the songs' 20 / 21) the first time it can show */
    // the item ring's hint (once): Link in play for 1.5 s with an item (a new game: the telescope; a loaded save or
    // state with items: at once), the ring on; the inventory read at most once a second
    private long ringPlaySince = -1, ringCheckedAt;
    private boolean ringHintDone;

    /** MainActivity's poll (every style and controller) */
    void ringHint(int[] s) {
        if (ringHintDone || editing || ring == null || !ring.enabled() || !GameHint.enabled()) return;
        boolean play = s[0] != 0 && s[8] == 0 && s[9] == 0;
        long now = SystemClock.uptimeMillis();
        if (!play) { ringPlaySince = -1; return; }
        if (ringPlaySince < 0) ringPlaySince = now;
        if (now - ringPlaySince < 1500 || now - ringCheckedAt < 1000) return;
        ringCheckedAt = now;
        int[] r = Native.itemRing();
        if (r[0] == -1) return;
        for (int k = 0; k < 21; k++)
            if (r[k] != 0xFF) {
                if (hint.showing()) return;  // (another hint first)
                offerHint(HINT_RING);
                ringHintDone = true;  // (shown now, or seen before)
                return;
            }
    }

    void offerHint(int m) {
        if (!editing && hint.offer(m)) releaseAll();
    }

    void askHint(int m, String text) {
        if (editing || !hint.ask(m, text)) return;
        releaseAll();
        if (micPointer >= 0) {  // (the question takes the touches: the mic button would never see its finger go)
            micPointer = -1;
            listener.onSongMic(false);
        }
    }

    // the mic button: held while singing, at the bottom left of the TV picture (under the left thumb: the movement
    // stick has nothing to do while Link conducts), the time switch above it; the take's timeline (SongPanel) under
    // the game's own time bar and note row
    private boolean micShown, micHeld, songConducting;
    private boolean msgShown;  // the game's message box, while Link conducts or in an event (pollState): A shows

    /** an event that has shown text (a talk, a scene's lines; the gaps between them too), not a choice, number entry,
     *  shop or song (minigame); also a talk's start before its box opens (event mode 1, as the quiet scene's grace) and
     *  TALK_HOLD_MS after one (the event mode drops for a moment between two parts of a talk) */
    private boolean talking() {
        if (!contextActive() || minigame != 0 || singing() || conductPads()) return false;
        return talkNow() || SystemClock.uptimeMillis() - talkSeenAt < TALK_HOLD_MS;
    }
    private boolean talkNow() {
        return contextActive() && state[8] != 0 && (sceneText || state[8] == 1 && !quietScene) && minigame == 0 && !singing() && !conductPads();
    }
    // ---- a new action on A in play (Open, Lift, Speak, ...): A pops up and two rings spread from it (aFlash) for
    // A_FLASH_MS, once when it changes to it; not the roll (A while running) or Put away (after each swing), not when the same action showed
    // A_FLASH_AGAIN_MS before (running past a door flips Roll / Open). Only in play: not in events, talks, minigames,
    // the Picto Box or the telescope, and not right after one (a talk's Next turning back into Speak)
    private static final long A_FLASH_MS = 900, A_FLASH_RING_MS = 600, A_FLASH_AGAIN_MS = 2500;
    private int aSeen;
    private boolean aPlayWas;
    private long aFlashAt = -A_FLASH_MS;
    private final android.util.SparseLongArray aFlashedAt = new android.util.SparseLongArray();

    /** pollState: A's action now */
    private void noteNewAction() {
        boolean play = contextActive() && state[8] == 0 && minigame == 0 && !talking() && !pictoMode() && !album()
                && !telescope() && !quietScene && controlsVisible;
        Control a = null;
        for (Control c : controls)
            if (c.kind == KIND_BUTTON && c.bit == Native.A) a = c;
        int code = a != null && play && !a.hidden && shown(a) ? aAction() : 0;
        long now = SystemClock.uptimeMillis();
        if (code != aSeen && code != 0 && code != 0x0C && code != 0x08 && play && aPlayWas
                && now - aFlashedAt.get(code, -A_FLASH_AGAIN_MS) >= A_FLASH_AGAIN_MS) {
            aFlashAt = now;
            android.util.Log.i("wwhd", "touch A: new action 0x" + Integer.toHexString(code));
            invalidate();
        }
        if (code != 0) aFlashedAt.put(code, now);  // (while it shows: its next showing counts from its end)
        aSeen = code;
        aPlayWas = play;
    }

    /** 0..1 through A's highlight, or -1 */
    private float aFlash() {
        long t = SystemClock.uptimeMillis() - aFlashAt;
        if (t < 0 || t >= A_FLASH_MS) return -1;
        tickIn(16);
        return t / (float) A_FLASH_MS;
    }

    /** A's highlight: rings spreading from the button (gold, fading), the second one A_FLASH_MS - A_FLASH_RING_MS later */
    private void drawAFlash(Canvas canvas, float x, float y, float r, float f) {
        for (int k = 0; k < 2; k++) {
            float t = (f * A_FLASH_MS - k * (A_FLASH_MS - A_FLASH_RING_MS)) / A_FLASH_RING_MS;
            if (t < 0 || t >= 1) continue;
            flashPaint.setStyle(Paint.Style.STROKE);
            flashPaint.setStrokeWidth(r * 0.16f * (1 - t * 0.6f));
            flashPaint.setColor(0xFFFFE27A);
            flashPaint.setAlpha((int) (230 * (1 - t) * (1 - t)));
            canvas.drawCircle(x, y, r * (1.0f + 0.65f * t), flashPaint);
        }
    }
    private final Paint flashPaint = new Paint(Paint.ANTI_ALIAS_FLAG);

    /** A's size during its highlight: a pop to 1.15 and back in the first third */
    private static float aPop(float f) {
        return f < 0 || f >= 0.33f ? 1f : 1f + 0.15f * (float) Math.sin(Math.PI * f / 0.33f);
    }

    private static final long TALK_HOLD_MS = 500;
    private long talkSeenAt = -TALK_HOLD_MS;
    private boolean talkHeld;
    private String lastTalkLog = "";
    /** a talk's text by touch: a tap anywhere (but on ZR's fast forward) is A, like on the title screen. Not with Minigames
     *  by touch off: a choice is a plain talk then and needs the stick. Not in the hold after it (a tap would start
     *  the talk again) */
    private boolean talkTap() { return talkNow() && minigameOn && !quietScene; }
    /** a message choice, number entry, a shop's item choice or its chosen item up close: only A / B do anything */
    private boolean shopTalk() {
        return contextActive() && (minigame >= MINIGAME_CHOICE && minigame <= MINIGAME_SHOP || minigame == MINIGAME_SHOP_HELD)
                && !singing() && !conductPads();
    }
    private static final int MINIGAME_SHOP = 8;
    private static final int MINIGAME_SHOP_HELD = 12;  // the chosen item up close: a tap on it goes on, elsewhere puts it back
    private boolean quietScene;  // an event without text, or a fade / wipe (pollState): no buttons, no d-pad
    private long textSeenAt;
    private boolean sceneText;  // this event has shown a message
    private static final long QUIET_SCENE_MS = 1000;
    private int micPointer = -1;
    private final SongPanel songPanel = new SongPanel(this);

    /** the mic button shows while Link conducts: the other controls step aside (+ and B stay) */
    private boolean singing() { return micShown && songConducting && !editing; }
    private final RectF micRect = new RectF(), beatRect = new RectF();

    private boolean placeMic() {
        if (!micShown || editing || drcMode) return false;  // (not over the GamePad picture: the song list's play buttons)
        float d = getResources().getDisplayMetrics().density, r = 32 * d, rb = 21 * d;
        RectF tv = tvRect.isEmpty() ? new RectF(0, 0, getWidth(), getHeight()) : tvRect;
        float cx = tv.left + 40 * d + r, cy = tv.bottom - 28 * d - r;
        micRect.set(cx - r, cy - r, cx + r, cy + r);
        float by = cy - r - 14 * d - rb;  // the time switch above it
        beatRect.set(cx - rb, by - rb, cx + rb, by + rb);
        return true;
    }

    // the time switch: the game's round base with the time (3/4, 4/4, 6/4) Link conducts in while the mic button
    // is held (SongListener.conductsByVoice)
    private void drawBeat(Canvas canvas, int a) {
        float r = beatRect.width() / 2, cx = beatRect.centerX(), cy = beatRect.centerY();
        android.graphics.Bitmap base = GameIcons.get(getContext(), GameIcons.BASE_ROUND, () -> post(this::invalidate));
        if (base != null) {
            float bs = r * 2 * 1.14f;
            baseSrc.set(0, 0, base.getWidth(), base.getHeight());
            tmp.set(cx - bs / 2, cy - bs / 2, cx + bs / 2, cy + bs / 2);
            iconPaint.setAlpha(a);
            canvas.drawBitmap(base, baseSrc, tmp, iconPaint);
        } else {
            songBg.setColor(0xE0FFFFFF);
            canvas.drawCircle(cx, cy, r, songBg);
        }
        String t = listener.songBeat() + "/4";
        if (GameFont.covers(t) && GameFont.ready(getContext(), this::invalidate)) {  // the game's font, as its buttons
            android.graphics.Bitmap b = GameFont.label(t, r * 0.95f / GameFont.cellHeight(), 0xFFFFFFFF, GAME_INK);
            tmp.set(cx - b.getWidth() / 2f, cy - b.getHeight() / 2f, cx + b.getWidth() / 2f, cy + b.getHeight() / 2f);
            iconPaint.setAlpha(a);
            canvas.drawBitmap(b, null, tmp, iconPaint);
            return;
        }
        songPaint.setTextSize(r * 0.8f);
        songPaint.setFakeBoldText(true);
        songPaint.setColor((a << 24) | (GAME_INK & 0xFFFFFF));
        songPaint.setTextAlign(Paint.Align.CENTER);
        canvas.drawText(t, cx, cy - (songPaint.descent() + songPaint.ascent()) / 2, songPaint);
    }

    private void drawMic(Canvas canvas) {
        if (!placeMic()) return;
        float r = micRect.width() / 2, cx = micRect.centerX(), cy = micRect.centerY(), k = micHeld ? 0.9f : 1f;
        int a = Math.max(160, (int) (255 * opacity));
        android.graphics.Bitmap base = GameIcons.get(getContext(), GameIcons.BASE_ROUND, () -> post(this::invalidate));
        if (base != null) {
            float bs = r * 2 * 1.14f * k;
            baseSrc.set(0, 0, base.getWidth(), base.getHeight());
            tmp.set(cx - bs / 2, cy - bs / 2, cx + bs / 2, cy + bs / 2);
            iconPaint.setAlpha(a);
            canvas.drawBitmap(base, baseSrc, tmp, iconPaint);
        } else {
            songBg.setColor(0xE0FFFFFF);
            canvas.drawCircle(cx, cy, r * k, songBg);
        }
        if (micHeld) {  // listening: a red ring, as recorders show it
            songBg.setColor(0xFFE0402A);
            songBg.setStyle(Paint.Style.STROKE);
            songBg.setStrokeWidth(r * 0.12f);
            canvas.drawCircle(cx, cy, r * 1.08f, songBg);
            songBg.setStyle(Paint.Style.FILL);
        }
        Pictograms.draw(canvas, "mic", cx, cy, r * 2 * 0.62f * k, (a << 24) | (micHeld ? 0xC0301E : GAME_INK & 0xFFFFFF), micHeld);
        drawBeat(canvas, a);
        if (songConducting) {
            SongListener.Take take = listener.songTake();
            boolean practice = listener.songPractice();
            songPanel.draw(canvas, tvRect.isEmpty() ? new RectF(0, 0, getWidth(), getHeight()) : tvRect, take, practice);
            if (take != null && take.end >= 0) postInvalidateDelayed(250);  // (it goes again after a while)
        }
        if (micHeld) {
            drawCount(canvas);
            postInvalidateDelayed(33);  // (the count moves)
        }
    }

    // ---- conducting by note pads (Controls > Minigames by touch > Note pads; off: two invisible sticks, minigames.cpp
    // conduct_step): the right pad is the C stick as a d-pad of the game's five note signs (centre, up, right, down,
    // left; the finger let go = centre), the left one the stick's three times (left 4/4, centre 3/4, right 6/4,
    // JAIZelInst::setBeat), picked by a tap and held until another. MainActivity.pushInput sends both. While singing
    // (the mic button) the time switch picks the time: only the note pad shows.
    static final int HINT_CONDUCT_PAD = 30;  // GameHint: its own hint (not the two sticks')
    static final int HINT_LEAF_GLIDE = 31;   // GameHint: the Deku Leaf's button while gliding (item buttons by tap)
    static final int HINT_RING = 32;         // GameHint: how the item ring opens (Link's first item, ringHint)
    private static final int[] PAD_TIMES = {4, 3, 6};
    private boolean conductPadsOn = true, padsLaid;
    private boolean stickFollows = true;  // the left stick (fixed or floating) is pulled along by a thumb leaving its range (off: stays put)
    private int notePointer = -1, timePointer = -1;
    private int padNote, padTime = 3;  // the C stick's direction (0 centre, 1 up, 2 right, 3 down, 4 left); the time
    private float noteX, noteY, noteArm, noteSign, timeX, timeY, timeStep, timeR;

    void setStickFollows(boolean on) {
        stickFollows = on;
    }

    void setConductPads(boolean on) {
        conductPadsOn = on;
        invalidate();
    }

    /** the note pads show (and their touches count) */
    boolean conductPads() {
        return conductPadsOn && minigame == MINIGAME_CONDUCT && controlsVisible && !editing && !drcMode && front == 0;
    }

    private boolean timePadShown() { return conductPads() && !singing(); }

    /** the C stick's direction the note pad holds (0 centre, 1 up, 2 right, 3 down, 4 left) */
    int conductNote() { return conductPads() ? padNote : 0; }

    /** the time the time pad holds (3, 4, 6); 3 (the stick in the middle) while it doesn't show */
    int conductTime() { return timePadShown() ? padTime : 3; }

    private void resetPads() {
        notePointer = timePointer = -1;
        padNote = 0;
        padTime = 3;
    }

    // the right pad where the right stick sits (under B), the left one where the left stick does (the controls' unit)
    private void placePads() {
        int W = getWidth(), H = getHeight();
        float u = Math.min(W, H) / 7f * scale, m = u * 0.25f;
        noteArm = u * 0.78f;
        noteSign = u * 0.66f;
        noteX = W - m - u * 1.6f;
        noteY = H - m - u * 1.0f;
        timeR = u * 0.36f;
        timeStep = u * 0.88f;
        timeX = m + u * 1.6f;
        timeY = noteY;
    }

    private int noteAt(float x, float y) {
        float dx = x - noteX, dy = y - noteY;
        if (Math.hypot(dx, dy) < noteArm * 0.45f) return 0;
        if (Math.abs(dx) > Math.abs(dy)) return dx > 0 ? 2 : 4;
        return dy < 0 ? 1 : 3;
    }

    private boolean onNotePad(float x, float y) {
        return Math.hypot(x - noteX, y - noteY) <= noteArm + noteSign * 0.75f;
    }

    private boolean onTimePad(float x, float y) {
        return Math.abs(y - timeY) <= timeR * 1.6f && Math.abs(x - timeX) <= timeStep + timeR * 1.4f;
    }

    private int timeAt(float x) {
        int i = Math.round((x - timeX) / timeStep);
        return PAD_TIMES[Math.max(-1, Math.min(1, i)) + 1];
    }

    /** a finger landed (true: a pad took it) */
    private boolean padDown(int id, float x, float y) {
        if (!conductPads()) return false;
        placePads();
        if (notePointer < 0 && onNotePad(x, y)) {
            notePointer = id;
            setPad(noteAt(x, y), padTime);
            return true;
        }
        if (timePointer < 0 && timePadShown() && onTimePad(x, y)) {
            timePointer = id;
            setPad(padNote, timeAt(x));
            return true;
        }
        return false;
    }

    private void padMove(int id, float x, float y) {
        if (id == notePointer) setPad(noteAt(x, y), padTime);
        else if (id == timePointer) setPad(padNote, timeAt(x));
    }

    /** a finger went up (true: it was a pad's) */
    private boolean padUp(int id) {
        if (id == notePointer) {
            notePointer = -1;
            setPad(0, padTime);  // (let go: the C stick back in the middle, the centre note)
            return true;
        }
        if (id == timePointer) {
            timePointer = -1;  // (the time stays)
            return true;
        }
        return false;
    }

    private void setPad(int note, int time) {
        if (note == padNote && time == padTime) return;
        padNote = note;
        padTime = time;
        listener.onControlsChanged();
        invalidate();
    }

    private void drawPads(Canvas canvas) {
        if (!conductPads()) return;
        placePads();
        int a = Math.max(160, (int) (255 * opacity));
        Runnable redraw = () -> post(this::invalidate);
        // the note pad: the five signs as the game's row shows them, the one held lit (and larger)
        songBg.setColor((Math.min(a, 200) * 3 / 8) << 24);
        canvas.drawCircle(noteX, noteY, noteArm + noteSign * 0.62f, songBg);
        for (int d = 0; d <= 4; d++) {
            float px = noteX + (d == 2 ? noteArm : d == 4 ? -noteArm : 0), py = noteY + (d == 1 ? -noteArm : d == 3 ? noteArm : 0);
            boolean lit = d == padNote;
            float s = noteSign * (lit && notePointer >= 0 ? 1.15f : 1f);
            android.graphics.Bitmap b = GameIcons.get(getContext(), "#baton:" + d + ":" + (lit ? 1 : 0), redraw);
            if (b == null) {
                songBg.setColor((a << 24) | (lit ? 0xFFE100 : 0xB8B8B8));
                canvas.drawCircle(px, py, s * 0.3f, songBg);
                continue;
            }
            tmp.set(px - s / 2, py - s / 2, px + s / 2, py + s / 2);
            iconPaint.setAlpha(lit ? 255 : a);
            canvas.drawBitmap(b, null, tmp, iconPaint);
        }
        if (!timePadShown()) return;
        // the time pad: the game's round base with the time, the one held in the game's yellow
        android.graphics.Bitmap base = GameIcons.get(getContext(), GameIcons.BASE_ROUND, redraw);
        boolean font = GameFont.ready(getContext(), this::invalidate);
        for (int i = 0; i < 3; i++) {
            float cx = timeX + (i - 1) * timeStep, cy = timeY;
            boolean on = PAD_TIMES[i] == padTime;
            float r = timeR * (on ? 1.1f : 1f);
            int ai = on ? 255 : a;
            if (base != null) {
                float bs = r * 2 * 1.14f;
                baseSrc.set(0, 0, base.getWidth(), base.getHeight());
                tmp.set(cx - bs / 2, cy - bs / 2, cx + bs / 2, cy + bs / 2);
                iconPaint.setAlpha(ai);
                canvas.drawBitmap(base, baseSrc, tmp, iconPaint);
            } else {
                songBg.setColor((ai << 24) | 0xFFFFFF);
                canvas.drawCircle(cx, cy, r, songBg);
            }
            String t = PAD_TIMES[i] + "/4";
            if (font && GameFont.covers(t)) {
                android.graphics.Bitmap b = GameFont.label(t, r * 0.95f / GameFont.cellHeight(), on ? 0xFFFFE100 : 0xFFFFFFFF, GAME_INK);
                tmp.set(cx - b.getWidth() / 2f, cy - b.getHeight() / 2f, cx + b.getWidth() / 2f, cy + b.getHeight() / 2f);
                iconPaint.setAlpha(ai);
                canvas.drawBitmap(b, null, tmp, iconPaint);
            } else {
                songPaint.setTextSize(r * 0.8f);
                songPaint.setFakeBoldText(true);
                songPaint.setColor((ai << 24) | ((on ? 0xC08000 : GAME_INK) & 0xFFFFFF));
                songPaint.setTextAlign(Paint.Align.CENTER);
                canvas.drawText(t, cx, cy - (songPaint.descent() + songPaint.ascent()) / 2, songPaint);
            }
        }
    }

    // ---- the count-in (SongListener.count, at the game's tempo): the beat's number, in the game's font, left of the game's time bar
    // and note row (BatonInput_00: the bar at y 321, the signs at 245 of the 1280 x 720 layout, centred); each
    // number grows on its click and settles
    private int countShown;
    private long countSince;

    private void drawCount(Canvas canvas) {
        int k = listener.songCount();
        if (k != countShown) {
            countShown = k;
            countSince = SystemClock.uptimeMillis();
        }
        if (k <= 0) return;
        RectF tv = tvRect.isEmpty() ? new RectF(0, 0, getWidth(), getHeight()) : tvRect;
        float u = tv.height() / 720f;  // the layout's unit
        float cx = tv.centerX() - 360 * u, cy = tv.top + (360 - 283) * u;
        float pop = Math.max(0, 1 - (SystemClock.uptimeMillis() - countSince) / 250f);
        float px = 96 * u * (1 + 0.35f * pop);  // (the system font's, while the game's isn't ready)
        String t = Integer.toString(k);
        if (GameFont.ready(getContext(), this::invalidate)) {
            // (one size of label, scaled when drawn: the font caches each size it makes)
            android.graphics.Bitmap b = GameFont.label(t, 96 * u / GameFont.cellHeight(), 0xFFFFE100, 0xFF1B2A4A);
            float k2 = 1 + 0.35f * pop, bw = b.getWidth() * k2, bh = b.getHeight() * k2;
            baseSrc.set(0, 0, b.getWidth(), b.getHeight());
            tmp.set(cx - bw / 2, cy - bh / 2, cx + bw / 2, cy + bh / 2);
            iconPaint.setAlpha(255);
            canvas.drawBitmap(b, baseSrc, tmp, iconPaint);
        } else {
            songPaint.setTextSize(px * 0.7f);
            songPaint.setFakeBoldText(true);
            songPaint.setColor(0xFFFFE100);
            songPaint.setTextAlign(Paint.Align.CENTER);
            canvas.drawText(t, cx, cy - (songPaint.descent() + songPaint.ascent()) / 2, songPaint);
        }
    }

    void showBanner(String text) {
        banner = text;
        bannerUntil = SystemClock.uptimeMillis() + BANNER_MS;
        invalidate();
        postDelayed(this::invalidate, BANNER_MS + 20);
    }

    private void drawSongs(Canvas canvas) {
        boolean showBanner = banner != null && SystemClock.uptimeMillis() < bannerUntil;
        if (!showBanner && (!songListening || micShown)) return;  // (with the mic button: the song panel shows it)
        float d = getResources().getDisplayMetrics().density;
        RectF tv = tvRect.isEmpty() ? new RectF(0, 0, getWidth(), getHeight()) : tvRect;
        StringBuilder heard = new StringBuilder("\uD83C\uDFA4");  // (a microphone)
        for (int i = 0; i < Math.min(songNotes, 8); i++) heard.append(i == 0 ? " ♪" : "♪");
        String t = showBanner ? banner : heard.toString();
        songPaint.setTextSize((showBanner ? 18 : 15) * d);
        songPaint.setFakeBoldText(showBanner);
        songPaint.setColor(0xFFFFFFFF);
        songPaint.setTextAlign(Paint.Align.CENTER);
        float w = songPaint.measureText(t) + 28 * d, h = (showBanner ? 38 : 30) * d;
        float cx = tv.centerX(), top = tv.top + 12 * d;
        tmp.set(cx - w / 2, top, cx + w / 2, top + h);
        songBg.setColor(showBanner ? 0xD0284A7A : 0x90000000);
        canvas.drawRoundRect(tmp, h / 2, h / 2, songBg);
        canvas.drawText(t, cx, top + h / 2 - (songPaint.descent() + songPaint.ascent()) / 2, songPaint);
    }

    // ---- Picto Box album pages (Game > Album pages, picto_pages.cpp): while the album is open a bar at the top of
    // the TV picture, ◀ page / count ▶ (on the last page + starts a new one; the game ignores it on an empty page);
    // a tap on either end turns the page, ZL / L do on a controller (the runtime takes them)
    private int pageCur = -1, pageCount, pageUsed;
    private boolean pageBar;
    private final RectF pageBarRect = new RectF();

    // the album's picto: what it caught (Game > Picto subjects): the gold medal's subject (the figurine Carlov can
    // make of it) by the game's own figurine name, or the subject with why the picto missed the medal
    private int pictoSubject = -1;

    // ---- a picto mailed at a postbox (postbox.cpp): what was mailed, delivered or why not, for a few seconds
    private String postNotice;
    private long postNoticeUntil;
    private boolean postboxNear;
    static final int HINT_POSTBOX = 33;  // GameHint: after the postbox asked what to send, a picto for Carlov?

    private void pollPostbox() {
        boolean near = Native.getOption("postbox_near") != 0;
        if (near != postboxNear) {
            postboxNear = near;
            invalidate();
        }
        if (Native.getOption("postbox_offer") != 0 && !editing) askHint(HINT_POSTBOX, getContext().getString(R.string.postbox_ask));
        int n = Native.getOption("postbox_notice");
        if (n < 0) return;
        int kind = n >> 8, fig = n & 0xFF;
        String name = GameText.figurineName(fig + 0x49);
        if (name == null) name = "?";
        int id;
        switch (kind) {
            case 1: id = R.string.postbox_mailed; break;
            case 2: id = R.string.postbox_delivered; break;
            case 10: id = R.string.postbox_no_carlov; break;
            case 11: id = R.string.postbox_mono; break;
            case 12: id = R.string.postbox_no_subject; break;
            case 13: id = R.string.postbox_made; break;
            case 14: id = R.string.postbox_queued; break;
            case 15: id = R.string.postbox_face; break;
            case 16: id = R.string.postbox_body; break;
            default: id = R.string.postbox_full; break;
        }
        postNotice = getContext().getString(id, name);
        postNoticeUntil = SystemClock.uptimeMillis() + 5000;
        invalidate();
        postDelayed(this::invalidate, 5050);
    }

    private void drawPostNotice(Canvas canvas) {
        if (postNotice == null || SystemClock.uptimeMillis() > postNoticeUntil || editing) return;
        RectF tv = tvRect.isEmpty() ? new RectF(0, 0, getWidth(), getHeight()) : tvRect;
        float k = tv.height() / 720f, top = tv.top + 20 * k;
        if (gameLook()) {  // (letters the game's font lacks: drawGameText draws them in the system font)
            float scale = BOX_TEXT * k / GameFont.MSG.height(), h = BOX_H * k;
            float w = Math.min(gameTextWidth(postNotice, scale) + 2 * BOX_PAD * k, tv.width() - 20 * k), cx = tv.centerX();
            RectF box = new RectF(cx - w / 2, top, cx + w / 2, top + h);
            drawGameBox(canvas, box);
            drawGameText(canvas, postNotice, box.left + BOX_PAD * k, top + (BOX_H - BOX_TEXT) / 2 * k, scale, 0xFFFFFFFF, k);
            return;
        }
        float d = getResources().getDisplayMetrics().density;
        songPaint.setTextSize(15 * d);
        songPaint.setFakeBoldText(false);
        songPaint.setTextAlign(Paint.Align.CENTER);
        float h = 36 * d, w = songPaint.measureText(postNotice) + 32 * d, cx = tv.centerX();
        tmp.set(cx - w / 2, top, cx + w / 2, top + h);
        songBg.setColor(0xC0000000);
        canvas.drawRoundRect(tmp, h / 2, h / 2, songBg);
        songPaint.setColor(0xFFFFFFFF);
        canvas.drawText(postNotice, cx, top + h / 2 - (songPaint.descent() + songPaint.ascent()) / 2, songPaint);
    }

    /** MainActivity's poll (every style and controller) */
    void pictoPages(int[] s) {
        pollPostbox();
        boolean open = Native.getOption("picto_album") != 0;  // (R: back to the Picto Box; the touch state isn't kept with every style)
        int subject = open && MainActivity.instance != null && MainActivity.instance.prefs.getBoolean("picto_subject", true)
                ? Native.getOption("picto_subject") : -1;
        if (subject != pictoSubject) {
            pictoSubject = subject;
            invalidate();
        }
        int cur = open ? Native.getOption("picto_page") : -1;
        int n = cur >= 0 ? Native.getOption("picto_pages") : 0;
        int used = cur >= 0 ? Native.getOption("picto_page_used") : 0;
        boolean bar = cur >= 0 && n > 0;
        if (bar == pageBar && cur == pageCur && n == pageCount && used == pageUsed) return;
        pageBar = bar;
        pageCur = cur;
        pageCount = n;
        pageUsed = used;
        invalidate();
    }

    // ---- in the game's look (as GameHint's): its choice box (MsgSubWindowC2_00), its font in white with a soft
    // shadow, the inventory's yellow arrows to turn the pages, the album's medal (BestShotMark_00) before a medal
    // subject's name; plain boxes until the textures and the font are built. Sizes in layout units (720 high)
    private final Paint gameImg = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG),
            gameGlyph = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private GameUi.MenuArrow pageArrowL, pageArrowR;
    private static final float BOX_H = 62, BOX_TEXT = 30, BOX_PAD = 26, BOX_ARROW = 46;
    // the medal's gold, as the album shows it: dark to deep gold, light to a pale yellow
    private static final android.graphics.ColorMatrixColorFilter MEDAL_GOLD = GameUi.GameTabPlate.lerpFilter(0xFF8C5A00, 0xFFFFF48A);

    private boolean gameLook() {
        Runnable again = () -> post(this::invalidate);
        return GameFont.MSG.isReady(getContext(), again) && GameIcons.get(getContext(), "MsgSubWindowC2_00^t", again) != null;
    }

    /** the game's choice box over r: its left half as the texture has it, the middle stretched, the right half mirrored */
    private void drawGameBox(Canvas c, RectF r) {
        android.graphics.Bitmap sub = GameIcons.get(getContext(), "MsgSubWindowC2_00^t", null);
        if (sub == null) return;
        // the pieces opaque, a pixel over each other at whole pixels, in a layer blended once (each piece translucent
        // left a light line where they met: the filtered edges let the picture through)
        int w = sub.getWidth(), hgt = sub.getHeight();
        android.graphics.Rect midSrc = new android.graphics.Rect(w - 4, 0, w - 2, hgt);
        float left = Math.round(r.left), right = Math.round(r.right), half = Math.round(Math.min(r.height() * w / hgt, (right - left) / 2));
        int layer = c.saveLayerAlpha(left - 2, r.top - 2, right + 2, r.bottom + 2, 225);
        gameImg.setAlpha(255);
        gameImg.setColorFilter(null);
        tmp.set(left + half - 1, r.top, right - half + 1, r.bottom);
        c.drawBitmap(sub, midSrc, tmp, gameImg);
        tmp.set(left, r.top, left + half, r.bottom);
        c.drawBitmap(sub, null, tmp, gameImg);
        c.save();
        c.scale(-1, 1, right - half / 2, 0);
        tmp.set(right - half, r.top, right, r.bottom);
        c.drawBitmap(sub, null, tmp, gameImg);
        c.restore();
        c.restoreToCount(layer);
    }

    private float gameTextWidth(String s, float scale) {
        float w = 0;
        for (int i = 0; i < s.length(); i++) {
            char ch = s.charAt(i);
            if (GameFont.MSG.has(ch)) w += GameFont.MSG.advance(ch) * scale;
            else {
                songPaint.setTextSize(GameFont.MSG.height() * scale * 0.62f);
                songPaint.setFakeBoldText(true);
                w += songPaint.measureText(String.valueOf(ch));
            }
        }
        return w;
    }

    /** s in the game's font from x, its cell's top at y: a soft shadow, then the letters in colour */
    private float drawGameText(Canvas c, String s, float x, float y, float scale, int colour, float k) {
        for (int i = 0; i < s.length(); i++) {
            if (!GameFont.MSG.has(s.charAt(i))) {  // a letter the font lacks: the system font's, as tall
                songPaint.setTextSize(GameFont.MSG.height() * scale * 0.62f);
                songPaint.setFakeBoldText(true);
                songPaint.setTextAlign(Paint.Align.LEFT);
                String ch = String.valueOf(s.charAt(i));
                float base = y + GameFont.MSG.height() * scale * 0.72f;
                songPaint.setColor(0x90000000);
                c.drawText(ch, x + 2 * k, base + 2 * k, songPaint);
                songPaint.setColor(colour);
                c.drawText(ch, x, base, songPaint);
                x += songPaint.measureText(ch);
                continue;
            }
            gameGlyph.setColor(0x90000000);
            GameFont.MSG.draw(c, s.charAt(i), x + 2 * k, y + 2 * k, scale, gameGlyph);
            gameGlyph.setColor(colour);
            x += GameFont.MSG.draw(c, s.charAt(i), x, y, scale, gameGlyph);
        }
        return x;
    }

    private void drawPictoSubject(Canvas canvas, float top) {
        int subject = pictoSubject & 0xFF, flaws = (pictoSubject >> 8) & 0xFF;
        boolean colour = (pictoSubject >> 16 & 1) != 0;
        String name = pictoSubject < 0 || editing ? null : GameText.figurineName(subject);
        if (name == null) return;
        boolean medal = colour && flaws == 0;
        String why = "";
        if (!medal) {
            StringBuilder b = new StringBuilder();
            if (!colour) b.append(getContext().getString(R.string.picto_mono));
            if ((flaws & 1) != 0) b.append(b.length() > 0 ? ", " : "").append(getContext().getString(R.string.picto_turned_away));
            if ((flaws & 2) != 0) b.append(b.length() > 0 ? ", " : "").append(getContext().getString(R.string.picto_hidden));
            if (b.length() > 0) why = " · " + b;
        }
        RectF tv = tvRect.isEmpty() ? new RectF(0, 0, getWidth(), getHeight()) : tvRect;
        android.graphics.Bitmap mark = medal ? GameIcons.get(getContext(), "BestShotMark_00^t", () -> post(this::invalidate)) : null;
        if (gameLook() && GameFont.MSG.hasAll(name + why)) {
            float k = tv.height() / 720f, scale = BOX_TEXT * k / GameFont.MSG.height(), h = BOX_H * k, icon = mark != null ? h * 0.9f : 0;
            float tw = gameTextWidth(name, scale) + gameTextWidth(why, scale);
            float w = tw + icon + (icon > 0 ? 8 * k : 0) + 2 * BOX_PAD * k, cx = tv.centerX();
            RectF box = new RectF(cx - w / 2, top, cx + w / 2, top + h);
            drawGameBox(canvas, box);
            float x = box.left + BOX_PAD * k, ty = top + (BOX_H - BOX_TEXT) / 2 * k;
            if (mark != null) {
                tmp.set(x, top + (h - icon) / 2, x + icon, top + (h + icon) / 2);
                gameImg.setAlpha(255);
                gameImg.setColorFilter(MEDAL_GOLD);  // (the texture is a grey mask the game tints)
                canvas.drawBitmap(mark, null, tmp, gameImg);
                gameImg.setColorFilter(null);
                x += icon + 8 * k;
            }
            // the name as the game marks names in its messages (orange), why it missed the medal in grey
            x = drawGameText(canvas, name, x, ty, scale, medal ? 0xFFFFC832 : 0xFFFF7A2E, k);
            drawGameText(canvas, why, x, ty, scale, 0xFFD8D8D8, k);
            return;
        }
        String t = medal ? "★ " + name : name + why;
        float d = getResources().getDisplayMetrics().density;
        songPaint.setTextSize(16 * d);
        songPaint.setFakeBoldText(medal);
        songPaint.setTextAlign(Paint.Align.CENTER);
        float h = 36 * d, w = songPaint.measureText(t) + 32 * d, cx = tv.centerX();
        tmp.set(cx - w / 2, top, cx + w / 2, top + h);
        songBg.setColor(0xC0000000);
        canvas.drawRoundRect(tmp, h / 2, h / 2, songBg);
        songPaint.setColor(medal ? 0xFFFFD54A : 0xFFE8E8E8);  // (gold: the medal)
        canvas.drawText(t, cx, top + h / 2 - (songPaint.descent() + songPaint.ascent()) / 2, songPaint);
    }

    private boolean pageBarTouch(float x, float y) {
        if (!pageBar || editing || !pageBarRect.contains(x, y)) return false;
        float third = pageBarRect.width() / 3;
        int dir = x < pageBarRect.left + third ? -1 : x > pageBarRect.right - third ? 1 : 0;
        if (dir != 0) {
            Native.setOption("picto_page", dir);
            postDelayed(() -> pictoPages(Native.touchState()), 120);
        }
        return true;
    }

    private void drawPageBar(Canvas canvas) {
        float dd = getResources().getDisplayMetrics().density;
        RectF tv = tvRect.isEmpty() ? new RectF(0, 0, getWidth(), getHeight()) : tvRect;
        if (!pageBar || editing) {
            pageBarRect.setEmpty();
            drawPictoSubject(canvas, tv.top + 10 * dd);
            return;
        }
        String t = getContext().getString(R.string.picto_page, pageCur + 1, pageCount);
        // (a new page only after one with pictos, and not in the picto chooser: there the last page ends the bar)
        boolean last = pageCur >= pageCount - 1 && !chooser(), canBack = pageCur > 0,
                canOn = pageCur < pageCount - 1 || last && pageUsed > 0;
        if (gameLook() && GameFont.MSG.hasAll(t)) {
            float k = tv.height() / 720f, scale = BOX_TEXT * k / GameFont.MSG.height(), h = BOX_H * k, a = BOX_ARROW * k;
            float w = gameTextWidth(t, scale) + 2 * (a + 2 * BOX_PAD * k), cx = tv.centerX(), top = tv.top + 10 * k;
            pageBarRect.set(cx - w / 2, top, cx + w / 2, top + h);
            drawGameBox(canvas, pageBarRect);
            drawGameText(canvas, t, cx - gameTextWidth(t, scale) / 2, top + (BOX_H - BOX_TEXT) / 2 * k, scale, 0xFFFFFFFF, k);
            if (pageArrowL == null) {
                pageArrowL = new GameUi.MenuArrow(getContext(), true);
                pageArrowR = new GameUi.MenuArrow(getContext(), false);
            }
            float ay = top + (h - a) / 2, ax0 = pageBarRect.left + BOX_PAD * k, ax1 = pageBarRect.right - BOX_PAD * k - a;
            pageArrowL.setBounds(Math.round(ax0), Math.round(ay), Math.round(ax0 + a), Math.round(ay + a));
            pageArrowL.setAlpha(canBack ? 255 : 70);
            pageArrowL.draw(canvas);
            if (last) {  // a new page: the game's yellow, a plus in its font
                float ps = a * 1.15f / GameFont.MSG.height();
                drawGameText(canvas, "+", ax1 + (a - GameFont.MSG.advance('+') * ps) / 2, ay - a * 0.08f, ps, canOn ? 0xFFF7E81B : 0x46F7E81B, k);
            } else {
                pageArrowR.setBounds(Math.round(ax1), Math.round(ay), Math.round(ax1 + a), Math.round(ay + a));
                pageArrowR.setAlpha(canOn ? 255 : 70);
                pageArrowR.draw(canvas);
            }
            drawPictoSubject(canvas, pageBarRect.bottom + 6 * k);
            return;
        }
        float d = dd;
        songPaint.setTextSize(17 * d);
        songPaint.setFakeBoldText(true);
        songPaint.setTextAlign(Paint.Align.CENTER);
        float h = 44 * d, arrow = 56 * d, w = songPaint.measureText(t) + 2 * arrow;
        float cx = tv.centerX(), top = tv.top + 10 * d, base = top + h / 2 - (songPaint.descent() + songPaint.ascent()) / 2;
        pageBarRect.set(cx - w / 2, top, cx + w / 2, top + h);
        songBg.setColor(0xC0000000);
        canvas.drawRoundRect(pageBarRect, h / 2, h / 2, songBg);
        songPaint.setColor(0xFFFFFFFF);
        canvas.drawText(t, cx, base, songPaint);
        songPaint.setTextSize(20 * d);
        songPaint.setColor(canBack ? 0xFFFFFFFF : 0x50FFFFFF);
        canvas.drawText("◀", pageBarRect.left + arrow / 2, base, songPaint);
        songPaint.setColor(canOn ? 0xFFFFFFFF : 0x50FFFFFF);
        canvas.drawText(last ? "+" : "▶", pageBarRect.right - arrow / 2, base, songPaint);
        drawPictoSubject(canvas, pageBarRect.bottom + 8 * d);
    }

    private final android.graphics.Path shapePath = new android.graphics.Path();

    /** a hexagon or diamond button's outline, its corners rounded */
    private void drawPolygon(Canvas canvas, Control c) {
        float x = c.cx, y = c.cy, hw = c.w / 2, hh = c.h / 2;
        shapePath.reset();
        if (c.shape == SHAPE_HEX) {
            shapePath.moveTo(x - hw, y);
            shapePath.lineTo(x - hw / 2, y - hh);
            shapePath.lineTo(x + hw / 2, y - hh);
            shapePath.lineTo(x + hw, y);
            shapePath.lineTo(x + hw / 2, y + hh);
            shapePath.lineTo(x - hw / 2, y + hh);
        } else {
            shapePath.moveTo(x, y - hh);
            shapePath.lineTo(x + hw, y);
            shapePath.lineTo(x, y + hh);
            shapePath.lineTo(x - hw, y);
        }
        shapePath.close();
        android.graphics.CornerPathEffect round = new android.graphics.CornerPathEffect(Math.min(c.w, c.h) * 0.12f);
        fill.setPathEffect(round);
        stroke.setPathEffect(round);
        canvas.drawPath(shapePath, fill);
        canvas.drawPath(shapePath, stroke);
        fill.setPathEffect(null);
        stroke.setPathEffect(null);
    }

    private void drawControl(Canvas canvas, Control c, int a0) {
        if (!Float.isNaN(c.alpha)) a0 = (int) (255 * c.alpha);
        // the editor: hidden controls faint, and the jump buttons while Jump anywhere / the moon jump is off
        boolean offMod = c.kind == KIND_BUTTON && (c.bit & (Native.JUMP | Native.MOON)) != 0 && (c.bit & jumpInputs()) == 0;
        int a = editing && (c.hidden || offMod) ? a0 / 3 : a0;
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
                if (contextActive() && c.round && drawGameButton(canvas, c, a)) break;
                if (contextActive() && c.kind == KIND_BUTTON && c.bit == Native.A) {
                    float flash = aFlash();
                    if (flash >= 0) drawAFlash(canvas, c.cx, c.cy, Math.min(c.w, c.h) / 2, flash);
                }
                float inner = c.shape == SHAPE_DIAMOND ? 0.62f : 1f;  // the icon / name's room: a diamond's is smaller
                if (c.shape == SHAPE_HEX || c.shape == SHAPE_DIAMOND) {
                    drawPolygon(canvas, c);
                } else if (c.round) {
                    float r = Math.min(c.w, c.h) / 2;
                    canvas.drawCircle(c.cx, c.cy, r, fill);
                    canvas.drawCircle(c.cx, c.cy, r, stroke);
                } else {
                    tmp.set(c.cx - c.w / 2, c.cy - c.h / 2, c.cx + c.w / 2, c.cy + c.h / 2);
                    canvas.drawRoundRect(tmp, c.h * c.corner, c.h * c.corner, fill);
                    canvas.drawRoundRect(tmp, c.h * c.corner, c.h * c.corner, stroke);
                }
                android.graphics.Bitmap icon = iconOf(c);
                if (icon != null) {
                    iconPaint.setAlpha(Math.min(255, a + 70));
                    drawCentred(canvas, icon, c.cx, c.cy, Math.min(c.w, c.h) * 0.8f * inner);
                    break;
                }
                if (Pictograms.draw(canvas, pictogramOf(c), c.cx, c.cy, Math.min(c.w, c.h) * 0.62f * inner, text.getColor(), c.pressed)) break;
                String lbl = labelOf(c);
                float ts = text.getTextSize();
                float fit = c.round || inner < 1 ? Math.min(c.w, c.h) * 0.85f * inner : Math.min(c.w * 0.8f, c.h * 1.6f);
                if (lbl.length() > 2 && text.measureText(lbl) > fit) text.setTextSize(ts * fit / text.measureText(lbl));
                canvas.drawText(lbl, c.cx, c.cy - (text.descent() + text.ascent()) / 2, text);
                text.setTextSize(ts);
            }
        }
    }

    private static final int PLUS_INDEX = 11;

    /** the + button's place on the screen (OptionsMenu draws it on its inventory page), or null */
    RectF plusRect() {
        Control c = controls.get(PLUS_INDEX);
        if (c.w <= 0) return null;
        return new RectF(c.cx - c.w / 2, c.cy - c.h / 2, c.cx + c.w / 2, c.cy + c.h / 2);
    }

    /** the + button as it looks on this view (OptionsMenu's inventory page: the way back to the game) */
    void drawPlus(Canvas canvas) {
        drawControl(canvas, controls.get(PLUS_INDEX), (int) (255 * opacity));
    }

    // the game's look (context buttons): its HUD's white button base (A: the yellow star), the icon on
    // it, or the action's name in the game's lettering (dark, with a white edge)
    private final Paint gameText = new Paint(Paint.ANTI_ALIAS_FLAG), gameEdge = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final android.graphics.Rect baseSrc = new android.graphics.Rect();
    private static final int GAME_INK = 0xFF1B2A4A;

    /** false: the bases aren't built yet (then the plain look) */
    private boolean drawGameButton(Canvas canvas, Control c, int a) {
        android.graphics.Bitmap icon = iconOf(c);
        boolean star = c.bit == Native.A && !GameIcons.isLinkPose(icon);  // (Link's poses: on the round base)
        android.graphics.Bitmap base = GameIcons.get(getContext(), star ? GameIcons.BASE_STAR : GameIcons.BASE_ROUND,
                () -> post(this::invalidate));
        if (base == null) return false;
        float flash = c.bit == Native.A ? aFlash() : -1;
        float r = Math.min(c.w, c.h) / 2, k = (c.pressed ? 0.9f : 1f) * aPop(flash);
        if (flash >= 0) drawAFlash(canvas, c.cx, c.cy, r, flash);
        float bs = r * 2 * (star ? 1.32f : 1.14f) * k;  // the base's shadow and points reach past the circle
        // the base's face (its shadow hangs below): icon, pictogram and name go on its centre
        float fx = c.cx + bs * (0.49f - 0.5f), fy = c.cy + bs * ((star ? 0.485f : 0.46f) - 0.5f);
        int alpha = Math.min(255, a + 70);
        baseSrc.set(0, 0, base.getWidth(), base.getHeight());
        tmp.set(c.cx - bs / 2, c.cy - bs / 2, c.cx + bs / 2, c.cy + bs / 2);
        iconPaint.setAlpha(alpha);
        canvas.drawBitmap(base, baseSrc, tmp, iconPaint);
        if (icon != null) {
            drawCentred(canvas, icon, fx, fy, r * 2 * 0.78f * k);
            return true;
        }
        if (Pictograms.draw(canvas, pictogramOf(c), fx, fy, r * 2 * 0.62f * k, (alpha << 24) | (GAME_INK & 0xFFFFFF), false)) return true;
        String lbl = labelOf(c);
        float ts = text.getTextSize() * k, fit = r * 2 * (star ? 1.05f : 0.85f) * k;
        gameText.setTextAlign(Paint.Align.CENTER);
        gameText.setFakeBoldText(true);
        gameText.setTextSize(ts);
        if (lbl.length() > 2 && gameText.measureText(lbl) > fit) gameText.setTextSize(ts * fit / gameText.measureText(lbl));
        gameEdge.set(gameText);
        gameEdge.setStyle(Paint.Style.STROKE);
        gameEdge.setStrokeJoin(Paint.Join.ROUND);
        gameEdge.setStrokeWidth(gameText.getTextSize() * 0.22f);
        gameEdge.setColor(0xFFFFFFFF);
        gameEdge.setAlpha(alpha);
        gameText.setColor(GAME_INK);
        gameText.setAlpha(alpha);
        float y = fy - (gameText.descent() + gameText.ascent()) / 2;
        canvas.drawText(lbl, fx, y, gameEdge);
        canvas.drawText(lbl, fx, y, gameText);
        return true;
    }

    /** an icon s wide, its visible part (GameIcons.contentOffset) centred on (x, y), with iconPaint */
    private void drawCentred(Canvas canvas, android.graphics.Bitmap icon, float x, float y, float s) {
        float[] o = GameIcons.contentOffset(icon);
        x -= o[0] * s;
        y -= o[1] * s;
        iconSrc.set(0, 0, icon.getWidth(), icon.getHeight());
        tmp.set(x - s / 2, y - s / 2, x + s / 2, y + s / 2);
        canvas.drawBitmap(icon, iconSrc, tmp, iconPaint);
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
            String gt = album() ? (dx < 0 ? "#arrow:270" : dx > 0 ? "#arrow:90" : null)  // the album: previous / next photo
                    : dy < 0 ? "CollectIcon118_15^l" : dx < 0 ? "IconCannon_00^q" : dx > 0 ? "IconSalvage_00^q" : null;
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
