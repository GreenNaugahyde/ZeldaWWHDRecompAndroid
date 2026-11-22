package org.wwhdrecomp.app;

import android.annotation.SuppressLint;
import android.app.Dialog;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.view.Choreographer;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.MotionEvent;
import android.view.View;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;

import java.util.ArrayList;
import java.util.List;

/**
 * The + menu's Loot tab (first draft; a tab drawn beside the game's own, ControlsView.setLootTab): what the save file in
 * play has collected, Weapons (Sword, Shield) and Gear (Head, Chest, Outfit), dragged onto a slot as the items onto X / Y
 * / R in the game's menu. A strip of thumbnails along the bottom scrolls sideways (finger, D-pad, left stick); the thing in its middle stands large above
 * it with what it is worth, its picture from the game's own files. A tap on that one chooses it and a tap on its slot puts it on; held or pulled
 * up / down it can be dragged there; the slot tapped with nothing chosen takes it off. The Gear page has a window on the real Link: the TV picture, cut to him (MainActivity.setLootPreview),
 * the game running meanwhile (his model's proportions are set as he moves); a swipe in it turns the camera.
 * "All (test)" shows everything there is, a tap adds it to the collection: quests give nothing yet.
 */
final class LootMenu extends Dialog {
    private static LootMenu shown;

    static void open(MainActivity a) {
        if (!BuildConfig.LOOT) return;
        if (shown != null && shown.isShowing()) return;
        shown = new LootMenu(a);
        shown.show();
    }

    private static final Loot[][] PAGES = {{Loot.SWORDS, Loot.SHIELDS, Loot.SWORDS}, {Loot.HELMETS, Loot.CHESTS, Loot.OUTFITS}};
    private static final int[][] WORDS = {{R.string.loot_menu_sword, R.string.loot_menu_shield, R.string.loot_cat_other},
            {R.string.loot_menu_head, R.string.loot_menu_chest, R.string.loot_menu_outfit}};
    private static final int GEAR = 1, OTHER_TAB = 2;  // (the Weapons page's third list: what goes on X / Y / R)
    private static final String[] CATEGORY = {Loot.SWORD, Loot.SHIELD, Loot.OTHER};
    private static final int[] QUICK_ORDER = {1, 0, 2};  // Y, X, R, as the game's menu (LootQuick's numbers)
    private static final String[] QUICK_WORDS = {"Y", "X", "R"};
    // layout units (1280 x 720, y up, as the game's layouts and PadScreen)
    private static final float SHEET_L = -617, SHEET_T = 300, SHEET_R = 585, SHEET_B = -300;
    private static final float LIST_L = -570, LIST_R = -20, LIST_T = 140, LIST_B = -270;
    private static final float PIC = 250, PIC_X = -436, PIC_Y = -8, INFO_X = -166, BAND_B = -150;  // the preview: its picture, what is written beside it
    private static final float TRAY_T = -164, TRAY_B = -270, THUMB = 92, STEP_1 = 104, STEP_N = 86;  // the strip: a thumbnail, how far the next ones stand
    private static final long POP_MS = 180;
    private static final int JUMP = 5;
    private static final long REPEAT_FIRST_MS = 280, REPEAT_MS = 110, REPEAT_FAST_MS = 60, LIFT_MS = 350;
    private static final int[] RARITY_ON_PAPER = {0xFF909090, 0xFF2E9E3E, 0xFF2A70D8, 0xFFD08A10};
    private static final int[] RARITY = {R.string.loot_rarity_0, R.string.loot_rarity_1, R.string.loot_rarity_2, R.string.loot_rarity_3};
    private static final int[] ACTS = {R.string.loot_sword_boko, R.string.loot_sword_machete, R.string.loot_sword_mace, R.string.loot_sword_darknut, R.string.loot_sword_spear, R.string.loot_sword_pgsword};
    private static final float HOLE_L = 30, HOLE_R = 310, HOLE_T = 200, HOLE_B = -270;

    private final MainActivity a;
    private final Page view;
    private final int file = PlayerModels.playedFile();
    private int page, selected = -1;
    private final int[] sub = new int[PAGES.length];
    private boolean all;
    private List<String> ids = new ArrayList<>();
    private List<String> names = new ArrayList<>();
    private float stickX, stickY, leftX;
    private int hatY;
    private long leftNext;
    private int leftDir;

    private LootMenu(MainActivity a) {
        super(a, android.R.style.Theme_Translucent_NoTitleBar_Fullscreen);
        this.a = a;
        view = new Page();
        Window w = getWindow();
        if (w != null) {
            w.setWindowAnimations(0);
            w.getAttributes().layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_ALWAYS;
        }
        setContentView(view);
        rebuild();
    }

    private Loot kind() { return PAGES[page][sub[page]]; }

    private boolean other() { return page != GEAR && sub[page] == OTHER_TAB; }

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
        Choreographer.getInstance().postFrameCallback(tick);
    }

    @Override
    public void dismiss() {
        if (isShowing()) MenuSounds.play(a, MenuSounds.CLOSE);
        Choreographer.getInstance().removeFrameCallback(tick);
        a.setLootPreview(null);
        super.dismiss();
    }

    // a controller's right stick turns the camera while the window on Link is open
    private final Choreographer.FrameCallback tick = new Choreographer.FrameCallback() {
        @Override
        public void doFrame(long now) {
            if (!isShowing()) return;
            if (page == GEAR && (stickX != 0 || stickY != 0)) Native.cameraSwipe(stickX * 8, stickY * 8);
            final int dir = Math.abs(leftX) > 0.5f ? (leftX > 0 ? 1 : -1) : 0;  // (left stick / D-pad on a hat: one step, then again while held)
            final long ms = android.os.SystemClock.uptimeMillis();
            if (dir != leftDir) {
                leftDir = dir;
                if (dir != 0) {
                    view.step(dir);
                    leftNext = ms + REPEAT_FIRST_MS;
                }
            } else if (dir != 0 && ms >= leftNext) {
                view.step(dir);
                leftNext = ms + (Math.abs(leftX) > 0.9f ? REPEAT_FAST_MS : REPEAT_MS);
            }
            view.frame(now);
            Choreographer.getInstance().postFrameCallback(this);
        }
    };

    @Override
    public boolean onGenericMotionEvent(MotionEvent e) {
        if ((e.getSource() & InputDevice.SOURCE_JOYSTICK) == 0) return false;
        final float x = e.getAxisValue(MotionEvent.AXIS_Z), y = e.getAxisValue(MotionEvent.AXIS_RZ), lx = e.getAxisValue(MotionEvent.AXIS_X), hx = e.getAxisValue(MotionEvent.AXIS_HAT_X), hy = e.getAxisValue(MotionEvent.AXIS_HAT_Y);
        stickX = Math.abs(x) > 0.2f ? x : 0;
        stickY = Math.abs(y) > 0.2f ? y : 0;
        leftX = Math.abs(hx) > 0.5f ? hx : lx;
        final int up = hy < -0.5f ? -1 : hy > 0.5f ? 1 : 0;
        if (up != hatY) {
            hatY = up;
            if (up != 0) view.step(up * JUMP);
        }
        return true;  // (handled here: the framework would make D-pad keys of them as well)
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BACK || code == KeyEvent.KEYCODE_BUTTON_START || code == InputMapper.menuBack()) {
            dismiss();
            return true;
        }
        if (code == InputMapper.menuSelect()) {
            view.choose();
            return true;
        }
        final int bit = a.padBit(code), quick = bit == Native.X ? 0 : bit == Native.Y ? 1 : bit == Native.R ? 2 : -1;
        if (quick >= 0 && other() && !all && !ids.isEmpty()) {  // (as in the game's menu: the item chosen onto that button)
            toQuick(quick, view.cursor);
            return true;
        }
        if (bit == Native.Y && !other()) {  // (nothing chosen: off its slot)
            takeOff(sub[page]);
            return true;
        }
        switch (code) {
            case KeyEvent.KEYCODE_DPAD_LEFT: view.step(-1); return true;
            case KeyEvent.KEYCODE_DPAD_RIGHT: view.step(1); return true;
            case KeyEvent.KEYCODE_DPAD_UP: view.step(-JUMP); return true;
            case KeyEvent.KEYCODE_DPAD_DOWN: view.step(JUMP); return true;
            case KeyEvent.KEYCODE_BUTTON_L1: setPage(page + PAGES.length - 1); return true;
            case KeyEvent.KEYCODE_BUTTON_R1: setPage(page + 1); return true;
            case KeyEvent.KEYCODE_BUTTON_L2: setSub(sub[page] + PAGES[page].length - 1); return true;
            case KeyEvent.KEYCODE_BUTTON_R2: setSub(sub[page] + 1); return true;
            default: return super.onKeyDown(code, e);
        }
    }

    private void setPage(int p) {
        page = p % PAGES.length;
        MenuSounds.play(a, MenuSounds.TAB);
        rebuild();
    }

    private void setSub(int i) {
        sub[page] = i % PAGES[page].length;
        MenuSounds.play(a, MenuSounds.SWITCH);
        rebuild();
    }

    // the list of the page shown, by name
    private void rebuild() {
        final Loot k = kind();
        selected = -1;
        view.resetScroll();
        final java.util.Set<String> owned = k.owned(a, file);
        List<String[]> rows = new ArrayList<>();
        for (String id : k.ids())
            if ((all || owned.contains(id)) && (page == GEAR || CATEGORY[sub[page]].equals(k.category(a, id)))) rows.add(new String[] {id, k.name(a, id)});
        rows.sort((x, y) -> x[1].compareToIgnoreCase(y[1]));
        ids = new ArrayList<>(rows.size());
        names = new ArrayList<>(rows.size());
        for (String[] r : rows) {
            ids.add(r[0]);
            names.add(r[1]);
        }
        view.placeHole();
        view.invalidate();
    }

    // on its own slot only
    private void putOn(int i) {
        if (file == 0 || i < 0 || i >= ids.size() || other()) return;
        Loot k = kind();
        if (!k.owns(a, file, ids.get(i)) || askRepair(k, ids.get(i))) return;
        k.set(a, file, ids.get(i));
        selected = -1;
        MenuSounds.play(a, MenuSounds.DECIDE);
        view.invalidate();
    }

    // a weapon on X / Y / R (LootQuick); on the button it already has: off it
    private void toQuick(int button, int i) {
        if (file == 0 || i < 0 || i >= ids.size() || !other()) return;
        Loot k = kind();
        String id = ids.get(i);
        if (!k.owns(a, file, id) || askRepair(k, id)) return;
        boolean off = a.lootQuick.holds(file, button, k, id);
        a.lootQuick.set(file, button, k, off ? null : id);
        selected = -1;
        MenuSounds.play(a, off ? MenuSounds.CANCEL : MenuSounds.DECIDE);
        view.invalidate();
    }

    // a broken thing cannot be put on: asks to repair it (true: it was broken)
    private boolean askRepair(Loot k, String id) {
        if (!k.broken(a, file, id)) return false;
        final int cost = k.repairCost(id);
        final GameDialog d = new GameDialog(a).title(k.name(a, id));
        if (Native.lootWorldRupees() < cost) d.message(a.getString(R.string.loot_repair_no, cost)).button(R.string.opt_ok, null);
        else d.message(a.getString(R.string.loot_repair_q, cost)).button(R.string.loot_repair, () -> {
            Native.lootWorldPay(cost);
            k.setBroken(a, file, id, false);
            MenuSounds.play(a, MenuSounds.DECIDE);
            view.invalidate();
        }).button(R.string.opt_cancel, null);
        d.show();
        return true;
    }

    private void takeOff(int slot) {
        Loot k = PAGES[page][slot];
        if (file == 0 || k.get(a, file) == null) return;
        k.set(a, file, null);
        MenuSounds.play(a, MenuSounds.CANCEL);
        view.invalidate();
    }

    // in "All (test)": into or out of the collection (what is worn stays on until taken off)
    private void toggleOwned(int i) {
        if (file == 0) return;
        Loot k = kind();
        String id = ids.get(i);
        k.setOwned(a, file, id, !k.owns(a, file, id));
        MenuSounds.play(a, MenuSounds.SWITCH);
        view.invalidate();
    }

    @SuppressLint("ViewConstructor")
    private final class Page extends View {
        private final float dp = a.getResources().getDisplayMetrics().density;
        private final Runnable redraw = () -> post(this::invalidate);
        private final GameDraw draw = new GameDraw(getContext(), redraw);
        private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), bmp = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private float s = 1, cx, cy;
        int cursor;  // the thing in the middle
        private float pos;  // where the row stands (cursor while nothing moves; between two while it moves)
        private long lastFrame;
        private boolean frameOdd;
        private final Paint glow = new Paint(Paint.ANTI_ALIAS_FLAG), picture = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
        private final android.graphics.Path path = new android.graphics.Path();
        private final android.graphics.ColorMatrixColorFilter grey;
        private final RectF arrowL = new RectF(), arrowR = new RectF();
        private float flingV;  // cards a second, as the finger left
        private long lastMoveAt, popAt;
        private final RectF preview = new RectF();
        private GameUi.GameBackground background;
        private GameUi.GamePaper paper;
        private float paperUnit;
        private final RectF[] tabs = {new RectF(), new RectF()}, subs = {new RectF(), new RectF(), new RectF()}, slots = {new RectF(), new RectF(), new RectF()},
                quicks = {new RectF(), new RectF(), new RectF()};
        private final RectF list = new RectF(), hole = new RectF(), allBtn = new RectF(), close = new RectF();
        // the finger
        private float downX, downY, lastX, lastY, dragX, dragY;
        private int downRow = -1, dragRow = -1;
        private boolean moved, scrolling, swiping;

        Page() {
            super(a);
            setBackgroundColor(Color.TRANSPARENT);
            final android.graphics.ColorMatrix m = new android.graphics.ColorMatrix();
            m.setSaturation(0);
            grey = new android.graphics.ColorMatrixColorFilter(m);
        }

        private float X(float x) { return cx + x * s; }
        private float Y(float y) { return cy - y * s; }
        private RectF box(float l, float t, float r, float b) { return new RectF(X(l), Y(t), X(r), Y(b)); }

        void resetScroll() { cursor = 0; pos = 0; }

        private boolean holeOn() { return page == GEAR; }

        /** the window on Link: where it is on the screen, to the game's picture */
        void placeHole() {
            if (getWidth() == 0 || !holeOn()) {
                a.setLootPreview(null);
                return;
            }
            int[] at = new int[2];
            getLocationOnScreen(at);
            RectF r = new RectF(hole);
            r.offset(at[0], at[1]);
            a.setLootPreview(r);
        }

        private void measureAll(int w, int h) {
            s = Math.min(w / 1280f, h / 720f);
            cx = w / 2f;
            cy = h / 2f;
            list.set(box(LIST_L, LIST_T, LIST_R, LIST_B));
            hole.set(box(HOLE_L, HOLE_T, HOLE_R, HOLE_B));
            allBtn.set(box(-165, 198, -20, 148));
            close.set(box(530, 355, 615, 270));
        }

        @Override
        protected void onSizeChanged(int w, int h, int ow, int oh) {
            measureAll(w, h);
            post(this::placeHole);
        }

        @Override
        protected void onDraw(Canvas c) {
            int w = getWidth(), h = getHeight();
            if (w == 0) return;
            measureAll(w, h);
            c.save();
            if (holeOn()) c.clipOutRect(hole);
            if (background == null) background = new GameUi.GameBackground(getContext(), s);
            background.setBounds(0, 0, w, h);
            background.draw(c);
            if (paper == null || paperUnit != s) {
                paper = new GameUi.GamePaper(getContext(), s);
                paperUnit = s;
            }
            paper.setBounds(new Rect(Math.round(X(SHEET_L)), Math.round(Y(SHEET_T)), Math.round(X(SHEET_R)), Math.round(Y(SHEET_B))));
            paper.draw(c);
            drawSubTabs(c);
            if (file == 0) draw.msg(c, a.getString(R.string.loot_test_no_file), cx, cy, 44 * s, GameUi.OPTION_TEXT, 0);
            else {
                drawList(c);
                drawSlots(c);
            }
            drawTabs(c);
            Bitmap back = GameIcons.get(getContext(), "Back_00^t", redraw);
            if (back != null) c.drawBitmap(back, null, close, bmp);
            c.restore();
            if (holeOn()) {
                fill.setStyle(Paint.Style.STROKE);
                fill.setStrokeWidth(4 * s);
                fill.setColor(0xFF9C6D20);
                c.drawRect(hole, fill);
                fill.setStyle(Paint.Style.FILL);
            }
            if (dragRow >= 0) {
                String name = names.get(dragRow);
                Bitmap dragged = LootIcons.get(a, kind(), ids.get(dragRow), redraw);
                if (dragged != null) c.drawBitmap(dragged, null, new RectF(dragX - 70 * s, dragY - 210 * s, dragX + 70 * s, dragY - 70 * s), bmp);
                float tw = draw.msgWidth(name, 34 * s) + 30 * s;
                RectF r = new RectF(dragX - tw / 2, dragY - 70 * s, dragX + tw / 2, dragY - 20 * s);
                fill.setColor(0xE0202830);
                c.drawRoundRect(r, 12 * s, 12 * s, fill);
                draw.msg(c, name, r.centerX(), r.centerY(), 34 * s, 0xFFFFFFFF, 0);
            }
        }

        private void drawTabs(Canvas c) {
            int[] words = {R.string.loot_menu_weapons, R.string.loot_menu_gear};
            GameUi.Palette[] pals = {GameUi.BLUE, GameUi.AMBER};
            int[] outlines = {0xFF003192, 0xFFB2410A};
            for (int i = 0; i < 2; i++) {
                boolean on = i == page;
                float x = -462 + i * 305, tw = on ? 290 : 256, th = on ? 122 : 106, top = on ? 350 : 352;
                RectF r = box(x - tw / 2, top, x + tw / 2, top - th);
                tabs[i].set(r);
                GameUi.GameTabPlate p = new GameUi.GameTabPlate(getContext(), on, pals[i], 2 * s);
                p.setBounds(Math.round(r.left), Math.round(r.top), Math.round(r.right), Math.round(r.bottom));
                p.draw(c);
                draw.title(c, a.getString(words[i]), r.centerX(), r.centerY() - th * 0.04f * s, th * (on ? 0.44f : 0.40f) * s, 0xFFFFFFFF, outlines[i]);
            }
        }

        // the kinds of the page, and the test list's switch
        private void drawSubTabs(Canvas c) {
            for (int i = 0; i < subs.length; i++) subs[i].setEmpty();
            for (int i = 0; i < PAGES[page].length; i++) {
                subs[i].set(box(-570 + i * 135, 198, -570 + i * 135 + 125, 148));
                plate(c, subs[i], i == sub[page], a.getString(WORDS[page][i]));
            }
            plate(c, allBtn, all, a.getString(R.string.loot_menu_all));
        }

        private void plate(Canvas c, RectF r, boolean on, String word) {
            GameUi.GameRowPlate p = new GameUi.GameRowPlate(getContext(), on ? 0xFFC87A10 : 0xFF2A9AD6, on ? 0xFFFFD27A : 0xFF8AD8F8, s);
            p.setBounds(Math.round(r.left), Math.round(r.top), Math.round(r.right + 6 * s), Math.round(r.bottom + 8 * s));
            p.draw(c);
            draw.msg(c, word, r.centerX(), r.centerY(), 32 * s, 0xFF00141E, 0);
        }

        // ---------------------------------------------------------------- the row
        private float cardX(float d) {
            final float a = Math.abs(d), o = a <= 1 ? a * STEP_1 : STEP_1 + (a - 1) * STEP_N;
            return list.centerX() + Math.signum(d) * o * s;
        }

        private float cardScale(float d) {
            final float a = Math.abs(d);
            return a <= 1 ? 1 - 0.24f * a : Math.max(0.6f, 0.76f - 0.08f * (a - 1));
        }

        private float cardY() { return Y((TRAY_T + TRAY_B) / 2); }

        /** each frame while the menu is open: the row settling on its place, the glow breathing */
        void frame(long nanos) {
            final float dt = lastFrame == 0 ? 0 : Math.min(0.05f, (nanos - lastFrame) / 1e9f);
            lastFrame = nanos;
            if (android.os.SystemClock.uptimeMillis() - popAt < POP_MS) invalidate();
            if (!scrolling && pos != cursor) {
                pos += (cursor - pos) * Math.min(1, dt * 14);
                if (Math.abs(cursor - pos) < 0.002f) pos = cursor;
            }
            frameOdd = !frameOdd;
            if (frameOdd && !ids.isEmpty()) invalidate();
        }

        private void setCursor(int i) {
            final int c = Math.max(0, Math.min(ids.size() - 1, i));
            if (c == cursor) return;
            cursor = c;
            selected = -1;
            popAt = android.os.SystemClock.uptimeMillis();
            MenuSounds.play(a, MenuSounds.CURSOR);
        }

        // the thing under the finger: the large picture is the one in the middle; on the strip the nearest to the middle first; -1 none
        private int cardAt(float x, float y) {
            if (ids.isEmpty()) return -1;
            if (preview.contains(x, y)) return cursor;
            if (!list.contains(x, y)) return -1;
            final float half = THUMB / 2 * s;
            for (int off = 0; off <= 3; off++)
                for (int sign = -1; sign <= 1; sign += 2) {
                    final int i = Math.round(pos) + sign * off;
                    if (i < 0 || i >= ids.size()) continue;
                    final float k = cardScale(i - pos);
                    if (Math.abs(x - cardX(i - pos)) < half * k && Math.abs(y - cardY()) < half + 6 * s) return i;
                }
            return -1;
        }

        private void drawList(Canvas c) {
            final Loot k = kind();
            final String worn = k.get(a, file);
            preview.set(box(PIC_X - PIC / 2, PIC_Y + PIC / 2, PIC_X + PIC / 2, PIC_Y - PIC / 2));
            c.save();
            c.clipRect(list);
            drawBand(c);
            drawTray(c);
            if (ids.isEmpty()) draw.msg(c, a.getString(R.string.loot_menu_empty), list.centerX(), Y(PIC_Y), 38 * s, GameUi.OPTION_TEXT, 0);
            else {
                drawPreview(c, k, worn);
                c.save();
                c.clipRect(box(LIST_L + 6, TRAY_T, LIST_R - 6, TRAY_B));
                final int first = Math.max(0, (int) Math.floor(pos) - 4), last = Math.min(ids.size() - 1, (int) Math.ceil(pos) + 4);
                final List<Integer> order = new ArrayList<>();  // (the far ones first: the middle one on top)
                for (int i = first; i <= last; i++) order.add(i);
                order.sort((x, y) -> Float.compare(Math.abs(y - pos), Math.abs(x - pos)));
                for (int i : order) drawThumb(c, k, i, worn);
                c.restore();
            }
            c.restore();
            if (ids.isEmpty()) return;
            drawArrows(c);
            drawCardInfo(c, k);
        }

        // the game's item page: wide pale chevrons behind the preview, drifting with the strip
        private void drawBand(Canvas c) {
            final float top = list.top, bottom = Y(BAND_B), mid = (top + bottom) / 2, gap = 170 * s, w = 88 * s, skew = 46 * s;
            fill.setColor(0x2E5C8DA6);
            c.drawRect(list.left, top, list.right, bottom, fill);
            fill.setColor(0x305C8DA6);
            float shift = (-pos * 62 * s) % gap;
            if (shift > 0) shift -= gap;
            for (float x = list.left - 2 * gap + shift; x < list.right + gap; x += gap) {
                path.reset();
                path.moveTo(x, top);
                path.lineTo(x + w, top);
                path.lineTo(x + w + skew, mid);
                path.lineTo(x + w, bottom);
                path.lineTo(x, bottom);
                path.lineTo(x + skew, mid);
                path.close();
                c.drawPath(path, fill);
            }
            fill.setColor(0xFFFFFFFF);
            c.drawRect(list.left, bottom - 3 * s, list.right, bottom, fill);
        }

        // the strip's tray
        private void drawTray(Canvas c) {
            final RectF t = box(LIST_L, TRAY_T, LIST_R, TRAY_B);
            fill.setColor(0xC82E4A5E);
            c.drawRoundRect(t, 16 * s, 16 * s, fill);
            fill.setColor(0x40FFFFFF);
            c.drawRoundRect(t.left + 4 * s, t.top + 3 * s, t.right - 4 * s, t.top + 9 * s, 3 * s, 3 * s, fill);
        }

        private void drawPreview(Canvas c, Loot k, String worn) {
            final String id = ids.get(cursor);
            final boolean broken = !all && k.broken(a, file, id), owned = !all || k.owns(a, file, id);
            final long ms = android.os.SystemClock.uptimeMillis();
            final float x = X(PIC_X), y = Y(PIC_Y), base = PIC / 2 * s;
            final float t = Math.min(1, (ms - popAt) / (float) POP_MS), pop = 0.82f + 0.18f * (1 - (1 - t) * (1 - t));  // (a new one comes up to size)
            final float half = base * pop, breath = 1 + 0.05f * (float) Math.sin(ms / 420.0), r = base * 1.12f * breath;
            if (k.rarity(id) >= 2 && !broken && owned) drawRays(c, x, y, r * 1.06f, ms, k.rarity(id), 1);
            final int core = cursor == selected ? 0x00D7FF : 0xFFE860;
            glow.setShader(new android.graphics.RadialGradient(x, y, r, new int[] {0xF0000000 | core, 0x90000000 | core, core}, new float[] {0, 0.55f, 1}, android.graphics.Shader.TileMode.CLAMP));
            c.drawCircle(x, y, r, glow);
            glow.setShader(null);
            final Bitmap b = LootIcons.get(a, k, id, redraw);
            picture.setAlpha(owned ? 255 : 130);
            picture.setColorFilter(broken || !owned ? grey : null);
            if (b != null) {
                final float bob = (float) Math.sin(ms / 520.0) * 5 * s;
                c.drawBitmap(b, null, new RectF(x - half, y - half + bob, x + half, y + half + bob), picture);
            } else {  // (being drawn from the game's files)
                fill.setColor(0x405C8DA6);
                c.drawCircle(x, y, half * 0.6f, fill);
            }
            picture.setColorFilter(null);
            picture.setAlpha(255);
            if (id.equals(worn) || other() && isQuick(k, id)) drawWornMark(c, x + base * 0.72f, y - base * 0.72f, 1);
        }

        private void drawThumb(Canvas c, Loot k, int i, String worn) {
            final float d = i - pos, ad = Math.abs(d), scale = cardScale(d), x = cardX(d), y = cardY(), half = THUMB / 2 * s * scale;
            final String id = ids.get(i);
            final boolean broken = !all && k.broken(a, file, id), owned = !all || k.owns(a, file, id);
            final int alpha = ad <= 2.2f ? 255 : Math.max(0, 255 - (int) (255 * (ad - 2.2f) / 1.1f));
            if (alpha == 0) return;
            final float near = Math.max(0, 1 - ad);
            final RectF t = new RectF(x - half, y - half, x + half, y + half);
            if (near > 0) {
                fill.setColor((int) (0xB0 * near) << 24 | (i == selected ? 0x00D7FF : 0xFFE860));
                c.drawRoundRect(t.left - 8 * s, t.top - 8 * s, t.right + 8 * s, t.bottom + 8 * s, 18 * s, 18 * s, fill);
            }
            fill.setColor(alpha * 15 / 16 << 24 | 0xEDEAD7);
            c.drawRoundRect(t, 12 * s, 12 * s, fill);
            fill.setColor(alpha << 24 | (RARITY_ON_PAPER[k.rarity(id)] & 0xFFFFFF));  // (its rarity along the foot)
            c.drawRoundRect(t.left + 6 * s, t.bottom - 9 * s, t.right - 6 * s, t.bottom - 4 * s, 2 * s, 2 * s, fill);
            final Bitmap b = LootIcons.get(a, k, id, redraw);
            if (b != null) {
                picture.setAlpha(owned ? alpha : alpha / 2);
                picture.setColorFilter(broken || !owned ? grey : null);
                final float in = 6 * s;
                c.drawBitmap(b, null, new RectF(t.left + in, t.top + in, t.right - in, t.bottom - in - 4 * s), picture);
                picture.setColorFilter(null);
                picture.setAlpha(255);
            }
            if (id.equals(worn) || other() && isQuick(k, id)) drawWornMark(c, t.right - 8 * s, t.top + 8 * s, 0.6f);
        }

        private boolean isQuick(Loot k, String id) {
            for (int b = 0; b < LootQuick.BUTTONS; b++)
                if (a.lootQuick.holds(file, b, k, id)) return true;
            return false;
        }

        // slow rays behind the rarer things
        private void drawRays(Canvas c, float x, float y, float r, long ms, int rarity, float near) {
            final int n = rarity >= 3 ? 12 : 8;
            final int color = (int) ((rarity >= 3 ? 0x90 : 0x60) * near) << 24 | (RARITY_ON_PAPER[rarity] & 0xFFFFFF);
            fill.setColor(color);
            final double turn = ms / 4200.0;
            for (int i = 0; i < n; i++) {
                final double t = turn + i * 2 * Math.PI / n, w = Math.PI / n * 0.55;
                path.reset();
                path.moveTo(x, y);
                path.lineTo(x + (float) Math.cos(t - w) * r, y + (float) Math.sin(t - w) * r);
                path.lineTo(x + (float) Math.cos(t + w) * r, y + (float) Math.sin(t + w) * r);
                path.close();
                c.drawPath(path, fill);
            }
        }

        // a yellow badge with a tick: it is on
        private void drawWornMark(Canvas c, float x, float y, float scale) {
            final float r = 24 * s * Math.max(0.6f, scale);
            fill.setColor(0xFF7A4A08);
            c.drawCircle(x, y, r + 3 * s, fill);
            fill.setColor(GameUi.YELLOW);
            c.drawCircle(x, y, r, fill);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(r * 0.28f);
            fill.setStrokeCap(Paint.Cap.ROUND);
            fill.setColor(0xFF7A4A08);
            path.reset();
            path.moveTo(x - r * 0.5f, y + r * 0.02f);
            path.lineTo(x - r * 0.12f, y + r * 0.42f);
            path.lineTo(x + r * 0.55f, y - r * 0.4f);
            c.drawPath(path, fill);
            fill.setStyle(Paint.Style.FILL);
            fill.setStrokeCap(Paint.Cap.BUTT);
        }

        // the game's yellow arrows at the ends of the strip, nudging outward
        private void drawArrows(Canvas c) {
            final float nudge = (float) Math.abs(Math.sin(android.os.SystemClock.uptimeMillis() / 300.0)) * 5 * s, y = cardY(), h = 30 * s, w = 22 * s, hit = 44 * s;
            arrowL.setEmpty();
            arrowR.setEmpty();
            if (cursor > 0) {
                arrowL.set(list.left, y - hit, list.left + 1.6f * hit, y + hit);
                arrow(c, list.left + 4 * s - nudge + w, y, -w, h);
            }
            if (cursor < ids.size() - 1) {
                arrowR.set(list.right - 1.6f * hit, y - hit, list.right, y + hit);
                arrow(c, list.right - 4 * s + nudge - w, y, w, h);
            }
        }

        private void arrow(Canvas c, float x, float y, float w, float h) {
            path.reset();
            path.moveTo(x + w, y);
            path.lineTo(x, y - h);
            path.lineTo(x + w * 0.16f, y);
            path.lineTo(x, y + h);
            path.close();
            fill.setColor(0x88FFFFFF);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(9 * s);
            fill.setStrokeJoin(Paint.Join.ROUND);
            c.drawPath(path, fill);
            fill.setStyle(Paint.Style.FILL);
            fill.setStrokeJoin(Paint.Join.MITER);
            fill.setColor(GameUi.YELLOW);
            c.drawPath(path, fill);
        }

        // about the one in the middle, beside its picture: its name on a plate in its rarity's colour, what it does, what is left of it
        private void drawCardInfo(Canvas c, Loot k) {
            final String id = ids.get(cursor);
            final boolean broken = !all && k.broken(a, file, id);
            final int rarity = k.rarity(id);
            final float x = X(INFO_X), room = 300 * s;
            final String name = broken ? a.getString(R.string.loot_broken_tag, names.get(cursor)) : names.get(cursor);
            final float wide = Math.min(room, draw.msgWidth(name, 34 * s) + 60 * s);
            final RectF plate = new RectF(x - wide / 2, Y(92), x + wide / 2, Y(92) + 58 * s);
            final int[][] colours = {{0xFF8A9AA4, 0xFFD6E0E6}, {0xFF3AA24A, 0xFFA6E8A0}, {0xFF2A86D6, 0xFF8AD8F8}, {0xFFC87A10, 0xFFFFD27A}};
            final int[] col = broken ? new int[] {0xFFA04030, 0xFFE8A090} : colours[rarity];
            GameUi.GameRowPlate p = new GameUi.GameRowPlate(getContext(), col[0], col[1], s);
            p.setBounds(Math.round(plate.left), Math.round(plate.top), Math.round(plate.right), Math.round(plate.bottom));
            p.draw(c);
            draw.msg(c, cut(name, wide - 30 * s, 34 * s), x, plate.centerY() - 3 * s, 34 * s, 0xFF00141E, 0);
            draw.msg(c, a.getString(RARITY[rarity]), x, Y(14), 30 * s, RARITY_ON_PAPER[rarity], 0);
            draw.msg(c, cut(k == Loot.SHIELDS ? a.getString(R.string.loot_no_wear) : rowStat(k, id), room, 30 * s), x, Y(-28), 30 * s, GameUi.OPTION_TEXT, 0);
            if (wears(k)) {
                final float w = 270 * s, h = 22 * s, y = Y(-76);
                final int total = k.uses(id), left = broken ? 0 : Math.max(0, total - k.wear(a, file, id));
                final float part = total == 0 ? 0 : left / (float) total;
                final RectF bar = new RectF(x - w / 2, y - h / 2, x + w / 2, y + h / 2);
                fill.setColor(0xFF2E3D47);
                c.drawRoundRect(bar.left - 3 * s, bar.top - 3 * s, bar.right + 3 * s, bar.bottom + 3 * s, 13 * s, 13 * s, fill);
                fill.setColor(0xFF55646E);
                c.drawRoundRect(bar, 10 * s, 10 * s, fill);
                if (part > 0) {
                    fill.setColor(part < 0.25f ? 0xFFE04030 : part < 0.5f ? 0xFFF2B21C : 0xFF40C050);
                    c.drawRoundRect(bar.left, bar.top, bar.left + w * part, bar.bottom, 10 * s, 10 * s, fill);
                    fill.setColor(0x60FFFFFF);
                    c.drawRoundRect(bar.left + 4 * s, bar.top + 3 * s, bar.left + Math.max(0, w * part - 4 * s), bar.top + 8 * s, 3 * s, 3 * s, fill);
                }
                draw.msg(c, durability(k, id, true), x, y - 1 * s, 22 * s, 0xFFFFFFFF, 0);
            }
            draw.msg(c, (cursor + 1) + " / " + ids.size(), list.right - 10 * s, Y(124), 26 * s, GameUi.OPTION_HINT, 1);
        }

        private boolean wears(Loot k) { return k == Loot.SWORDS; }

        // what it does, in the words of its slot: a sword's strength, what an Other thing acts as, armour's defence; shields: no number of their own
        private String rowStat(Loot k, String id) {
            if (k == Loot.SWORDS && Loot.OTHER.equals(k.category(a, id))) return a.getString(ACTS[Loot.action(a, id)]);
            if (k == Loot.SWORDS) return a.getString(R.string.loot_strength, effective(k, id));
            if (k.armor >= 0) return a.getString(R.string.loot_defence, effective(k, id));
            return a.getString(RARITY[k.rarity(id)]);
        }

        // with the slot's colour tier (Mods), as the game gets it
        private int effective(Loot k, String id) { return k.power(id) * Loot.TIER_POWER[file > 0 ? k.tier(a, file) : 0] / 100; }

        // hits left, of how many: "150/150", or with the word
        private String durability(Loot k, String id, boolean word) {
            final int total = k.uses(id), left = file > 0 && k.broken(a, file, id) ? 0 : Math.max(0, total - (file > 0 ? k.wear(a, file, id) : 0));
            return word ? a.getString(R.string.loot_hits, left, total) : left + "/" + total;
        }

        // everything about the thing, for the line under the slots
        private String detail(Loot k, String id) {
            final StringBuilder b = new StringBuilder(a.getString(RARITY[k.rarity(id)]));
            if (k == Loot.SWORDS && Loot.OTHER.equals(k.category(a, id))) b.append("  ·  ").append(a.getString(R.string.loot_acts, a.getString(ACTS[Loot.action(a, id)])));
            else if (k == Loot.SWORDS || k.armor >= 0) b.append("  ·  ").append(rowStat(k, id));
            b.append("  ·  ").append(wears(k) ? durability(k, id, true) : a.getString(R.string.loot_no_wear));
            return b.toString();
        }

        private String cut(String t, float maxW, float h) {
            if (draw.msgWidth(t, h) <= maxW) return t;
            int n = t.length();
            while (n > 1 && draw.msgWidth(t.substring(0, n) + "…", h) > maxW) n--;
            return t.substring(0, n) + "…";
        }

        // a slot per kind of the page: on Weapons side by side the whole width, on Gear beside the window
        private void drawSlots(Canvas c) {
            int n = page == GEAR ? 3 : 2;
            for (int i = 0; i < slots.length; i++) slots[i].setEmpty();
            float l = page == GEAR ? 325 : 30, r = 555, h = page == GEAR ? 130 : 100, gap = 20, top = page == GEAR ? 200 : 150;
            for (int i = 0; i < n; i++) {
                RectF b = box(l, top - i * (h + gap), r, top - i * (h + gap) - h);
                slots[i].set(b);
                boolean target = i == sub[page] && (dragRow >= 0 || selected >= 0);
                fill.setColor(target ? 0xE0404040 : 0xC8303030);
                c.drawRoundRect(b, 16 * s, 16 * s, fill);
                if (target) {
                    fill.setStyle(Paint.Style.STROKE);
                    fill.setStrokeWidth(5 * s);
                    fill.setColor(0xC800D7FF);
                    c.drawRoundRect(b.left + 3 * s, b.top + 3 * s, b.right - 3 * s, b.bottom - 3 * s, 14 * s, 14 * s, fill);
                    fill.setStyle(Paint.Style.FILL);
                }
                Loot k = PAGES[page][i];
                draw.title(c, a.getString(WORDS[page][i]), b.centerX(), b.top + 30 * s, 36 * s, 0xFFFFFFFF, 0xFF1E1E1E);
                final String on = k.get(a, file);
                draw.msg(c, cut(k.name(a, on), b.width() - 24 * s, 30 * s), b.centerX(), b.bottom - (on == null ? 36 : 44) * s, 30 * s, 0xFFFFFFFF, 0);
                if (on != null) {  // (what it is worth, as worn)
                    String stat = rowStat(k, on);
                    if (wears(k)) stat += "  ·  " + durability(k, on, true);
                    draw.msg(c, cut(stat, b.width() - 24 * s, 24 * s), b.centerX(), b.bottom - 18 * s, 24 * s, 0xFFFFE9A0, 0);
                }
            }
            for (RectF q : quicks) q.setEmpty();
            if (other()) drawQuicks(c, l, r);
        }

        // the buttons a weapon can go on, in the game's order
        private void drawQuicks(Canvas c, float l, float r) {
            final float w = (r - l - 2 * 14) / 3;
            for (int place = 0; place < 3; place++) {
                int button = QUICK_ORDER[place];
                RectF b = box(l + place * (w + 14), -90, l + place * (w + 14) + w, -178);
                quicks[button].set(b);
                boolean target = !all && (dragRow >= 0 || selected >= 0);
                fill.setColor(target ? 0xE0404040 : 0xC8303030);
                c.drawRoundRect(b, 14 * s, 14 * s, fill);
                if (target) {
                    fill.setStyle(Paint.Style.STROKE);
                    fill.setStrokeWidth(4 * s);
                    fill.setColor(0xC800D7FF);
                    c.drawRoundRect(b.left + 2 * s, b.top + 2 * s, b.right - 2 * s, b.bottom - 2 * s, 12 * s, 12 * s, fill);
                    fill.setStyle(Paint.Style.FILL);
                }
                String name = a.lootQuick.name(file, button);
                draw.title(c, QUICK_WORDS[place], b.centerX(), b.top + 24 * s, 34 * s, 0xFFFFFFFF, 0xFF1E1E1E);
                draw.msg(c, name == null ? "-" : cut(name, b.width() - 16 * s, 26 * s), b.centerX(), b.bottom - 24 * s, 26 * s, 0xFFFFFFFF, 0);
            }
        }

        // ---------------------------------------------------------------- input
        void step(int by) {
            if (ids.isEmpty()) return;
            setCursor(cursor + by);
            invalidate();
        }

        void choose() {
            if (ids.isEmpty()) return;
            if (all) toggleOwned(cursor);
            else putOn(cursor);
        }

        @SuppressLint("ClickableViewAccessibility")
        @Override
        public boolean onTouchEvent(MotionEvent e) {
            float x = e.getX(), y = e.getY();
            switch (e.getActionMasked()) {
                case MotionEvent.ACTION_DOWN:
                    downX = lastX = x;
                    downY = lastY = y;
                    moved = scrolling = false;
                    swiping = holeOn() && hole.contains(x, y);
                    downRow = cardAt(x, y);
                    dragRow = -1;
                    flingV = 0;
                    lastMoveAt = e.getEventTime();
                    removeCallbacks(lift);
                    if (downRow >= 0 && !all) postDelayed(lift, LIFT_MS);
                    return true;
                case MotionEvent.ACTION_MOVE:
                    float slop = 8 * dp, dx = x - downX, dy = y - downY;
                    if (swiping) {
                        Native.cameraSwipe((x - lastX) / dp, (y - lastY) / dp);
                    } else if (!moved && Math.hypot(dx, dy) > slop) {
                        moved = true;
                        removeCallbacks(lift);
                        if (downRow == cursor && !all && Math.abs(dy) > Math.abs(dx)) dragRow = downRow;  // (the middle one, pulled up or down)
                        else if (list.contains(downX, downY) && Math.abs(dx) >= Math.abs(dy)) scrolling = true;
                    }
                    if (dragRow >= 0) {
                        dragX = x;
                        dragY = y;
                        invalidate();
                    } else if (scrolling) {
                        final float by = -(x - lastX) / (STEP_1 * s);
                        final float dtm = Math.max(1, e.getEventTime() - lastMoveAt) / 1000f;
                        flingV = flingV * 0.6f + by / dtm * 0.4f;
                        lastMoveAt = e.getEventTime();
                        pos = Math.max(0, Math.min(ids.size() - 1, pos + by));
                        setCursor(Math.round(pos));
                        invalidate();
                    }
                    lastX = x;
                    lastY = y;
                    return true;
                case MotionEvent.ACTION_UP:
                    removeCallbacks(lift);
                    if (scrolling) {
                        scrolling = false;
                        if (e.getEventTime() - lastMoveAt < 80) setCursor(Math.round(pos + flingV * 0.22f));
                        invalidate();
                    }
                    if (dragRow >= 0) {
                        int row = dragRow;
                        dragRow = -1;
                        if (slots[sub[page]].contains(x, y)) putOn(row);
                        else for (int i = 0; i < quicks.length; i++)
                            if (quicks[i].contains(x, y)) toQuick(i, row);
                        invalidate();
                    } else if (!moved && !swiping) tap(x, y);
                    return true;
                case MotionEvent.ACTION_CANCEL:
                    removeCallbacks(lift);
                    scrolling = false;
                    dragRow = -1;
                    invalidate();
                    return true;
                default:
                    return true;
            }
        }

        // held on a card: lifted, to be dragged to its slot
        private final Runnable lift = () -> {
            if (moved || downRow < 0) return;
            if (downRow != cursor) setCursor(downRow);
            dragRow = cursor;
            dragX = lastX;
            dragY = lastY;
            moved = true;
            MenuSounds.play(a, MenuSounds.SWITCH);
            invalidate();
        };

        private void tap(float x, float y) {
            if (close.contains(x, y)) {
                dismiss();
                return;
            }
            for (int i = 0; i < tabs.length; i++)
                if (tabs[i].contains(x, y) && i != page) {
                    setPage(i);
                    return;
                }
            for (int i = 0; i < PAGES[page].length; i++)
                if (subs[i].contains(x, y) && i != sub[page]) {
                    setSub(i);
                    return;
                }
            if (allBtn.contains(x, y)) {
                all = !all;
                MenuSounds.play(a, MenuSounds.SWITCH);
                rebuild();
                return;
            }
            for (int i = 0; i < quicks.length; i++)
                if (quicks[i].contains(x, y)) {
                    if (selected >= 0 && !all) toQuick(i, selected);
                    else if (a.lootQuick.name(file, i) != null) {
                        a.lootQuick.set(file, i, null, null);
                        MenuSounds.play(a, MenuSounds.CANCEL);
                        invalidate();
                    }
                    return;
                }
            for (int i = 0; i < PAGES[page].length; i++)
                if (slots[i].contains(x, y)) {
                    if (i != sub[page]) {
                        setSub(i);
                        return;
                    }
                    if (selected >= 0) putOn(selected);
                    else takeOff(i);
                    return;
                }
            if (arrowL.contains(x, y)) {
                step(-1);
                return;
            }
            if (arrowR.contains(x, y)) {
                step(1);
                return;
            }
            int r = cardAt(x, y);
            if (r < 0) return;
            if (r != cursor) {  // (one at the side: it comes to the middle)
                setCursor(r);
                invalidate();
                return;
            }
            if (all) toggleOwned(r);
            else {
                selected = r == selected ? -1 : r;
                MenuSounds.play(a, MenuSounds.CURSOR);
                invalidate();
            }
        }
    }
}
