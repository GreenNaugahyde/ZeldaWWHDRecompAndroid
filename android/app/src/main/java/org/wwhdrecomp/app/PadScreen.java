package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.MotionEvent;
import android.view.View;

import java.util.ArrayList;
import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The GamePad screen drawn on the GamePad device itself (picture "Drawn here"): no video, only the game's state comes
 * (PadLink.GAME_STATE: the item ring's data, the touch state, the save data, the mini map's state), the names once
 * (PadLink.LABELS: in the game's words and language) and the game's textures and fonts once each from the console's
 * game files (GameIcons / GameFont from the console). It is drawn as the game draws its GamePad menus, at the places
 * of their layouts (MenuItem_00, MenuBtns_00, IconSlideArea_00; 1280 x 720 layout units, centre origin, y up):
 * <ul>
 * <li>The tabs ITEMS and MAP over the paper sheet (MenuBtns_00: blue / orange, the chosen one lit and larger).</li>
 * <li>Items: the item page's panels and items where the game has them, the Y / X / R bars above (IconSlideArea_00:
 *     what is on them), the Save / Options buttons in their frame. An item tapped (the game's cursor on it, its name
 *     above) and then a bar tapped or the GamePad device's X / Y / R pressed, or an item dragged onto a bar, goes on
 *     it (PadLink.EQUIP). A bag opens its eight items. Save: the game's own Save (PadLink.SAVE_GAME); Options: the
 *     console's options (PadLink.OPTIONS, drawn here by UiDraw). The arrow (or a swipe) turns to the collection.</li>
 * <li>Collection: the file's progress (GameProgress over the save data) on the game's caption box and panels: each
 *     part with its icons, what is had lit; a tapped icon's name in the caption box.</li>
 * <li>Map: the sea chart as the game's GamePad map draws it (its islands where the chart is filled in, the King of Red
 *     Lions, Link and the way he faces), the wind on the compass; a square tapped: its name and island beside.</li>
 * </ul>
 * Touches come from PadActivity's Controls (beside its buttons), in this view's coordinates.
 */
final class PadScreen extends View {
    interface Link { void post(byte type, byte[] body); }

    private static final int[] BAG_SLOTS = {4, 11, 18}, BAG_FIRST = {24, 36, 48};
    private static final int EMPTY = 0xFF;
    private static final int PAGE_ITEMS = 0, PAGE_COLLECTION = 1, PAGE_MAP = 2;

    // the item page's slots (MenuItem_00 L_BtnItemIcon_00..18: their places) -> inventory slots (GameCube order;
    // aspect.cpp kItemPageSlot, but the Spoils Bag top and the Delivery Bag below as the game's screen shows them);
    // the sail and the Wind Waker are not on the page
    private static final int[] PAGE_SLOT = {0, 4, 3, 6, 13, 14, 15, 7, 11, 5, 12, 19, 16, 17, 8, 18, 20, 9, 10};
    private static final float[][] PAGE_AT = {{-154, 38}, {54, 38}, {216, 38}, {344, 38}, {472, 38}, {-481, -98}, {-353, -98},
            {-208, -98}, {-8, -98}, {172, -98}, {300, -98}, {428, -98}, {-443, -234}, {-315, -234}, {-150, -234}, {50, -234},
            {220, -234}, {348, -234}, {476, -234}};
    // an open bag's items (L_BtnItemIcon_19..26 around W_BagItemBase_00 at 313, -92) and its Back button
    private static final float[][] BAG_AT = {{-131, 130}, {-175, -6}, {-127, -142}, {-3, 130}, {-47, -6}, {1, -142}, {125, 130}, {81, -6}};
    private static final float BAG_X = 313, BAG_Y = -92;
    // the panels behind the item groups (W_ItemBase_00..03: centre, size, caps)
    private static final float[][] PANELS = {{347, -92, 478, 414}, {44, -92, 300, 414}, {-160, -92, 284, 414}, {-380, -191, 330, 216}};
    private static final String[][] PANEL_CAPS = {{"ItemBackBase_00^t", null}, {"ItemBackBase_00^t", "ItemBackBase_01^t"},
            {"ItemBackBase_00^t", "ItemBackBase_01^t"}, {"ItemBackBase_02^t", null}};
    // the Y / X / R bars (IconSlideArea_00, as the game's screen shows them): centres, the button's mark in each
    private static final float BAR_Y = 171, BAR_W = 260, BAR_H = 106;
    private static final float[] BAR_X = {-87, 198, 483}, MARK_DX = {-66, -66, -54};
    private static final int[] BAR_BUTTON = {1, 0, 2};  // Y, X, R -> the item ring's button (0 X, 1 Y, 2 R)
    private static final String[] BAR_LETTER = {"Y", "X", "R"};
    // the sheet (W_Base_00 as on the screen), the Save / Options frame and buttons, the page arrows
    private static final float SHEET_L = -617, SHEET_T = 300, SHEET_R = 585, SHEET_B = -300;
    private static final float FRAME_X = -399, FRAME_Y = 110, FRAME_W = 310, FRAME_H = 226;
    private static final float SAVE_Y = 152, OPTIONS_Y = 68, BUTTON_W = 244, BUTTON_H = 56;
    private static final float ARROW_X = 578, ARROW_Y = 15;
    // the map: the chart's square (7 x 7) and the panel beside it
    private static final float CHART_X = -120, CHART_Y = -22, CHART = 480;
    private static final float SIDE_X = 382, SQUARE = 100000f;

    private final Link link;
    private int[] ring;      // Native.itemRing there (null: none yet), -1s outside the game
    private int[] touchSt;   // Native.touchState there
    private float[] map;     // Native.miniMapState there (rounded)
    private byte[] save;
    private GameProgress.Result progress;
    private final Map<String, String> names = new HashMap<>(), ui = new HashMap<>();
    private final GameProgress.Labels labels;

    private int page;          // PAGE_*
    private int bag = -1;      // the bag open (0..2), -1 the items
    private int selected = -1; // the slot chosen (waiting for a button)
    private float scroll;      // collection page
    private String caption;    // the collection's tapped icon: its name
    private int square = -1;   // the map's chosen square (row * 7 + column), -1: Link's

    // the places of the last drawing (for touches)
    private final RectF[] tabs = {new RectF(), new RectF()};  // items, map
    private final List<RectF> cellRects = new ArrayList<>();
    private final List<Integer> cellSlots = new ArrayList<>();
    private final List<RectF> iconRects = new ArrayList<>();
    private final List<String> iconNames = new ArrayList<>();
    private final RectF[] bars = {new RectF(), new RectF(), new RectF()};
    private final RectF backRect = new RectF(), saveRect = new RectF(), optionsRect = new RectF(), arrowRect = new RectF();
    private final RectF chartRect = new RectF(), listRect = new RectF();
    private float contentH;

    private final Paint fill = new Paint(Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final Paint bmp = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Paint glyphs = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final float dp;
    private final Runnable redraw = () -> post(this::invalidate);

    // the layout's place on the view: its centre and pixels per layout unit
    private float cx, cy, s;
    private final RectF frame = new RectF();

    PadScreen(Context c, Link link) {
        super(c);
        this.link = link;
        dp = c.getResources().getDisplayMetrics().density;
        text.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        labels = padLabels(c);
    }

    private GameProgress.Labels padLabels(Context c) {
        final int[] parts = {R.string.progress_part_dungeons, R.string.progress_part_hearts, R.string.progress_part_items,
                R.string.progress_part_equipment, R.string.progress_part_upgrades, R.string.progress_part_songs,
                R.string.progress_part_quest, R.string.progress_part_charts, R.string.progress_part_sea,
                R.string.progress_part_figurines};
        return new GameProgress.Labels() {
            public String section(int part) { return name(PadLink.LABEL_SECTION, part, c.getString(parts[part])); }
            public String item(int id) { return name(PadLink.LABEL_ITEM, id, ""); }
            public String dungeon(int n) { return name(PadLink.LABEL_DUNGEON, n, ""); }
            public String island(int room) { return name(PadLink.LABEL_ISLAND, room, ""); }
            public String song(int melody) { return name(PadLink.LABEL_SONG, melody, ""); }
            public String figurine(int n) { return name(PadLink.LABEL_FIGURINE, n, ""); }
            public String own(int which) { return name(PadLink.LABEL_OWN, which, ""); }
        };
    }

    private String name(int kind, int key, String otherwise) {
        String n = names.get(kind + ":" + key);
        return n != null && !n.isEmpty() ? n : otherwise;
    }

    /** a word of the game's menus ("<layout>:<pane>"), its tags left out, else otherwise */
    private String word(String key, String otherwise) {
        String w = ui.get(key);
        if (w == null) return otherwise;
        w = w.replaceAll("\\{[^}]*\\}", "").replace("\\n", " ").trim();
        return w.isEmpty() ? otherwise : w;
    }

    // ---------------------------------------------------------------- state from the console (main thread)
    void state(int[] ring, int[] touch, byte[] save, float[] map) {
        if (ring != null) this.ring = ring;
        if (touch != null) touchSt = touch;
        if (map != null) this.map = map;
        if (save != null) {
            this.save = save;
            recount();
        }
        if (!inGame()) {
            bag = -1;
            selected = -1;
        }
        invalidate();
    }

    private void recount() {
        try {
            progress = save != null && save.length > 0x79C ? GameProgress.count(save, labels) : null;
        } catch (RuntimeException e) {
            progress = null;
        }
    }

    /** the names in the game's words (PadLink.LABELS) */
    void labels(Map<String, String> l) {
        names.clear();
        ui.clear();
        for (Map.Entry<String, String> e : l.entrySet()) {
            if (e.getKey().startsWith(PadLink.LABEL_UI + ":")) {
                int tab = e.getValue().indexOf('\t');
                if (tab > 0) ui.put(e.getValue().substring(0, tab), e.getValue().substring(tab + 1));
            } else names.put(e.getKey(), e.getValue());
        }
        recount();
        invalidate();
    }

    private Bitmap indoor;  // the map indoors (PadLink.INDOOR_MAP), null: none

    /** the map indoors drawn by the console (null: none) */
    void indoor(Bitmap b) {
        indoor = b;
        if (page == PAGE_MAP) invalidate();
    }

    /** a new connection: everything anew */
    void clear() {
        ring = touchSt = null;
        map = null;
        save = null;
        progress = null;
        bag = selected = -1;
        caption = null;
        square = -1;
        indoor = null;
        tinted.clear();
        invalidate();
    }

    private boolean inGame() { return ring != null && ring.length >= 49 && ring[21] != -1; }

    /** shows a page (PadActivity's "page" extra: 0 items, 1 collection, 2 map) */
    void showPage(int p) {
        if (p >= PAGE_ITEMS && p <= PAGE_MAP) turn(p);
    }

    /** an item has been chosen on the items page (the device's X / Y / R put it on: equipFromButton) */
    boolean choosing() { return page == PAGE_ITEMS && selected >= 0 && getVisibility() == VISIBLE; }

    /** the GamePad device's X / Y / R pressed while an item is chosen: it goes on that button (true: taken) */
    boolean equipFromButton(int button) {
        if (!choosing()) return false;
        equip(button, selected);
        return true;
    }

    // ---------------------------------------------------------------- textures (from the console, GameIcons)
    private Bitmap tex(String name) { return GameIcons.get(getContext(), name, redraw); }

    private final Map<String, Bitmap> tinted = new HashMap<>();

    /** a texture as a layout material draws it: its grey from the black to the white colour (MiniMap.tint) */
    private Bitmap tint(String name, int black, int white) {
        String key = name + "#" + Integer.toHexString(black) + Integer.toHexString(white);
        Bitmap b = tinted.get(key);
        if (b != null) return b;
        Bitmap src = tex(name);
        if (src == null) return null;
        b = MiniMap.tint(src, black, white, 0xFF);
        tinted.put(key, b);
        return b;
    }

    // ---------------------------------------------------------------- layout units -> the view
    private float X(float x) { return cx + x * s; }
    private float Y(float y) { return cy - y * s; }

    private RectF box(float x, float y, float w, float h) {
        return new RectF(X(x) - w * s / 2, Y(y) - h * s / 2, X(x) + w * s / 2, Y(y) + h * s / 2);
    }

    // ---------------------------------------------------------------- the game's fonts
    /** text in the game's text font (CKingMsg), h its cell height in pixels, centred on x (align 0), from x (-1) or
     *  ending at x (1), its cell's middle at y; the system's font until the font is here */
    private float msg(Canvas c, String t, float x, float y, float h, int color, int align) {
        GameFont f = GameFont.MSG;
        if (!f.isReady(getContext(), redraw)) {
            text.setColor(color);
            text.setTextSize(h * 0.62f);
            text.setTextAlign(align == 0 ? Paint.Align.CENTER : align < 0 ? Paint.Align.LEFT : Paint.Align.RIGHT);
            c.drawText(t, x, y + h * 0.22f, text);
            return text.measureText(t);
        }
        float k = h / f.height(), w = 0;
        text.setTextSize(h * 0.62f);
        for (int i = 0; i < t.length(); i++) w += f.has(t.charAt(i)) ? f.advance(t.charAt(i)) * k : text.measureText(String.valueOf(t.charAt(i)));
        float left = align == 0 ? x - w / 2 : align < 0 ? x : x - w, top = y - h / 2;
        glyphs.setColor(color);
        text.setColor(color);
        text.setTextAlign(Paint.Align.LEFT);
        for (int i = 0; i < t.length(); i++) {
            char ch = t.charAt(i);
            if (f.has(ch)) left += f.draw(c, ch, left, top, k, glyphs);
            else {
                String one = String.valueOf(ch);
                c.drawText(one, left, top + f.baselinePx() * k, text);
                left += text.measureText(one);
            }
        }
        return w;
    }

    /** the width msg would take */
    private float msgWidth(String t, float h) {
        GameFont f = GameFont.MSG;
        if (!f.isReady(getContext(), redraw)) {
            text.setTextSize(h * 0.62f);
            return text.measureText(t);
        }
        float k = h / f.height(), w = 0;
        text.setTextSize(h * 0.62f);
        for (int i = 0; i < t.length(); i++) w += f.has(t.charAt(i)) ? f.advance(t.charAt(i)) * k : text.measureText(String.valueOf(t.charAt(i)));
        return w;
    }

    /** a label in the game's title font with its outline (CKingMain: tabs, counts), centred at x, y, cell height h */
    private void title(Canvas c, String t, float x, float y, float h, int fillColor, int outline) {
        if (GameFont.ready(getContext(), redraw) && GameFont.covers(t)) {
            float k = h / GameFont.cellHeight();
            Bitmap b = GameFont.label(t, k, fillColor, outline);
            if (b != null) {
                c.drawBitmap(b, x - b.getWidth() / 2f, y - b.getHeight() / 2f, bmp);
                return;
            }
        }
        text.setTextSize(h * 0.6f);
        text.setTextAlign(Paint.Align.CENTER);
        text.setStyle(Paint.Style.STROKE);
        text.setStrokeWidth(h * 0.1f);
        text.setColor(outline);
        c.drawText(t, x, y + h * 0.21f, text);
        text.setStyle(Paint.Style.FILL);
        text.setColor(fillColor);
        c.drawText(t, x, y + h * 0.21f, text);
    }

    // ---------------------------------------------------------------- drawing
    private GameUi.GameBackground background;
    private GameUi.GamePaper paper;
    private float paperUnit;

    @Override
    protected void onDraw(Canvas c) {
        int w = getWidth(), h = getHeight();
        if (w == 0 || h == 0) return;
        // the layout in the part the touch buttons leave free (face buttons right, d-pad bottom left, the top row)
        boolean buttons = sideL > 0.05f;
        float l = w * sideL, r = w * sideR, t = buttons ? h * 0.115f : h * 0.02f, b = buttons ? h * 0.875f : h * 0.98f;
        // (fitted by what is drawn: the sheet and the arrows across, the tabs' tops to the sheet's foot)
        s = Math.min((r - l) / 1250f, (b - t) / 670f);
        cx = (l + r) / 2 + 16 * s;
        cy = (t + b) / 2 + 26 * s;
        frame.set(cx - 640 * s, cy - 360 * s, cx + 640 * s, cy + 360 * s);
        if (background == null) background = new GameUi.GameBackground(getContext(), s);
        background.setBounds(0, 0, w, h);
        background.draw(c);
        if (paper == null || paperUnit != s) {
            paper = new GameUi.GamePaper(getContext(), s);
            paperUnit = s;
        }
        Rect sheet = new Rect(Math.round(X(SHEET_L)), Math.round(Y(SHEET_T)), Math.round(X(SHEET_R)), Math.round(Y(SHEET_B)));
        paper.setBounds(sheet);
        paper.draw(c);
        cellRects.clear();
        cellSlots.clear();
        iconRects.clear();
        iconNames.clear();
        if (!inGame()) {
            msg(c, getContext().getString(R.string.pad_screen_wait), X(0), Y(0), 44 * s, GameUi.OPTION_TEXT, 0);
        } else if (page == PAGE_ITEMS) drawItems(c);
        else if (page == PAGE_COLLECTION) drawCollection(c);
        else drawMap(c);
        drawTabs(c);
    }

    // the part of the screen left free by the touch buttons (face buttons right, d-pad bottom left), as fractions
    private float sideL = 0.13f, sideR = 0.75f;

    /** the free part's left and right edges (0..1): narrower while the touch buttons are shown */
    void setSides(float l, float r) {
        if (l == sideL && r == sideR) return;
        sideL = l;
        sideR = r;
        invalidate();
    }

    // MenuBtns_00: ITEMS blue, MAP orange (the game's BOTTLES has no use here)
    private void drawTabs(Canvas c) {
        String[] words = {word("MenuBtns_00:L_BtnMenu_00-T_Btn_00", getContext().getString(R.string.pad_screen_items)),
                word("MenuBtns_00:L_BtnMenu_01-T_Btn_00", getContext().getString(R.string.pad_screen_map))};
        GameUi.Palette[] pals = {GameUi.BLUE, GameUi.AMBER};
        int[] outlines = {0xFF003192, 0xFFB2410A};
        for (int i = 0; i < 2; i++) {
            boolean on = i == 0 ? page != PAGE_MAP : page == PAGE_MAP;
            float x = -462 + i * 305, tw = on ? 290 : 256, th = on ? 122 : 106, top = on ? 350 : 352;
            RectF r = new RectF(X(x - tw / 2), Y(top), X(x + tw / 2), Y(top - th));
            tabs[i].set(r);
            GameUi.GameTabPlate p = new GameUi.GameTabPlate(getContext(), on, pals[i], 2 * s);
            p.setBounds(Math.round(r.left), Math.round(r.top), Math.round(r.right), Math.round(r.bottom));
            p.draw(c);
            title(c, words[i], r.centerX(), r.centerY() - th * 0.04f * s, th * (on ? 0.44f : 0.40f) * s, 0xFFFFFFFF, outlines[i]);
        }
    }

    // ---- the items page
    private void drawItems(Canvas c) {
        // the panels behind the item groups: navy over the paper, their edges brown, faint (alpha 60)
        bmp.setAlpha(60);
        for (int i = 0; i < PANELS.length; i++) {
            float[] p = PANELS[i];
            capWindow(c, box(p[0], p[1], p[2], p[3]), tint(PANEL_CAPS[i][0], 0xFF9C6D20, 0xFF000B3B),
                    PANEL_CAPS[i][1] == null ? null : tint(PANEL_CAPS[i][1], 0xFF9C6D20, 0xFF000B3B));
        }
        bmp.setAlpha(255);
        drawSaveFrame(c);
        drawBars(c);
        if (bag < 0) {
            for (int i = 0; i < PAGE_SLOT.length; i++) {
                int slot = PAGE_SLOT[i];
                drawItem(c, PAGE_AT[i][0], PAGE_AT[i][1], ring[slot], slot);
            }
        } else drawBag(c);
        // the arrow to the collection
        GameUi.MenuArrow a = new GameUi.MenuArrow(getContext(), false);
        arrowRect.set(box(ARROW_X, ARROW_Y, 70, 124));
        a.setBounds(Math.round(arrowRect.left), Math.round(arrowRect.top), Math.round(arrowRect.right), Math.round(arrowRect.bottom));
        a.draw(c);
        if (selected >= 0) drawChosen(c);
    }

    /** a window of three parts across: its left cap, the middle (the cap's last column stretched), the right cap (rt,
     *  or lt mirrored); the caps as tall as the window */
    private void capWindow(Canvas c, RectF r, Bitmap lt, Bitmap rt) {
        if (lt == null) return;
        float k = r.height() / lt.getHeight(), cw = Math.min(lt.getWidth() * k, r.width() / 2);
        c.drawBitmap(lt, null, new RectF(r.left, r.top, r.left + cw, r.bottom), bmp);
        Bitmap right = rt != null ? rt : lt;
        float rw = Math.min(right.getWidth() * k, r.width() / 2);
        if (r.right - rw > r.left + cw)
            c.drawBitmap(lt, new Rect(lt.getWidth() - 1, 0, lt.getWidth(), lt.getHeight()), new RectF(r.left + cw - 0.5f, r.top, r.right - rw + 0.5f, r.bottom), bmp);
        c.save();
        if (rt == null) {  // (mirrored)
            c.scale(-1, 1, r.right - rw / 2, 0);
        }
        c.drawBitmap(right, null, new RectF(r.right - rw, r.top, r.right, r.bottom), bmp);
        c.restore();
    }

    // the Save / Options frame (W_InFrame_00: notched paper, darker) and its buttons (BtnMenuSaveOption_00, blue here)
    private void drawSaveFrame(Canvas c) {
        RectF r = box(FRAME_X, FRAME_Y, FRAME_W, FRAME_H);
        String[] pieces = {"LT", "RT", "LB", "RB", "L", "R", "T", "B"};
        Bitmap[] bs = new Bitmap[8];
        boolean all = true;
        for (int i = 0; i < 8; i++) all &= (bs[i] = tint("NotchedWindow" + pieces[i] + "_00^t", 0xFF857D50, 0xFFAEA992)) != null;
        Bitmap pat = tint("PaperPattern_00^t", 0xFF857D50, 0xFFAEA992);
        float f = 30 * s;
        if (all) {
            RectF[] at = {new RectF(r.left, r.top, r.left + f, r.top + f), new RectF(r.right - f, r.top, r.right, r.top + f),
                    new RectF(r.left, r.bottom - f, r.left + f, r.bottom), new RectF(r.right - f, r.bottom - f, r.right, r.bottom),
                    new RectF(r.left, r.top + f, r.left + f, r.bottom - f), new RectF(r.right - f, r.top + f, r.right, r.bottom - f),
                    new RectF(r.left + f, r.top, r.right - f, r.top + f), new RectF(r.left + f, r.bottom - f, r.right - f, r.bottom)};
            for (int i = 0; i < 8; i++) c.drawBitmap(bs[i], null, at[i], bmp);
            RectF in = new RectF(r.left + f - 0.5f, r.top + f - 0.5f, r.right - f + 0.5f, r.bottom - f + 0.5f);
            if (pat != null) {
                android.graphics.BitmapShader sh = new android.graphics.BitmapShader(pat, android.graphics.Shader.TileMode.REPEAT, android.graphics.Shader.TileMode.REPEAT);
                android.graphics.Matrix m = new android.graphics.Matrix();
                m.setScale(2 * s, 2 * s);
                sh.setLocalMatrix(m);
                fill.setShader(sh);
                c.drawRect(in, fill);
                fill.setShader(null);
            } else {
                fill.setColor(0xFFAEA992);
                c.drawRect(in, fill);
            }
        } else {
            fill.setColor(0xFFAEA992);
            c.drawRect(r, fill);
        }
        String sv = word("MenuItem_00:L_MenuSave_00-T_Btn_00", getContext().getString(R.string.pad_screen_save)),
                op = word("MenuItem_00:L_MenuOption_00-T_Btn_00", getContext().getString(R.string.pad_screen_options_button));
        saveRect.set(box(FRAME_X + 2, SAVE_Y, BUTTON_W, BUTTON_H));
        optionsRect.set(box(FRAME_X + 2, OPTIONS_Y, BUTTON_W, BUTTON_H));
        for (int i = 0; i < 2; i++) {
            RectF br = i == 0 ? saveRect : optionsRect;
            boolean lit = pressed == (i == 0 ? saveRect : optionsRect);
            GameUi.GameRowPlate plate = new GameUi.GameRowPlate(getContext(), lit ? 0xFF1E8CC8 : 0xFF2A9AD6, lit ? 0xFFC8F0FF : 0xFF8AD8F8, s);
            plate.setBounds(Math.round(br.left), Math.round(br.top), Math.round(br.right + 6 * s), Math.round(br.bottom + 8 * s));
            plate.draw(c);
            msg(c, i == 0 ? sv : op, br.centerX(), br.centerY(), 40 * s, 0xFF00141E, 0);
        }
    }

    // the Y / X / R bars (W_*DropSlide_00: dark, alpha 200; the button's mark ABXY / R; the item on it)
    private void drawBars(Canvas c) {
        Bitmap cap = tex("DropSlideL_00^t"), round = tex("ABXY^f"), rmark = tex("R^f");
        for (int i = 0; i < 3; i++) {
            RectF r = box(BAR_X[i], BAR_Y, BAR_W, BAR_H);
            bars[i].set(r);
            boolean target = selected >= 0 || dragSlot >= 0;
            bmp.setAlpha(target ? 235 : 200);
            if (cap != null) capWindow(c, r, cap, null);
            else {
                fill.setColor(0xC8303030);
                c.drawRoundRect(r, 16 * s, 16 * s, fill);
            }
            bmp.setAlpha(255);
            if (target) {  // (where the chosen item can go: the game's float glow, cyan)
                fill.setStyle(Paint.Style.STROKE);
                fill.setStrokeWidth(5 * s);
                fill.setColor(0xC800D7FF);
                c.drawRoundRect(r.left + 3 * s, r.top + 3 * s, r.right - 3 * s, r.bottom - 3 * s, 20 * s, 20 * s, fill);
                fill.setStyle(Paint.Style.FILL);
            }
            float mx = BAR_X[i] + MARK_DX[i];
            Bitmap m = i == 2 ? rmark : round;
            RectF mr = i == 2 ? box(mx, BAR_Y, 102, 57) : box(mx, BAR_Y, 78, 78);
            if (m != null) c.drawBitmap(m, null, mr, bmp);
            msg(c, BAR_LETTER[i], mr.centerX() - (i == 2 ? 6 * s : 0), mr.centerY() + 2 * s, 46 * s, 0xFF1E1E1E, 0);
            int slot = ring[21 + BAR_BUTTON[i]];
            int item = itemAt(slot);
            if (item >= 0 && item != EMPTY && slot != dragSlot) {
                Bitmap ib = tex(GameIcons.itemTexture(item));
                if (ib != null) c.drawBitmap(ib, null, box(BAR_X[i] + 40, BAR_Y, 90, 90), bmp);
                drawCount(c, slot, BAR_X[i] + 40 + 32, BAR_Y - 26, 0.8f);
            }
        }
    }

    /** an item at (x, y) (layout units): the yellow glow when it is on X / Y / R, its icon, its count */
    private void drawItem(Canvas c, float x, float y, int item, int slot) {
        if (item < 0 || item == EMPTY) return;
        RectF r = box(x, y, 128, 128);
        boolean on = false;
        for (int b = 0; b < 3; b++) on |= ring[21 + b] == slot;
        if (on) {
            Bitmap g = tint("ItemNow_00^t", 0xFF000000, 0xFFFFE100);
            if (g != null) {
                bmp.setAlpha(230);
                c.drawBitmap(g, null, box(x, y, 138, 138), bmp);
                bmp.setAlpha(255);
            }
        }
        cellRects.add(r);
        cellSlots.add(slot);
        if (slot == dragSlot) return;
        Bitmap b = tex(GameIcons.itemTexture(item));
        if (b != null) c.drawBitmap(b, null, r, bmp);
        drawCount(c, slot, x + 40, y - 37, 1f);
    }

    /** the number the game writes on an item (arrows on the bow, bombs; a bag's things) */
    private void drawCount(Canvas c, int slot, float x, float y, float k) {
        int n = count(slot);
        if (n < 0) return;
        title(c, String.valueOf(n), X(x), Y(y), 50 * k * s, 0xFFFFA81E, 0xFF3C1E00);
    }

    private int count(int slot) {
        if (save == null || save.length < 0xB4) return -1;
        int item = itemAt(slot);
        if (item < 0 || item == EMPTY) return -1;
        if (slot == 12 && (item == 0x27 || item == 0x35 || item == 0x36)) return save[0x69] & 0xFF;  // arrows
        if (slot == 13 && item == 0x31) return save[0x6A] & 0xFF;  // bombs
        for (int b = 0; b < 3; b++)  // a bag's things (dSv_player_bag_item_record_c: the spoils and the bait)
            if (b < 2 && slot >= BAG_FIRST[b] && slot < BAG_FIRST[b] + 8) return save[0x9C + b * 8 + slot - BAG_FIRST[b]] & 0xFF;
        return -1;
    }

    // an open bag: its panel over the right group (W_BagItemBase_00) with the eight things and the Back button
    private void drawBag(Canvas c) {
        for (int i = 0; i < PAGE_SLOT.length; i++) {  // (the other items stay, faint)
            int slot = PAGE_SLOT[i];
            int item = ring[slot];
            if (item < 0 || item == EMPTY || PAGE_AT[i][0] > 100) continue;
            Bitmap b = tex(GameIcons.itemTexture(item));
            bmp.setAlpha(110);
            if (b != null) c.drawBitmap(b, null, box(PAGE_AT[i][0], PAGE_AT[i][1], 128, 128), bmp);
            bmp.setAlpha(255);
        }
        Bitmap glow = tint("ItemBackBaseGlow_00^t", 0xFF000000, 0xFF737994);
        bmp.setAlpha(235);
        if (glow != null) capWindow(c, box(BAG_X, BAG_Y, 562, 494), glow, null);
        else {
            fill.setColor(0xEB737994);
            c.drawRect(box(BAG_X, BAG_Y, 562, 494), fill);
        }
        bmp.setAlpha(255);
        Bitmap bb = tex(GameIcons.itemTexture(ring[BAG_SLOTS[bag]]));
        if (bb != null) {
            bmp.setAlpha(90);
            c.drawBitmap(bb, null, box(BAG_X + 10, BAG_Y, 300, 300), bmp);
            bmp.setAlpha(255);
        }
        for (int i = 0; i < 8; i++) {
            int slot = BAG_FIRST[bag] + i;
            drawItem(c, BAG_X + BAG_AT[i][0], BAG_Y + BAG_AT[i][1], ring[25 + bag * 8 + i], slot);
        }
        // Back (BtnBack_00 at 129, -142 of the bag): the game's word on a paper button
        backRect.set(box(BAG_X + 129, BAG_Y - 142, 128, 70));
        GameUi.GameRowPlate p = new GameUi.GameRowPlate(getContext(), pressed == backRect, s);
        p.setBounds(Math.round(backRect.left), Math.round(backRect.top), Math.round(backRect.right + 6 * s), Math.round(backRect.bottom + 8 * s));
        p.draw(c);
        msg(c, word("MenuItem_00:L_BtnBack_00-T_Btn_00", getContext().getString(R.string.pad_screen_back)), backRect.centerX(),
                backRect.centerY(), 36 * s, 0xFF00141E, 0);
    }

    // the chosen item: the game's cursor around it, its name over it on the title band (W_TextBase: TitleBase)
    private void drawChosen(Canvas c) {
        RectF at = null;
        for (int i = 0; i < cellSlots.size(); i++) if (cellSlots.get(i) == selected) at = cellRects.get(i);
        if (at == null) return;
        RectF r = new RectF(at.left + 10 * s, at.top + 10 * s, at.right - 10 * s, at.bottom - 10 * s);
        if (GameUi.MenuCursor.draw(getContext(), c, r, 34 * s, redraw)) postInvalidateDelayed(250);
        String n = name(PadLink.LABEL_ITEM, itemAt(selected), "");
        String hint = word("MenuItem_00:T_TitleR_01_05", "");
        if (n.isEmpty()) return;
        float th = 40 * s, w = Math.max(msgWidth(n, th), hint.isEmpty() ? 0 : msgWidth(hint, th * 0.8f)) + 50 * s;
        float bx = Math.max(X(SHEET_L) + w / 2 + 10 * s, Math.min(X(SHEET_R) - w / 2 - 10 * s, at.centerX()));
        float by = at.top - (hint.isEmpty() ? 34 : 54) * s;
        RectF band = new RectF(bx - w / 2, by - (hint.isEmpty() ? 26 : 46) * s, bx + w / 2, by + 26 * s);
        Bitmap base = tint("TitleBase_00^t", 0xFF5B705D, 0xFFF2EFC4);
        fill.setColor(0xFFF2EFC4);
        if (base != null) c.drawBitmap(base, null, band, bmp);
        else c.drawRoundRect(band, 6 * s, 6 * s, fill);
        msg(c, n, bx, band.bottom - 26 * s, th, 0xFF1A291B, 0);
        if (!hint.isEmpty()) msg(c, hint, bx, band.top + 18 * s, th * 0.7f, 0xFF4A5A4B, 0);
    }

    /** the item id in an inventory / bag slot (bags: 24.., 36.., 48..), -1 none */
    private int itemAt(int slot) {
        if (slot < 0 || ring == null) return -1;
        if (slot < 21) return ring[slot];
        for (int b = 0; b < 3; b++)
            if (slot >= BAG_FIRST[b] && slot < BAG_FIRST[b] + 8) return ring[25 + b * 8 + slot - BAG_FIRST[b]];
        return -1;
    }

    // ---- the collection (the items' second page): the caption box above, the parts below on panels
    private void drawCollection(Canvas c) {
        GameUi.MenuArrow a = new GameUi.MenuArrow(getContext(), true);
        arrowRect.set(box(-ARROW_X, ARROW_Y, 70, 124));
        a.setBounds(Math.round(arrowRect.left), Math.round(arrowRect.top), Math.round(arrowRect.right), Math.round(arrowRect.bottom));
        a.draw(c);
        // the caption box (W_TextBase_01: TitleBase, yellow paper, the wind's swirls faint in it)
        RectF cap = box(20, 172, 1000, 104);
        fill.setColor(0xFFF2EFC4);
        c.drawRect(cap, fill);
        Bitmap deco = tex("DecoWind_00^t");
        if (deco != null) {
            bmp.setAlpha(30);
            bmp.setColorFilter(new android.graphics.PorterDuffColorFilter(0xFF0062B0, android.graphics.PorterDuff.Mode.SRC_IN));
            float dw = cap.height() * 1.2f * deco.getWidth() / deco.getHeight();
            c.save();
            c.clipRect(cap);
            for (float x = cap.left - dw * 0.3f; x < cap.right; x += dw) c.drawBitmap(deco, null, new RectF(x, cap.top - cap.height() * 0.1f, x + dw, cap.bottom + cap.height() * 0.1f), bmp);
            c.restore();
            bmp.setColorFilter(null);
            bmp.setAlpha(255);
        }
        fill.setStyle(Paint.Style.STROKE);
        fill.setStrokeWidth(6 * s);
        fill.setColor(0xFFB8BBA6);
        c.drawRect(cap, fill);
        fill.setStyle(Paint.Style.FILL);
        if (progress == null) return;
        String line = caption != null ? caption : getContext().getString(R.string.pad_screen_collection_hint, progress.percent());
        msg(c, line, cap.centerX(), cap.centerY(), 44 * s, 0xFF1A291B, 0);
        // the parts, scrolled: each part's title on its band, its icons on a panel
        listRect.set(X(-520), Y(108), X(SHEET_R - 30), Y(SHEET_B + 24));
        c.save();
        c.clipRect(listRect);
        float y = listRect.top - scroll, x0 = listRect.left, xr = listRect.right, size = 84 * s, gap = 10 * s;
        for (GameProgress.Section sec : progress.sections) {
            float th = 40 * s;
            // the title band (TitleBase_00 of the map's legend: a strip with a torn end)
            RectF band = new RectF(x0, y + 4 * s, x0 + msgWidth(sec.title, th) + 150 * s, y + 4 * s + 52 * s);
            Bitmap tb = tint("TitleBase_00^t", 0xFF5B705D, 0xFFD7D380);
            if (tb != null) c.drawBitmap(tb, null, band, bmp);
            else {
                fill.setColor(0xFFD7D380);
                c.drawRect(band, fill);
            }
            msg(c, sec.title, band.left + 18 * s, band.centerY(), th, 0xFF1A291B, -1);
            title(c, sec.done + " / " + sec.total, band.right - 60 * s, band.centerY(), 40 * s, sec.done >= sec.total ? 0xFFFFE100 : 0xFFFFFFFF, 0xFF3C3C1E);
            y = band.bottom + 8 * s;
            float rowTop = y, x = x0 + 12 * s;
            int rows = 1;
            for (GameProgress.Entry e : sec.entries)
                for (int k = 0; k < e.icons.length; k++) {
                    if (x + size > xr) {
                        x = x0 + 12 * s;
                        y += size + gap;
                        rows++;
                    }
                    x += size + gap;
                }
            // the panel behind the icons (the item page's navy, faint)
            fill.setColor(0x26000B3B);
            c.drawRoundRect(x0, rowTop - 4 * s, xr, rowTop + rows * (size + gap) + 4 * s, 18 * s, 18 * s, fill);
            x = x0 + 12 * s;
            y = rowTop;
            for (GameProgress.Entry e : sec.entries) {
                for (int k = 0; k < e.icons.length; k++) {
                    if (x + size > xr) {
                        x = x0 + 12 * s;
                        y += size + gap;
                    }
                    RectF ir = new RectF(x, y, x + size, y + size);
                    Bitmap b = tex(e.icons[k]);
                    boolean got = e.got[k] || (e.icons.length == 1 && e.done > 0);
                    if (b != null) {
                        bmp.setAlpha(got ? 255 : 70);
                        if (!got) bmp.setColorFilter(GREY);
                        c.drawBitmap(b, null, ir, bmp);
                        bmp.setColorFilter(null);
                        bmp.setAlpha(255);
                    } else {
                        fill.setColor(got ? 0x88F5DC4A : 0x22000B3B);
                        c.drawRoundRect(ir, 12 * s, 12 * s, fill);
                    }
                    if (e.icons.length == 1 && e.total > 1) title(c, e.done + "", ir.right - 16 * s, ir.bottom - 14 * s, 40 * s, 0xFFFFA81E, 0xFF3C1E00);
                    if (ir.bottom > listRect.top && ir.top < listRect.bottom) {
                        iconRects.add(ir);
                        iconNames.add(e.name + (e.total > 1 ? "  " + Math.min(e.done, e.total) + " / " + e.total : ""));
                    }
                    x += size + gap;
                }
            }
            y += size + gap + 14 * s;
        }
        contentH = y + scroll - listRect.top;
        c.restore();
    }

    private static final android.graphics.ColorMatrixColorFilter GREY = new android.graphics.ColorMatrixColorFilter(new float[] {
            0.3f, 0.5f, 0.2f, 0, 0, 0.3f, 0.5f, 0.2f, 0, 0, 0.3f, 0.5f, 0.2f, 0, 0, 0, 0, 0, 1, 0});

    // ---- the map: the sea chart (the game's GamePad map: its paper, the islands where the chart is filled in, the
    // grid; Link and the King of Red Lions), the chosen square's name and island beside, the wind's compass below
    private void drawMap(Canvas c) {
        if (map != null && map.length > 0 && map[0] == 2) {  // indoors: the console's map of the stage
            drawIndoor(c);
            return;
        }
        RectF ch = box(CHART_X, CHART_Y, CHART, CHART);
        chartRect.set(ch);
        float q = ch.width() / 7;
        // the paper: the cloth's grain in the chart's beige (MiniMap's colours)
        Bitmap cloth = tint("ClothPattern_01^r", 0xFF000000 | MiniMap.PAPER_BLACK, 0xFF000000 | MiniMap.PAPER_WHITE);
        if (cloth != null) {
            android.graphics.BitmapShader sh = new android.graphics.BitmapShader(cloth, android.graphics.Shader.TileMode.MIRROR, android.graphics.Shader.TileMode.MIRROR);
            android.graphics.Matrix m = new android.graphics.Matrix();
            m.setScale(2.5f * s, 2.5f * s);
            m.postTranslate(ch.left, ch.top);
            sh.setLocalMatrix(m);
            fill.setShader(sh);
            c.drawRect(ch, fill);
            fill.setShader(null);
        } else {
            fill.setColor(0xFFF3E6C2);
            c.drawRect(ch, fill);
        }
        boolean sea = map != null && map.length > 60 && map[0] == 1;
        for (int i = 0; i < 49; i++) {
            int row = i / 7, col = i % 7;
            boolean filled = map != null && map.length > 60 && map[12 + i] > 0;
            RectF sq = new RectF(ch.left + col * q, ch.top + row * q, ch.left + (col + 1) * q, ch.top + (row + 1) * q);
            if (!filled) {
                fill.setColor(MiniMap.UNKNOWN & 0x33FFFFFF);
                c.drawRect(sq, fill);
                continue;
            }
            Bitmap isl = tint(GameIcons.mapIsland(i + 1, false), 0xFF000000 | MiniMap.ISLAND_BLACK, 0xFF000000 | MiniMap.ISLAND_WHITE);
            if (isl != null) c.drawBitmap(isl, null, sq, bmp);
        }
        // the grid and the edge
        fill.setColor(MiniMap.GRID);
        for (int i = 1; i < 7; i++) {
            c.drawRect(ch.left + i * q - s, ch.top, ch.left + i * q + s, ch.bottom, fill);
            c.drawRect(ch.left, ch.top + i * q - s, ch.right, ch.top + i * q + s, fill);
        }
        fill.setStyle(Paint.Style.STROKE);
        fill.setStrokeWidth(5 * s);
        fill.setColor(MiniMap.EDGE);
        c.drawRect(ch, fill);
        fill.setStyle(Paint.Style.FILL);
        // the chosen square (else Link's)
        int linkSq = -1;
        if (sea) {
            int col = (int) Math.floor((map[1] + 3.5f * SQUARE) / SQUARE), row = (int) Math.floor((map[2] + 3.5f * SQUARE) / SQUARE);
            if (col >= 0 && col < 7 && row >= 0 && row < 7) linkSq = row * 7 + col;
        }
        int shown = square >= 0 ? square : linkSq;
        if (shown >= 0) {
            RectF sq = new RectF(ch.left + (shown % 7) * q, ch.top + (shown / 7) * q, ch.left + (shown % 7 + 1) * q, ch.top + (shown / 7 + 1) * q);
            fill.setColor(shown == linkSq ? 0xC0F07828 : 0x40FF5A1E);  // (Link's square orange, as the game's map)
            c.drawRect(sq, fill);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(4 * s);
            fill.setColor(0xFFE0501E);
            c.drawRect(sq, fill);
            fill.setStyle(Paint.Style.FILL);
        }
        // the King of Red Lions and Link
        if (map != null && map.length > 11) {
            float per = ch.width() / (7 * SQUARE);
            if (map[5] != 0 && (sea || map[0] == 2)) {
                Bitmap ship = tex("Ship_00^q");
                float sx = ch.centerX() + map[6] * per, sy = ch.centerY() + map[7] * per;
                if (ship != null && map[4] == 0) {
                    float hh = 54 * s, hw = hh * ship.getWidth() / ship.getHeight();
                    c.save();
                    c.rotate(180f - (float) Math.toDegrees(map[8]), sx, sy);
                    c.drawBitmap(ship, null, new RectF(sx - hw / 2, sy - hh / 2, sx + hw / 2, sy + hh / 2), bmp);
                    c.restore();
                }
            }
            if (sea) {
                float lx = ch.centerX() + map[1] * per, ly = ch.centerY() + map[2] * per;
                Bitmap arrow = tex("MapDirectionArrow_00^t"), face = tex("MapLinkS_00^l");
                if (arrow != null) {
                    c.save();
                    c.rotate(180f - (float) Math.toDegrees(map[3]), lx, ly);
                    float ah = 46 * s, aw = ah * arrow.getWidth() / arrow.getHeight();
                    bmp.setColorFilter(new android.graphics.PorterDuffColorFilter(0xFFE0281E, android.graphics.PorterDuff.Mode.MULTIPLY));
                    c.drawBitmap(arrow, null, new RectF(lx - aw / 2, ly - ah * 1.25f, lx + aw / 2, ly - ah * 0.25f), bmp);
                    bmp.setColorFilter(null);
                    c.restore();
                }
                if (face != null) c.drawBitmap(face, null, new RectF(lx - 22 * s, ly - 22 * s, lx + 22 * s, ly + 22 * s), bmp);
            }
        }
        drawMapSide(c, shown);
    }

    /** indoors: the stage's map as the console's full map draws it (its Map pictures), framed on the sheet; the
     *  compass beside */
    private void drawIndoor(Canvas c) {
        RectF r = box(-90, -22, 680, 510);
        chartRect.setEmpty();
        fill.setColor(0x26000B3B);
        c.drawRoundRect(r, 18 * s, 18 * s, fill);
        if (indoor != null) {
            float k = Math.min(r.width() / indoor.getWidth(), r.height() / indoor.getHeight());
            float w = indoor.getWidth() * k, h = indoor.getHeight() * k;
            RectF at = new RectF(r.centerX() - w / 2, r.centerY() - h / 2, r.centerX() + w / 2, r.centerY() + h / 2);
            c.drawBitmap(indoor, null, at, bmp);
            fill.setStyle(Paint.Style.STROKE);
            fill.setStrokeWidth(5 * s);
            fill.setColor(MiniMap.EDGE);
            c.drawRect(at, fill);
            fill.setStyle(Paint.Style.FILL);
        } else msg(c, word("MenuMap_00:T_NoData_00", "…"), r.centerX(), r.centerY(), 44 * s, GameUi.OPTION_TEXT, 0);
        drawMapSide(c, -1);
    }

    private void drawMapSide(Canvas c, int shown) {
        // the square's name on the title band and its island
        if (shown >= 0) {
            String n = name(PadLink.LABEL_ISLAND, shown + 1, "");
            boolean filled = map != null && map.length > 60 && map[12 + shown] > 0;
            if (!filled) n = word("MenuMap_00:L_BtnMapBalloon_00-T_Unopened_00", "?");
            float th = 40 * s, w = Math.max(240 * s, msgWidth(n, th) + 60 * s);
            RectF band = new RectF(X(SIDE_X) - w / 2, Y(232), X(SIDE_X) + w / 2, Y(180));
            Bitmap tb = tint("TitleBase_00^t", 0xFF7A4A1E, 0xFFC8956A);
            if (tb != null) c.drawBitmap(tb, null, band, bmp);
            else {
                fill.setColor(0xFFC8956A);
                c.drawRect(band, fill);
            }
            msg(c, n, band.centerX(), band.centerY(), th, 0xFF5A1E0A, 0);
            RectF pic = box(SIDE_X, 50, 230, 230);
            fill.setColor(0x26000B3B);
            c.drawRoundRect(pic, 18 * s, 18 * s, fill);
            if (filled) {
                Bitmap full = tint(GameIcons.mapIsland(shown + 1, true), 0xFF000000 | MiniMap.ISLAND_BLACK, 0xFF000000 | MiniMap.ISLAND_WHITE);
                Bitmap isl = full != null ? full : tint(GameIcons.mapIsland(shown + 1, false), 0xFF000000 | MiniMap.ISLAND_BLACK, 0xFF000000 | MiniMap.ISLAND_WHITE);
                if (isl != null) c.drawBitmap(isl, null, pic, bmp);
            }
        }
        // the compass: the wind's way on the game's wind dial (MapWindBase / WindArrowNow), N E S W around it
        float x = X(SIDE_X), y = Y(-180), rad = 86 * s;
        Bitmap dial = tint("MapWindBase_00^t", 0xFF3C2A10, 0xFFFFF4D8), arrow = tint("WindArrowNow_00^t", 0xFF2A6EB4, 0xFFFFFFFF);
        if (dial != null) {
            for (int qd = 0; qd < 4; qd++) {
                c.save();
                c.scale(qd % 2 == 0 ? 1 : -1, qd < 2 ? 1 : -1, x, y);
                c.drawBitmap(dial, null, new RectF(x - rad, y - rad, x, y), bmp);
                c.restore();
            }
        }
        if (arrow != null && map != null && map.length > 71 && (map[70] != 0 || map[71] != 0)) {
            float deg = 180f - (float) Math.toDegrees(map[70]), hh = rad * 1.9f, hw = hh * arrow.getWidth() / arrow.getHeight();
            c.save();
            c.rotate(deg, x, y);
            for (int half = 0; half < 2; half++) {
                c.save();
                if (half == 1) c.scale(-1, 1, x, y);
                c.drawBitmap(arrow, null, new RectF(x - hw, y - hh / 2, x, y + hh / 2), bmp);
                c.restore();
            }
            c.restore();
        }
        String[] nesw = {word("MenuMap_00:T_DecoN_00", "N"), word("MenuMap_00:T_DecoE_00", "E"), word("MenuMap_00:T_DecoS_00", "S"),
                word("MenuMap_00:T_DecoW_00", "W")};
        float d = rad * 1.25f;
        float[][] at = {{0, -d}, {d, 0}, {0, d}, {-d, 0}};
        for (int i = 0; i < 4; i++) title(c, nesw[i], x + at[i][0], y + at[i][1], 44 * s, i == 0 ? 0xFFFFC81E : 0xFFFFFFFF, 0xFF5A3C14);
    }

    // ---------------------------------------------------------------- touches (from Controls, this view's coordinates)
    private int dragSlot = -1, downSlot = -1;
    private float downX, downY, lastY, dragX, dragY;
    private boolean dragged;
    private RectF pressed;

    void touch(int action, float x, float y) {
        switch (action) {
            case MotionEvent.ACTION_DOWN: {
                downX = x;
                downY = lastY = y;
                dragged = false;
                downSlot = page == PAGE_ITEMS ? cellAt(x, y) : -1;
                pressed = page != PAGE_ITEMS ? null : saveRect.contains(x, y) ? saveRect : optionsRect.contains(x, y) ? optionsRect
                        : bag >= 0 && backRect.contains(x, y) ? backRect : null;
                if (pressed != null) invalidate();
                break;
            }
            case MotionEvent.ACTION_MOVE: {
                if (Math.hypot(x - downX, y - downY) > 12 * dp) {
                    dragged = true;
                    if (pressed != null) {
                        pressed = null;
                        invalidate();
                    }
                }
                if (page == PAGE_COLLECTION && dragged && listRect.contains(downX, downY)) {
                    scroll = Math.max(0, Math.min(Math.max(0, contentH - listRect.height()), scroll - (y - lastY)));
                    invalidate();
                } else if (page == PAGE_ITEMS && dragged && downSlot >= 0 && dragSlot < 0 && !isBag(downSlot)) {
                    dragSlot = downSlot;
                    invalidate();
                }
                if (dragSlot >= 0) {
                    dragX = x;
                    dragY = y;
                    invalidate();
                }
                lastY = y;
                break;
            }
            case MotionEvent.ACTION_UP: {
                if (dragSlot >= 0) {
                    int b = barAt(x, y);
                    if (b >= 0) equip(BAR_BUTTON[b], dragSlot);
                    dragSlot = -1;
                    invalidate();
                } else if (!dragged) tap(x, y);
                else if (dragged && Math.abs(x - downX) > 3 * Math.abs(y - downY) && Math.abs(x - downX) > 80 * dp && dragSlot < 0
                        && !(page == PAGE_COLLECTION && listRect.contains(downX, downY) && Math.abs(y - downY) > 20 * dp)) {
                    // a swipe across: the items' two pages, as the game's
                    if (x < downX && page == PAGE_ITEMS && bag < 0) turn(PAGE_COLLECTION);
                    else if (x > downX && page == PAGE_COLLECTION) turn(PAGE_ITEMS);
                }
                if (pressed != null) {
                    pressed = null;
                    invalidate();
                }
                break;
            }
            case MotionEvent.ACTION_CANCEL:
                dragSlot = -1;
                pressed = null;
                invalidate();
                break;
            default:
                break;
        }
    }

    @Override
    public void draw(Canvas c) {
        super.draw(c);
        if (dragSlot >= 0) {  // the item under the finger
            Bitmap b = tex(GameIcons.itemTexture(itemAt(dragSlot)));
            float sz = 128 * s;
            if (b != null) c.drawBitmap(b, null, new RectF(dragX - sz / 2, dragY - sz, dragX + sz / 2, dragY), bmp);
        }
    }

    private boolean isBag(int slot) {
        for (int k = 0; k < 3; k++) if (slot == BAG_SLOTS[k]) return true;
        return false;
    }

    private int cellAt(float x, float y) {
        for (int i = cellRects.size() - 1; i >= 0; i--) if (cellRects.get(i).contains(x, y)) return cellSlots.get(i);
        return -1;
    }

    private int barAt(float x, float y) {
        for (int b = 0; b < 3; b++) {
            RectF r = bars[b];
            float m = 20 * s;
            if (x >= r.left - m && x <= r.right + m && y >= r.top - m && y <= r.bottom + m) return b;
        }
        return -1;
    }

    private void click() {
        performHapticFeedback(android.view.HapticFeedbackConstants.VIRTUAL_KEY);
    }

    private void turn(int p) {
        page = p;
        bag = selected = -1;
        caption = null;
        scroll = 0;
        click();
        invalidate();
    }

    private void tap(float x, float y) {
        if (tabs[0].contains(x, y)) {
            if (page == PAGE_MAP) turn(PAGE_ITEMS);
            return;
        }
        if (tabs[1].contains(x, y)) {
            if (page != PAGE_MAP) {
                square = -1;
                turn(PAGE_MAP);
            }
            return;
        }
        if (!inGame()) return;
        if (page == PAGE_MAP) {
            if (chartRect.contains(x, y)) {
                int col = (int) ((x - chartRect.left) / (chartRect.width() / 7)), row = (int) ((y - chartRect.top) / (chartRect.height() / 7));
                int sq = Math.min(6, row) * 7 + Math.min(6, col);
                square = square == sq ? -1 : sq;
                click();
                invalidate();
            }
            return;
        }
        if (page == PAGE_COLLECTION) {
            if (arrowRect.contains(x, y)) {
                turn(PAGE_ITEMS);
                return;
            }
            for (int i = 0; i < iconRects.size(); i++)
                if (iconRects.get(i).contains(x, y) && listRect.contains(x, y)) {
                    caption = iconNames.get(i);
                    click();
                    invalidate();
                    return;
                }
            caption = null;
            invalidate();
            return;
        }
        // the items page
        if (saveRect.contains(x, y) && bag < 0) {
            link.post(PadLink.SAVE_GAME, new byte[0]);
            click();
            return;
        }
        if (optionsRect.contains(x, y) && bag < 0) {  // the console's options: they come as its dialog (UiDraw)
            link.post(PadLink.OPTIONS, new byte[0]);
            click();
            return;
        }
        if (bag < 0 && arrowRect.contains(x, y)) {
            turn(PAGE_COLLECTION);
            return;
        }
        if (bag >= 0 && backRect.contains(x, y)) {
            bag = -1;
            selected = -1;
            click();
            invalidate();
            return;
        }
        int b = barAt(x, y);
        if (b >= 0) {
            if (selected >= 0) equip(BAR_BUTTON[b], selected);
            return;
        }
        int slot = cellAt(x, y);
        if (slot < 0) {
            if (bag >= 0 && !box(BAG_X, BAG_Y, 562, 494).contains(x, y)) bag = -1;  // (beside the bag: closed)
            selected = -1;
            invalidate();
            return;
        }
        for (int k = 0; k < 3; k++)
            if (slot == BAG_SLOTS[k] && bag < 0) {  // a bag: its things
                bag = k;
                selected = -1;
                click();
                invalidate();
                return;
            }
        selected = selected == slot ? -1 : slot;
        click();
        invalidate();
    }

    private void equip(int button, int slot) {
        int item = itemAt(slot);
        if (isBag(slot) || item < 0 || item == EMPTY) return;  // (a bag goes on no button)
        link.post(PadLink.EQUIP, PadLink.body(o -> {
            o.writeInt(button);
            o.writeInt(slot);
        }));
        // (shown at once; the console's next state confirms it, or puts it back)
        int was = -1;
        for (int b = 0; b < 3; b++) if (ring[21 + b] == slot) was = b;
        if (was >= 0) ring[21 + was] = ring[21 + button];
        ring[21 + button] = slot;
        selected = -1;
        click();
        invalidate();
    }
}
