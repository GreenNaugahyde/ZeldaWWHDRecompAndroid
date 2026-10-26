package org.wwhdrecomp.app;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.ColorFilter;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.PixelFormat;
import android.graphics.RectF;
import android.graphics.Shader;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.graphics.drawable.Drawable;
import android.graphics.drawable.GradientDrawable;
import android.graphics.drawable.StateListDrawable;
import android.os.Bundle;
import android.text.Layout;
import android.text.TextPaint;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.ImageView;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.Random;

/**
 * The in-game menu, in the style of the game's own pause menus (drawn in code, no game assets): a
 * parchment panel with a rough edge over the dimmed game, cyan tabs (Saves, Graphics, Mods, Controls), blue
 * bar buttons for actions, and a slot per setting with its value: switches (ON/OFF), choices
 * (◀ value ▶) and sub-menus (›). Changes apply at once. Works with touch and with a game
 * controller: L/R switch tabs, the D-pad moves and changes values, and as in the game the right
 * button selects and the bottom one goes back.
 */
final class OptionsMenu extends Dialog {
    static int lastTab;  // reopens where it was left

    private static final int SLATE = GameUi.SLATE, CYAN_DARK = GameUi.CYAN_DARK, INK = GameUi.INK, HINT = GameUi.HINT;
    private static final int YELLOW = GameUi.YELLOW, GLOW = GameUi.GLOW;

    private final MainActivity a;
    private final float dp;
    // fitted to the window (phones are ~850 x 390 dp, the screens this was laid out on ~1200 x 750):
    // sizes scale by s, text by ts (shrinks less, and a very large system font scale is capped);
    // compact puts About and Quit in a bottom bar instead of a column on the left
    private final float s, ts;
    private final boolean compact;
    private android.widget.HorizontalScrollView tabScroll;
    private int tab;
    private LinearLayout rows;
    private ScrollView scroll;
    private static final int TABS = 5;
    // Saves, Graphics, Mods, Controls
    private static final GameUi.Palette[] TAB_COLORS = {GameUi.GREEN, GameUi.VIOLET, GameUi.BLUE, GameUi.AMBER, GameUi.CORAL};
    private final TextView[] tabs = new TextView[TABS];

    private String languageAtOpen;  // the game language when the menu opened: a change asks for a restart on closing

    // the picker at start (MainActivity.offerStartStates): only the save states and Start game; the
    // game waits before its intro until a state is loaded or the picker is closed
    private final boolean atStart;
    private boolean loadChosen;

    // a page of the GamePad inventory (its arrow left of the items, MainActivity.onOptionsPage): slides in from the
    // left, Back and its right arrow go back to the inventory; actions that show their effect in the game
    // (dismiss without backToInventory) leave the inventory too
    private final boolean inInventory;
    // the game's style (every menu, the picker at start too): the GamePad menus' paper, tabs and plates; the inventory
    // page lies on the game's own paper, the in-game menu draws it (GamePaper) on the game's blue background
    private final boolean gameStyle;
    private float unit;  // pixels per layout unit of the game's menus (1280 x 720 across its 16:9 screen)
    private RectF paper;  // the in-game menu's paper sheet, in the window
    private RectF pageArea;
    private int tabH;  // the inventory page's tab plates: the height of the game's tabs on its pages

    /** the inventory page's tabs: the game's font at one size that lets all five fit the page's width (their words
     * as wide as they are, the same margin each); again once the font is built (first use) */
    private final TextView[] footTabs = new TextView[3];  // About, Quit (and Back) as plates at the foot (the game's style)

    private GameUi.OutlinedText footPlate(String label, GameUi.Palette pal, Runnable run) {
        GameUi.OutlinedText t = new GameUi.OutlinedText(getContext());
        t.setText(label.toUpperCase(java.util.Locale.ROOT));
        t.setTextColor(Color.WHITE);
        t.outline = pal.dark;
        t.setBackground(new GameUi.GameTabPlate(getContext(), false, pal, unit));
        t.setMaxLines(1);
        t.setFocusable(true);
        t.setOnClickListener(v -> run.run());
        // (the visible part's middle: the plate is tabH less its shadow's room, 0.3 of it off the screen)
        t.setPadding(0, 0, 0, Math.round(tabH * 0.3f));
        return t;
    }

    private GameUi.OutlinedText startPlate;  // the picker at start's one plate (the game's style)

    private static final float CHOSEN_TEXT = 1.12f, CHOSEN_PLATE = 1.08f, CHOSEN_HEIGHT = 1.2f;  // the chosen tab is larger

    private void sizeGameTabs() {
        boolean font = GameFont.ready(getContext(), () -> { if (tabs[0] != null || startPlate != null) sizeGameTabs(); });
        // all in the game's font, or all in the system's when it lacks a letter of one (most kanji)
        for (int i = 0; i < TABS && tabs[0] != null && font; i++) font = GameFont.covers(tabs[i].getText().toString());
        if (startPlate != null && font) font = GameFont.covers(startPlate.getText().toString());
        for (TextView f : footTabs)
            if (f != null && font) font = GameFont.covers(f.getText().toString());
        float avail = pageArea.width() - px(16) - 4 * tabH * 0.12f, margin = tabH * 0.3f;
        float[] w = new float[TABS];
        float sum = 0, margins = 0;
        android.graphics.Paint p = new android.graphics.Paint();
        p.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
        p.setTextSize(100);
        for (int i = 0; i < TABS && tabs[0] != null; i++) {  // widths per pixel of letter height
            String t = tabs[i].getText().toString();
            w[i] = font ? GameFont.width(t) / GameFont.cellHeight() : p.measureText(t) / 100f;
            boolean on = i == tab;
            sum += w[i] * (on ? CHOSEN_TEXT * CHOSEN_PLATE : 1);
            margins += 2 * margin * (on ? CHOSEN_PLATE : 1);
        }
        float cell = sum > 0 ? Math.min(tabH * 0.58f, (avail - margins) / sum) : tabH * 0.58f;  // letter cell height in pixels
        if (startPlate != null) {  // the picker at start: its one plate, as a chosen tab
            String t = startPlate.getText().toString();
            float c = cell * CHOSEN_TEXT, fw = font ? GameFont.width(t) / GameFont.cellHeight() : p.measureText(t) / 100f;
            startPlate.gameFontScale = font ? c / GameFont.cellHeight() : 0;
            if (!font) startPlate.setTextSize(TypedValue.COMPLEX_UNIT_PX, c * 0.8f);
            int h = Math.round(tabH * CHOSEN_HEIGHT);
            startPlate.setPadding(0, 0, 0, Math.round(h * 0.1f));
            ViewGroup.LayoutParams lp = startPlate.getLayoutParams();
            lp.height = h;
            lp.width = Math.round((fw * c + 2 * margin) * CHOSEN_PLATE);
            startPlate.setLayoutParams(lp);
        }
        for (int i = 0; i < TABS && tabs[0] != null; i++) {
            GameUi.OutlinedText v = (GameUi.OutlinedText) tabs[i];
            boolean on = i == tab;
            float c = cell * (on ? CHOSEN_TEXT : 1);
            v.gameFontScale = font ? c / GameFont.cellHeight() : 0;
            if (!font) v.setTextSize(TypedValue.COMPLEX_UNIT_PX, c * 0.8f);
            int h = Math.round(tabH * (on ? CHOSEN_HEIGHT : 1));
            v.setPadding(0, 0, 0, Math.round(h * 0.1f));  // (the plate leaves its shadow's room below)
            ViewGroup.LayoutParams lp = v.getLayoutParams();
            lp.height = h;
            lp.width = Math.round((w[i] * c + 2 * margin) * (on ? CHOSEN_PLATE : 1));
            v.setLayoutParams(lp);
            v.invalidate();
        }
        for (TextView f : footTabs) {  // About and Quit: the tabs' letters, as wide as their words
            if (f == null) continue;
            GameUi.OutlinedText v = (GameUi.OutlinedText) f;
            String t = v.getText().toString();
            float fw = font ? GameFont.width(t) / GameFont.cellHeight() : p.measureText(t) / 100f;
            v.gameFontScale = font ? cell / GameFont.cellHeight() : 0;
            if (!font) v.setTextSize(TypedValue.COMPLEX_UNIT_PX, cell * 0.8f);
            ViewGroup.LayoutParams lp = v.getLayoutParams();
            lp.width = Math.round(fw * cell + 2 * margin);
            v.setLayoutParams(lp);
            v.invalidate();
        }
    }
    private boolean backToInventory;
    private FrameLayout root;

    OptionsMenu(MainActivity a) { this(a, false); }

    OptionsMenu(MainActivity a, boolean atStart) { this(a, atStart, false); }

    OptionsMenu(MainActivity a, boolean atStart, boolean inInventory) {
        super(a, android.R.style.Theme_Translucent_NoTitleBar_Fullscreen);
        this.a = a;
        this.atStart = atStart;
        this.inInventory = inInventory;
        gameStyle = true;  // (the picker at start too)
        languageAtOpen = a.gameLanguage();
        dp = a.getResources().getDisplayMetrics().density;
        tab = lastTab;
        android.graphics.Rect win = a.getWindowManager().getCurrentWindowMetrics().getBounds();
        float wDp = win.width() / dp, hDp = win.height() / dp;
        if (inInventory) {  // laid out in the game's paper area (layout units, y down: the 16:9 screen is -640..640, -360..360),
            // its tabs where the game's tabs are on its pages (they go out with them, aspect.cpp item_tabs_pane)
            unit = a.controlsView().layoutUnitPx();
            pageArea = a.controlsView().layoutToView(-556, -360, 522, 360);  // (inside the paper's torn edge, which ends at -600)
        } else if (gameStyle) {  // the same on a sheet of its own: a 16:9 screen's height (or width) in the window, the sheet
            // as wide as the window allows (clear of a camera cutout), 700 of the 720 units high as the game's
            android.graphics.Insets cut = android.graphics.Insets.NONE;
            WindowInsets wi = a.getWindow().getDecorView().getRootWindowInsets();
            if (wi != null) cut = wi.getInsets(WindowInsets.Type.displayCutout());
            float w = win.width(), h = win.height();
            unit = Math.min(w / 1280f, h / 720f);
            paper = new RectF(cut.left + 20 * unit, h / 2 - 350 * unit, w - cut.right - 20 * unit, h / 2 + 350 * unit);
            pageArea = new RectF(paper.left + 44 * unit, h / 2 - 360 * unit, paper.right - 44 * unit, h / 2 + 360 * unit);
        }
        if (gameStyle) {
            tabH = Math.round(92 * unit);  // (the game's: 180 layout units, ~110 of them on the screen)
            wDp = pageArea.width() / dp * 1.25f;  // (sized as for a somewhat larger screen: it has the paper only)
            hDp = pageArea.height() / dp * 1.25f;
        }
        s = Math.max(0.75f, Math.min(1f, Math.min(wDp / 1100f, hDp / 600f)));
        float fontScale = a.getResources().getConfiguration().fontScale;
        ts = (1 - (1 - s) * 0.6f) * Math.min(1f, 1.3f / fontScale);
        compact = gameStyle || wDp < 1000 || hDp < 520;  // (the game's style: About and Quit as plates at the foot)
        arrowW = px(56);
    }

    private int px(float v) { return Math.round(v * dp * s); }

    private void sp(TextView v, float size) { v.setTextSize(TypedValue.COMPLEX_UNIT_SP, size * ts); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Window w = getWindow();
        // the inventory's page: on the game's paper, nothing of its own behind it
        w.setBackgroundDrawable(inInventory ? new ColorDrawable(0) : gameStyle ? new GameUi.GameBackground(getContext(), unit) : new ColorDrawable(SLATE));
        w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        w.getAttributes().layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;

        root = inInventory ? new PageRoot() : new FrameLayout(getContext());
        // clear of a camera cutout (the window reaches into it; the inventory's page sits on the game's paper)
        if (!gameStyle) root.setOnApplyWindowInsetsListener((v, in) -> {
            android.graphics.Insets c = in.getInsets(WindowInsets.Type.displayCutout());
            v.setPadding(c.left, c.top, c.right, c.bottom);
            return in;
        });
        LinearLayout panel = new LinearLayout(getContext());
        panel.setOrientation(LinearLayout.VERTICAL);
        if (!gameStyle) panel.setBackground(new GameUi.Parchment(getContext()));
        if (gameStyle && !inInventory) {  // the sheet under the page
            View sheet = new View(getContext());
            sheet.setBackground(new GameUi.GamePaper(getContext(), unit));
            FrameLayout.LayoutParams slp = new FrameLayout.LayoutParams(Math.round(paper.width()), Math.round(paper.height()));
            slp.leftMargin = Math.round(paper.left);
            slp.topMargin = Math.round(paper.top);
            root.addView(sheet, slp);
        }
        if (compact) panel.setPadding(px(20), px(12), px(20), px(8));
        else panel.setPadding(px(28), px(18), px(28), px(14));
        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(-1, -1);
        if (compact) plp.setMargins(px(10), px(8), px(10), px(8));
        else plp.setMargins(px(26), px(18), px(26), px(18));
        if (gameStyle) {  // the paper's part of the screen (the inventory page: when it shows), from the tabs to the foot
            plp = new FrameLayout.LayoutParams(Math.round(pageArea.width()), Math.round(pageArea.height()));
            plp.leftMargin = Math.round(pageArea.left);
            plp.topMargin = Math.round(pageArea.top);
            panel.setPadding(px(8), px(4), px(8), px(4));
        }
        root.addView(panel, plp);

        // tabs (they scroll sideways when they don't fit) and Back
        LinearLayout top = new LinearLayout(getContext());
        top.setGravity(Gravity.CENTER_VERTICAL);
        LinearLayout strip = new LinearLayout(getContext());
        strip.setGravity(Gravity.CENTER_VERTICAL);
        strip.setPadding(px(2), px(2), px(2), px(2));  // room for the plates' outline
        tabScroll = new android.widget.HorizontalScrollView(getContext());
        tabScroll.setHorizontalScrollBarEnabled(false);
        tabScroll.setOverScrollMode(View.OVER_SCROLL_NEVER);
        tabScroll.addView(strip, new FrameLayout.LayoutParams(-2, -2));
        top.addView(tabScroll, new LinearLayout.LayoutParams(0, -2, 1));
        int[] names = {R.string.opt_saves, R.string.opt_game, R.string.opt_graphics, R.string.opt_mods, R.string.opt_controls};
        if (atStart) {  // one plate instead of the tabs: Continue starts the game as usual
            TextView v = new GameUi.OutlinedText(getContext());
            v.setText(a.getString(R.string.start_states_title).toUpperCase(java.util.Locale.ROOT));
            v.setContentDescription(a.getString(R.string.start_states_title_hint));
            v.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
            v.setTextColor(Color.WHITE);
            v.setGravity(Gravity.CENTER);
            v.setPadding(px(26), px(6), px(26), px(8));
            v.setBackground(new GameUi.TabPlate(getContext(), true, TAB_COLORS[0]));
            sp(v, 26);
            ((GameUi.OutlinedText) v).outline = TAB_COLORS[0].dark;
            v.setFocusable(true);
            v.setOnClickListener(x -> closeToGame());
            if (gameStyle) {  // as the menus' chosen tab
                GameUi.Palette pal = TAB_COLORS[0];
                v.setBackground(new GameUi.GameTabPlate(getContext(), true, pal, unit));
                v.setTextColor(pal.dark);
                ((GameUi.OutlinedText) v).outline = Color.WHITE;
                startPlate = (GameUi.OutlinedText) v;
            }
            strip.addView(v, new LinearLayout.LayoutParams(-2, -2));
        }
        for (int i = 0; i < TABS && !atStart; i++) {
            final int t = i;
            TextView v = new GameUi.OutlinedText(getContext());
            v.setText(a.getString(names[i]).toUpperCase(java.util.Locale.ROOT));
            v.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
            v.setTextColor(Color.WHITE);
            v.setGravity(Gravity.CENTER);
            v.setPadding(px(26), px(6), px(26), px(8));
            v.setFocusable(true);
            v.setOnClickListener(x -> selectTab(t));
            tabs[i] = v;
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.rightMargin = px(10);
            if (gameStyle) {  // in the game's tab style: its font, one size for all, each plate as wide as its word
                lp = new LinearLayout.LayoutParams(-2, tabH);
                lp.rightMargin = i < TABS - 1 ? Math.round(tabH * 0.12f) : 0;
                v.setPadding(0, 0, 0, 0);
                v.setMaxLines(1);
            }
            strip.addView(v, lp);
        }
        if (gameStyle) strip.setGravity(Gravity.TOP);  // (the chosen tab hangs lower)
        if (!atStart && !gameStyle) {  // (the game's style: a Back plate at the foot; the inventory page: its arrow and B go back)
            TextView back = barButton(a.getString(R.string.opt_back), v -> closeToGame());
            LinearLayout.LayoutParams backLp = new LinearLayout.LayoutParams(px(130), px(46));
            backLp.leftMargin = px(10);
            top.addView(back, backLp);
        }
        LinearLayout.LayoutParams topLp = new LinearLayout.LayoutParams(-1, -2);
        if (gameStyle) {  // the plates' tops above the game screen's edge, as the game's
            topLp.topMargin = -Math.round(tabH * 0.3f);
            panel.setClipChildren(false);
            root.setClipChildren(false);
        }
        panel.addView(top, topLp);

        // actions on the left (in a bottom bar when compact), the tab's settings on the right
        LinearLayout body = new LinearLayout(getContext());
        if (!compact) {
            LinearLayout left = new LinearLayout(getContext());
            left.setOrientation(LinearLayout.VERTICAL);
            left.setPadding(0, px(14), px(18), 0);
            developerSwitch(addAction(left, R.string.opt_about, a::showLicenses));
            addAction(left, R.string.opt_quit, a::quitApp);
            body.addView(left, new LinearLayout.LayoutParams(px(220), -1));
        }
        scroll = new ScrollView(getContext());
        rows = new LinearLayout(getContext());
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setPadding(0, px(10), 0, px(10));
        scroll.addView(rows, new FrameLayout.LayoutParams(-1, -2));
        body.addView(scroll, new LinearLayout.LayoutParams(0, -1, 1));
        panel.addView(body, new LinearLayout.LayoutParams(-1, 0, 1));

        TextView hint = GameUi.text(getContext(), gameStyle);
        hint.setText(atStart ? R.string.start_states_pad_hint : R.string.opt_pad_hint);
        hint.setTextColor(HINT);
        sp(hint, 13);
        hint.setGravity(Gravity.END | Gravity.CENTER_VERTICAL);
        if (compact) {
            LinearLayout bottom = new LinearLayout(getContext());
            bottom.setGravity(Gravity.CENTER_VERTICAL);
            bottom.setPadding(0, px(6), 0, 0);
            for (int i = 0; i < 2; i++) {
                Runnable run = i == 0 ? a::showLicenses : a::quitApp;
                TextView b = barButton(a.getString(i == 0 ? R.string.opt_about : R.string.opt_quit), v -> run.run());
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(px(130), px(46));
                lp.rightMargin = px(10);
                if (gameStyle) {  // plates as the tabs', their feet below the screen's edge as the tabs' tops are above it
                    GameUi.OutlinedText t = footPlate(a.getString(i == 0 ? R.string.opt_about : R.string.opt_quit), i == 0 ? GameUi.BLUE : GameUi.CORAL, run);
                    footTabs[i] = t;
                    b = t;
                    lp = new LinearLayout.LayoutParams(-2, tabH);
                    lp.rightMargin = Math.round(tabH * 0.12f);
                }
                if (i == 0) developerSwitch(b);
                bottom.addView(b, lp);
            }
            sp(hint, 11);  // (two lines fit beside the plates on a phone; at 13 the second was cut by the screen's edge)
            hint.setMaxLines(2);
            hint.setEllipsize(android.text.TextUtils.TruncateAt.END);
            if (inInventory) hint.setVisibility(View.INVISIBLE);  // (the overlay's controller hint; the + button sits there)
            if (gameStyle) {
                hint.setPadding(0, 0, 0, Math.round(tabH * 0.3f));  // (beside the plates' visible part)
                // there it lies on the sea below the paper, not on it: light with a dark shadow (grey was hard to read)
                hint.setTextColor(0xFFF2EEDC);
                hint.setShadowLayer(px(3), 0, px(1), 0xE0142430);
            }
            bottom.addView(hint, new LinearLayout.LayoutParams(0, -2, 1));
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(-1, -2);
            if (gameStyle && !inInventory && !atStart) {  // Back at the right end (the inventory page: its arrow, B and +; the picker: its plate)
                GameUi.OutlinedText t = footPlate(a.getString(R.string.opt_back), GameUi.GREEN, this::closeToGame);
                footTabs[2] = t;
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, tabH);
                lp.leftMargin = Math.round(tabH * 0.12f);
                bottom.addView(t, lp);
            }
            if (gameStyle) blp.bottomMargin = -Math.round(tabH * 0.3f);
            panel.addView(bottom, blp);
        } else {
            if (inInventory) hint.setVisibility(View.GONE);
            panel.addView(hint, new LinearLayout.LayoutParams(-1, -2));
        }

        if (inInventory) {
            // the inventory's page arrow, pointing back to the items (right of this page)
            View arrow = new View(getContext());
            pageArrow = arrow;
            arrow.setAlpha(0);  // (pops in once the page has arrived: follow)
            arrow.setBackground(new GameUi.MenuArrow(getContext(), false));
            arrow.setContentDescription(a.getString(R.string.opt_to_inventory));
            arrow.setFocusable(true);
            arrow.setOnClickListener(v -> closeToGame());
            pressOn(arrow, KeyEvent.KEYCODE_DPAD_RIGHT);  // (a press its way turns the page, as on the game's pages)
            // where the game's right arrow is (it shows under this one: the game's items page stays its page)
            RectF ar = a.controlsView().layoutToView(573 - 40, -74 - 6, 573 + 40, 74 - 6);
            FrameLayout.LayoutParams alp = new FrameLayout.LayoutParams(Math.round(ar.width()), Math.round(ar.height()));
            alp.leftMargin = Math.round(ar.left);
            alp.topMargin = Math.round(ar.top);
            root.addView(arrow, alp);
        }
        if (gameStyle) {  // the game's cursor on the focused tab, row or plate (a controller; none while touched)
            cursor = new FocusCursor();
            root.addView(cursor, new FrameLayout.LayoutParams(-1, -1));
            android.view.ViewTreeObserver vt = root.getViewTreeObserver();
            vt.addOnGlobalFocusChangeListener((was, now) -> {
                if (now != null) now.setDefaultFocusHighlightEnabled(false);  // (Android's thin frame on the tabs and plates)
                cursor.invalidate();
            });
            vt.addOnScrollChangedListener(cursor::invalidate);
            vt.addOnTouchModeChangeListener(touch -> cursor.invalidate());
        }
        if (inInventory) {
            // the + button where the game screen has it (it stays put while the page slides): back to the game
            FrameLayout outer = new FrameLayout(getContext());
            outer.addView(root, new FrameLayout.LayoutParams(-1, -1));
            plus = new PlusButton();
            outer.addView(plus, new FrameLayout.LayoutParams(-1, -1));
            showPlus();
            setContentView(outer);
        } else {
            setContentView(root);
        }
        WindowInsetsController ic = w.getInsetsController();  // the window's views exist from here on
        if (ic != null) {
            ic.hide(WindowInsets.Type.systemBars());
            ic.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        if (atStart) {
            page = this::startPage;
            fill();
            if (gameStyle) sizeGameTabs();
        } else {
            selectTab(tab);
        }
    }

    // sub-dialogs (frame generation, save states...) open on top; show their changes when back
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus && padOpened) {
            padOpened = false;
            tabs[tab].requestFocusFromTouch();  // (out of touch mode: the cursor shows at once)
        }
        if (hasFocus && lostFocus) fill();  // (not the first focus: the rows are fresh, a rebuild only stalled the opening)
        if (!hasFocus) lostFocus = true;
    }

    private boolean lostFocus;
    private boolean padOpened;

    /** opened with a controller (A on the inventory's arrow): its cursor on the chosen tab from the start, as the
     * window is still in touch mode after touches before and shows none */
    void openedByController() {
        if (atStart || tabs[tab] == null) return;
        if (getWindow().getDecorView().hasWindowFocus()) tabs[tab].requestFocusFromTouch();
        else padOpened = true;
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_L1: if (!atStart) selectTab((tab + TABS - 1) % TABS); return true;
            case KeyEvent.KEYCODE_BUTTON_R1: if (!atStart) selectTab((tab + 1) % TABS); return true;
            // by position, as in the game: the bottom button (Android's A) is the Wii U's B, back;
            // the right button (Android's B) is the Wii U's A, select
            case KeyEvent.KEYCODE_BUTTON_A: onBackPressed(); return true;
            case KeyEvent.KEYCODE_BUTTON_MODE: return true;  // closes on release (below): the game opens on release
            case KeyEvent.KEYCODE_BUTTON_START:  // + : as in the inventory, back to the game
                if (!inInventory) return super.onKeyDown(code, e);
                dismiss();
                return true;
            case KeyEvent.KEYCODE_BUTTON_SELECT:
                // held for 0.6 s closes, as it opens; only a fresh press (the one that opened the
                // menu may still be held, its repeats arrive here)
                if (e.getRepeatCount() == 0) rows.postDelayed(selectHeld, 600);
                return true;
            case KeyEvent.KEYCODE_BUTTON_B: {
                View f = getCurrentFocus();
                if (f != null) f.performClick();
                return true;
            }
            default: return super.onKeyDown(code, e);
        }
    }

    // a page inside a tab (e.g. the performance overlay's settings) instead of the tab's own rows
    private Runnable page;
    private boolean onPacksPage;  // the mod packs page is shown (leaving it with changes offers the restart)
    private final java.util.ArrayDeque<Runnable> pageStack = new java.util.ArrayDeque<>();  // the pages it was opened from

    private void openPage(Runnable p) {
        if (page != null) pageStack.push(page);
        page = p;
        fill();
        scroll.scrollTo(0, 0);
        if (rows.getChildCount() > 1) rows.getChildAt(1).requestFocus();
    }

    @Override
    public void onBackPressed() {
        if (atStart) dismiss();  // starts the game
        else if (onPacksPage && a.askRestartForPacks(this::onBackPressed)) {
            // asked to restart for the pack changes; "Later" comes back here and leaves the page
        } else if (page != null) {
            page = pageStack.poll();
            fill();
        } else {
            closeToGame();
        }
    }

    // back to the game (Back, the menu's own tab, the back key, Home / Guide): pack changes that need a
    // restart offer it first, once per change
    private void closeToGame() {
        if (!atStart && a.askRestartForPacks(this::closeToGame)) return;
        if (inInventory && !backToInventory) {  // slides back out to the left, the inventory stays
            slideOut();
            return;
        }
        dismiss();
    }

    // ---- the inventory's page: it lies left of the items, on the game's paper sheet (stretched there, aspect.cpp), and
    // moves with the game's pages (x +1280 shown, 0 the items: Native.itemPageX), which the app moves (opening and
    // closing glide, swipes follow the finger). The game's picture shows a page position a little after it is set:
    // the page here shows each position PAGE_LAG_MS late, to stay on the game's paper.
    private static final long PAGE_LAG_MS = 50;
    private final float[] pageXs = new float[32];
    private final long[] pageTs = new long[32];
    private int pageN;
    private boolean following, closing, arrowIn;
    private View pageArrow;  // the arrow back to the items: shown while the page stands still

    private void showPageArrow(boolean in) {
        if (in == arrowIn || pageArrow == null) return;
        arrowIn = in;
        pageArrow.animate().cancel();
        if (in) {  // a moment after the page stopped: from the right, a little past its place and back
            pageArrow.setTranslationX(tabH * 0.6f);
            pageArrow.animate().alpha(1).translationX(0).setStartDelay(250).setDuration(260).setInterpolator(new android.view.animation.OvershootInterpolator(2f)).start();
        } else {
            pageArrow.animate().alpha(0).setStartDelay(0).setDuration(90).start();
        }
    }

    private final android.view.Choreographer.FrameCallback follow = new android.view.Choreographer.FrameCallback() {
        @Override
        public void doFrame(long nanos) {
            if (!isShowing()) { following = false; return; }
            long now = android.os.SystemClock.uptimeMillis();
            pageXs[pageN % 32] = Native.itemPageX();
            pageTs[pageN % 32] = now;
            pageN++;
            float x = pageXs[(pageN - 1) % 32];
            for (int i = pageN - 1; i >= Math.max(0, pageN - 32); i--)  // the position PAGE_LAG_MS ago
                if (pageTs[i % 32] <= now - PAGE_LAG_MS) { x = pageXs[i % 32]; break; }
            root.setTranslationX((x - 1280f) * a.controlsView().layoutUnitPx());
            // the arrow: once the pages stand still on the page, gone as soon as they move
            showPageArrow(!closing && !Native.itemPageGliding() && x > 1279f && pageXs[(pageN - 1) % 32] > 1279f && !((PageRoot) root).dragging);
            if (closing && !Native.itemPageGliding() && x < 1f && pageXs[(pageN - 1) % 32] < 1f) {
                following = false;
                dismiss();
                return;
            }
            android.view.Choreographer.getInstance().postFrameCallback(this);
        }
    };

    @Override
    protected void onStart() {
        super.onStart();
        ((android.hardware.input.InputManager) a.getSystemService(Context.INPUT_SERVICE)).registerInputDeviceListener(devices, null);
        if (!inInventory || following) return;
        following = true;
        pageN = 0;
        root.setTranslationX((Native.itemPageX() - 1280f) * a.controlsView().layoutUnitPx());
        android.view.Choreographer.getInstance().postFrameCallback(follow);
    }

    private int clothesChoice = -1;  // Link's clothes chosen in this menu (game())

    @Override
    protected void onStop() {
        ((android.hardware.input.InputManager) a.getSystemService(Context.INPUT_SERVICE)).unregisterInputDeviceListener(devices);
        if (clothesChoice >= 0) {
            int c = clothesChoice;
            a.controlsView().postDelayed(() -> Native.setOption("link_clothes", c), 500);  // (once the menu has faded)
        }
        super.onStop();
    }

    /** the arrow (or A on it): the pages glide over to this one */
    void slideIn() { Native.setItemPageDrag(2, 1280); }

    /** back to the items: the pages glide back, the page closes once they are there */
    private void slideOut() {
        if (backToInventory) return;
        backToInventory = closing = true;
        Native.setItemPageDrag(2, 0);
    }

    /** a swipe that pulled the page in let go: on (to it) or back to the items */
    void dragEnd(boolean on) {
        if (on) Native.setItemPageDrag(2, 1280);
        else slideOut();
    }

    private static boolean flung(float dx, float v) { return Math.abs(dx) > 320 || Math.abs(v) > 1500; }

    // the page's root: takes over a drag to the left from the rows (they get a cancel); the pages follow it
    private final class PageRoot extends FrameLayout {
        private float x0, y0, lastX, v;
        private long lastT;
        private boolean dragging;

        PageRoot() { super(OptionsMenu.this.getContext()); }

        private float units(float px) { return px / a.controlsView().layoutUnitPx(); }

        @Override
        public boolean onInterceptTouchEvent(android.view.MotionEvent e) {
            switch (e.getActionMasked()) {
                case android.view.MotionEvent.ACTION_DOWN:
                    x0 = lastX = e.getRawX();
                    y0 = e.getRawY();
                    lastT = e.getEventTime();
                    v = 0;
                    dragging = false;
                    return false;
                case android.view.MotionEvent.ACTION_MOVE: {
                    float dx = e.getRawX() - x0, dy = e.getRawY() - y0;
                    if (!closing && -dx > android.view.ViewConfiguration.get(getContext()).getScaledTouchSlop() && -dx > 1.5f * Math.abs(dy)) {
                        dragging = true;
                        return true;
                    }
                    return false;
                }
                default:
                    return false;
            }
        }

        @Override
        public boolean onTouchEvent(android.view.MotionEvent e) {
            if (!dragging) {  // a touch between the rows (no child took it): the drag can start here too
                if (e.getActionMasked() == android.view.MotionEvent.ACTION_DOWN) { onInterceptTouchEvent(e); return true; }
                if (e.getActionMasked() != android.view.MotionEvent.ACTION_MOVE || !onInterceptTouchEvent(e)) return true;
            }
            float dx = units(e.getRawX() - x0);
            switch (e.getActionMasked()) {
                case android.view.MotionEvent.ACTION_MOVE: {
                    long t = e.getEventTime();
                    if (t > lastT) v = v * 0.5f + 0.5f * units(e.getRawX() - lastX) * 1000f / (t - lastT);
                    lastX = e.getRawX();
                    lastT = t;
                    Native.setItemPageDrag(1, Math.min(0, dx));
                    return true;
                }
                case android.view.MotionEvent.ACTION_UP:
                    dragging = false;
                    if (dx < 0 && v <= 0 && flung(dx, v)) closeToGame();
                    else Native.setItemPageDrag(2, 1280);
                    return true;
                case android.view.MotionEvent.ACTION_CANCEL:
                    dragging = false;
                    Native.setItemPageDrag(2, 1280);
                    return true;
                default:
                    return true;
            }
        }
    }

    /** built ahead (MainActivity.prepareOptionsPage): measured and laid out too, at the window's size and with the
     * cutout's padding it will get, so showing it has only to draw */
    void layOutAhead() {
        // (without it a visible 3-button navigation bar gets the legacy bar background, and the decor's layout asks
        // for the window's ViewRootImpl, which a dialog not shown yet doesn't have: crash on Android 13, issue #58;
        // Android 15+ sets it itself for edge-to-edge)
        getWindow().addFlags(android.view.WindowManager.LayoutParams.FLAG_DRAWS_SYSTEM_BAR_BACKGROUNDS);
        View d = getWindow().getDecorView();
        WindowInsets in = a.getWindow().getDecorView().getRootWindowInsets();
        if (in != null) d.dispatchApplyWindowInsets(in);
        android.graphics.Rect b = a.getWindowManager().getCurrentWindowMetrics().getBounds();
        d.measure(View.MeasureSpec.makeMeasureSpec(b.width(), View.MeasureSpec.EXACTLY), View.MeasureSpec.makeMeasureSpec(b.height(), View.MeasureSpec.EXACTLY));
        try {
            d.layout(0, 0, b.width(), b.height());
        } catch (RuntimeException e) {  // (the decor's own work after its children's layout: done again when shown)
            android.util.Log.w("wwhd", "options page laid out ahead: " + e);
        }
    }

    private int arrowW;  // the page arrow's width (onCreate)
    private PlusButton plus;

    /** + on the inventory page, unless the menu button is back (Controls > Menu button in the game) */
    private void showPlus() {
        if (plus != null) plus.setVisibility(a.prefs.getBoolean("menu_overlay", false) ? View.GONE : View.VISIBLE);
    }

    // draws the game screen's + (ControlsView.drawPlus) at its place; a touch on it goes back to the game
    private final class PlusButton extends View {
        private final int[] mine = new int[2], theirs = new int[2];

        PlusButton() { super(OptionsMenu.this.getContext()); }

        private RectF place() {
            ControlsView c = a.controlsView();
            RectF r = c == null ? null : c.plusRect();
            if (r == null) return null;
            getLocationOnScreen(mine);
            c.getLocationOnScreen(theirs);
            r.offset(theirs[0] - mine[0], theirs[1] - mine[1]);
            return r;
        }

        @Override
        protected void onDraw(Canvas canvas) {
            RectF r = place();
            if (r == null) return;
            ControlsView c = a.controlsView();
            canvas.save();
            canvas.translate(r.centerX() - c.plusRect().centerX(), r.centerY() - c.plusRect().centerY());
            c.drawPlus(canvas);
            canvas.restore();
        }

        @Override
        public boolean onTouchEvent(android.view.MotionEvent e) {
            RectF r = place();
            if (r == null || e.getActionMasked() != android.view.MotionEvent.ACTION_DOWN) return false;
            r.inset(-r.width() * 0.15f, -r.height() * 0.15f);
            if (!r.contains(e.getX(), e.getY())) return false;
            dismiss();  // (not backToInventory: the game)
            return true;
        }
    }

    private void pageTitle(int title) { pageTitle(a.getString(title)); }

    private void pageTitle(String title) {
        TextView t = GameUi.text(getContext(), gameStyle);
        t.setText("‹  " + title);
        t.setTextColor(INK);
        sp(t, 20);
        t.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
        t.setPadding(px(12), px(6), px(12), px(10));
        t.setFocusable(true);
        t.setBackground(gameStyle ? pressed(slot(GLOW, YELLOW)) : states(new ColorDrawable(0), slot(GLOW, YELLOW)));
        t.setOnClickListener(v -> onBackPressed());
        rows.addView(t, new LinearLayout.LayoutParams(-2, -2));
    }

    // frame generation (Lossless Scaling's, with the user's own Lossless.dll)
    private void frameGenPage() {
        pageTitle(R.string.opt_framegen);
        if (!a.hasFrameGenDll()) {
            note(a.getString(R.string.fg_about));
            submenu(R.string.opt_fg_choose_dll, 0, "", a::pickDll);
            return;
        }
        String err = Native.frameGenError();
        if (a.frameGenSwitchedOn() && !err.isEmpty()) note(a.getString(R.string.fg_error, err));
        toggle(R.string.opt_framegen, 0, a.frameGenSwitchedOn(), on -> {
            if (on) a.enableFrameGen(this::fill);  // (Mali GPUs: after a warning)
            else a.setFrameGen("fg_enabled", false);
        });
        // the rate frame generation reaches: frames shown without it (30, or interpolated) times the
        // multiplier; above the display's refresh rate it generates nothing (the renderer checks)
        String[] mult = new String[3];
        int base = a.shownFps(), maxHz = a.maxRefreshHz();
        for (int m = 2; m <= 4; m++)
            mult[m - 2] = a.getString(base * m > maxHz * 1.1 ? R.string.fg_multiplier_item_over : R.string.fg_multiplier_item, m, base * m);
        choice(R.string.opt_fg_multiplier, R.string.opt_fg_multiplier_hint, mult, a.prefs.getInt("fg_multiplier", 2) - 2,
                i -> a.setFrameGenMultiplier(i + 2));
        String[] modes = {a.getString(R.string.fg_performance), a.getString(R.string.fg_quality)};
        choice(R.string.opt_fg_network, 0, modes, a.prefs.getBoolean("fg_quality", false) ? 1 : 0, i -> a.setFrameGen("fg_quality", i == 1));
        String[] flows = new String[MainActivity.FG_FLOW_SCALES.length];
        for (int i = 0; i < flows.length; i++) flows[i] = Math.round(Float.parseFloat(MainActivity.FG_FLOW_SCALES[i]) * 100) + "%";
        choice(R.string.opt_fg_flow, R.string.opt_fg_flow_hint, flows, indexOf(MainActivity.FG_FLOW_SCALES, a.prefs.getString("fg_flow_scale", "0.5"), 1),
                i -> a.setFrameGenFlow(MainActivity.FG_FLOW_SCALES[i]));
        toggle(R.string.opt_fg_ui, R.string.opt_fg_ui_hint, a.prefs.getBoolean("fg_ui_detection", true), on -> a.setFrameGen("fg_ui_detection", on));
        submenu(R.string.opt_fg_replace_dll, a.frameGenDllTested() ? R.string.fg_dll_tested_hint : R.string.fg_dll_untested_hint, "",
                a::pickDll);
        submenu(R.string.opt_fg_remove_dll, 0, "", () -> new GameDialog(getContext()).title(R.string.opt_fg_remove_dll)
                .message(R.string.opt_fg_remove_confirm)
                .button(R.string.opt_cancel, null)
                .button(R.string.opt_fg_remove_dll, () -> { a.removeFrameGenDll(); fill(); }).show());
    }

    // the GPU driver (Adreno): the system's or a driver package the user installed
    private void gpuDriverPage() {
        pageTitle(R.string.opt_gpu_driver);
        note(a.getString(R.string.gpu_driver_about));
        String running = Native.gpuDriverInfo();
        if (!running.isEmpty()) note(a.getString(R.string.gpu_driver_running, running));
        if (Native.gpuDriverFellBack()) note(a.getString(R.string.gpu_driver_fell_back, a.gpuDriverLabel()));
        String cur = a.gpuDriverId();
        driverRow(a.getString(R.string.gpu_driver_system), a.getString(R.string.gpu_driver_system_hint), cur.isEmpty(),
                () -> a.chooseGpuDriver("", a.getString(R.string.gpu_driver_system)));
        for (GpuDrivers.Driver d : GpuDrivers.list(getContext())) {
            boolean active = d.id.equals(cur);
            driverRow(d.name, d.version, active, () -> {
                GameDialog dlg = new GameDialog(getContext()).title(d.name)
                        .message(d.version + (d.description.isEmpty() ? "" : "\n\n" + d.description))
                        .button(R.string.opt_cancel, null)
                        .button(R.string.gpu_driver_remove, () -> a.removeGpuDriver(d.id, this::fill));
                if (!active) dlg.button(R.string.gpu_driver_use, () -> a.chooseGpuDriver(d.id, d.name));
                dlg.show();
            });
        }
        submenu(R.string.gpu_driver_install, R.string.gpu_driver_install_hint, "", a::pickDriver);
    }

    private void driverRow(String label, String hint, boolean active, Runnable click) {
        LinearLayout r = rowText(label, hint == null || hint.isEmpty() ? null : hint);
        TextView v = GameUi.text(getContext(), gameStyle);
        v.setText(active ? "✓" : "›");
        v.setTextColor(active ? GameUi.GREEN.dark : INK);
        sp(v, active ? 22 : 16);
        v.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        r.addView(v, new LinearLayout.LayoutParams(-2, -2));
        r.setOnClickListener(x -> click.run());
    }

    // a paragraph of explanation on a page
    private void note(String text) {
        TextView t = GameUi.text(getContext(), gameStyle);
        t.setText(text);
        t.setTextColor(INK);
        sp(t, 16);
        t.setPadding(px(12), px(4), px(12), px(14));
        rows.addView(t, new LinearLayout.LayoutParams(-1, -2));
    }

    private void perfPage() {
        pageTitle(R.string.opt_perf);
        toggle(R.string.perf_show, 0, a.prefs.getBoolean("perf_hud", false), a::setPerfHud);
        int bits = a.prefs.getInt("perf_items", ControlsView.PERF_ALL);
        for (int i = 0; i < MainActivity.PERF_BITS.length; i++) {
            int bit = MainActivity.PERF_BITS[i];
            indentNext = true;  // the overlay's values, below its switch
            toggle(MainActivity.PERF_LABELS[i], 0, (bits & bit) != 0, on -> a.setPerfItem(bit, on));
        }
        submenu(R.string.opt_perf_move, R.string.opt_perf_move_hint, "", () -> {
            a.movePerfOverlay();
            dismiss();  // the game, to drag it on
        });
        submenu(R.string.opt_perf_reset, 0, "", () -> { a.resetPerfOverlay(); fill(); });
    }

    private final Runnable selectHeld = this::closeToGame;

    @Override
    public boolean onKeyUp(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) {
            rows.removeCallbacks(selectHeld);
            return true;
        }
        // Home / Guide: on release, so the release doesn't reach the game screen and reopen the menu
        if (code == KeyEvent.KEYCODE_BUTTON_MODE) {
            closeToGame();
            return true;
        }
        return super.onKeyUp(code, e);
    }

    @Override
    public void dismiss() {
        if (!a.gameLanguage().equals(languageAtOpen)) {
            languageAtOpen = a.gameLanguage();
            a.askRestartForLanguage();
        }
        rows.removeCallbacks(selectHeld);
        if (atStart && !loadChosen) Native.setStartHold(false);  // the game starts as usual
        super.dismiss();
        if (inInventory && !backToInventory) {
            Native.setItemPageDrag(2, 0);
            a.leaveInventory();
        }
        else if (inInventory) a.optionsPageClosed();
    }

    private void selectTab(int t) {
        if (onPacksPage && a.askRestartForPacks(() -> selectTab(t))) return;
        page = null;
        pageStack.clear();
        tab = lastTab = t;
        for (int i = 0; i < TABS; i++) {
            boolean on = i == t;
            GameUi.Palette pal = TAB_COLORS[i];
            if (gameStyle) {  // as the game's tabs: its plates; the chosen one's letters in its dark colour, outlined white
                tabs[i].setBackground(new GameUi.GameTabPlate(getContext(), on, pal, unit));
                tabs[i].setTextColor(on ? pal.dark : Color.WHITE);
                ((GameUi.OutlinedText) tabs[i]).outline = on ? Color.WHITE : pal.dark;
                continue;
            }
            tabs[i].setBackground(new GameUi.TabPlate(getContext(), on, pal));
            sp(tabs[i], on ? 26 : 21);
            ((GameUi.OutlinedText) tabs[i]).outline = on ? pal.dark : pal.muted().dark;
        }
        if (gameStyle) sizeGameTabs();
        for (int i = 0; i < TABS; i++) tabs[i].setFocusedByDefault(i == t);  // (a controller's first press, from touch: the chosen tab)
        // a controller's cursor goes to the new tab, wherever it was (a row of a page: fill would keep its place)
        if (getCurrentFocus() != null) tabs[t].requestFocus();
        View sel = tabs[t];  // in view when the tabs scroll
        tabScroll.post(() -> tabScroll.smoothScrollTo(sel.getLeft() - (tabScroll.getWidth() - sel.getWidth()) / 2, 0));
        fill();
        scroll.scrollTo(0, 0);
    }

    // ---- the tabs' settings
    private void fill() {
        if (rows == null) return;
        View focused = getCurrentFocus();
        int focusIndex = focused != null && focused.getParent() == rows ? rows.indexOfChild(focused) : -1;
        rows.removeAllViews();
        onPacksPage = false;
        if (page != null) page.run();
        else if (tab == 0) saves();
        else if (tab == 1) game();
        else if (tab == 2) graphics();
        else if (tab == 3) mods();
        else controls();
        if (focusIndex >= 0 && focusIndex < rows.getChildCount()) rows.getChildAt(focusIndex).requestFocus();
    }

    // save states (5 slots with their picture, time and place), the auto save and its 2 slots, the
    // last session's state (taken when the app goes to the background), and the import / export of saves
    static final int[] AUTO_STATE_MINUTES = {0, 2, 5, 10};

    private void saves() {
        for (int slot = 1; slot <= 5; slot++) slotRow(slot, a.getString(R.string.opt_slot, slot), true);
        int minutes = Native.getOption("auto_state");
        int cur = 0;
        for (int i = 0; i < AUTO_STATE_MINUTES.length; i++) if (AUTO_STATE_MINUTES[i] == minutes) cur = i;
        choice(R.string.opt_auto_state, R.string.opt_auto_state_hint, a.getResources().getStringArray(R.array.auto_state_modes), cur,
                i -> a.setAutoState(AUTO_STATE_MINUTES[i]));
        for (int k = 1; k <= 2; k++) {
            int slot = 5 + k;
            if (minutes != 0 || Native.saveSlotInfo(slot)[0].equals("1")) slotRow(slot, a.getString(R.string.opt_auto_slot, k), false);
        }
        boolean leave = a.prefs.getBoolean("leave_state", false);
        toggle(R.string.opt_leave_state, R.string.opt_leave_state_hint, leave, on -> a.prefs.edit().putBoolean("leave_state", on).apply());
        if (leave || Native.saveSlotInfo(8)[0].equals("1")) slotRow(8, a.getString(R.string.opt_last_slot), false);
        choice(R.string.opt_start_mode, R.string.opt_start_mode_hint, a.getResources().getStringArray(R.array.start_modes), a.startMode(),
                i -> a.prefs.edit().putInt("start_mode", i).apply());
        submenu(R.string.opt_export, R.string.opt_export_hint, "", a::chooseExport);
        submenu(R.string.opt_import, R.string.opt_import_hint, "", a::chooseImport);
    }

    // the picker at start: the last session first, then the other used slots, the latest first
    private void startPage() {
        java.util.List<Integer> slots = MainActivity.usedStateSlots(a.baseDir());
        if (slots.remove(Integer.valueOf(8))) slots.add(0, 8);
        for (int slot : slots) slotRow(slot, slotLabel(slot), false);
    }

    private String slotLabel(int slot) {
        return slot == 8 ? a.getString(R.string.opt_last_slot)
                : slot > 5 ? a.getString(R.string.opt_auto_slot, slot - 5) : a.getString(R.string.opt_slot, slot);
    }

    // a slot: picture, time, place and controls, its Save (not for the auto slots) and Load buttons
    private void slotRow(int slot, String label, boolean canSave) {
        String[] info = Native.saveSlotInfo(slot);
        boolean used = info[0].equals("1"), compatible = info[1].equals("1");
        String when = info[2];
        long created = info.length > 5 ? Long.parseLong(info[5]) : 0;
        if (created > 0) {  // in the device's language (the runtime's text has English month names)
            java.util.Locale loc = a.getResources().getConfiguration().getLocales().get(0);
            when = new java.text.SimpleDateFormat(android.text.format.DateFormat.getBestDateTimePattern(loc, "MMMdHHmmss"), loc)
                    .format(new java.util.Date(created * 1000));
        }
        String desc = !used ? a.getString(R.string.opt_slot_empty)
                : info[3].isEmpty() ? when : when + " · " + info[3];
        if (used && info[4].equals("pro"))  // saved before every controller acted as the GamePad
            desc += " · " + a.getString(R.string.controller_pro);
        if (used && !compatible) desc = a.getString(R.string.opt_slot_incompatible, desc);
        LinearLayout r = rowText(label, desc);
        ImageView pic = new ImageView(getContext());
        pic.setScaleType(ImageView.ScaleType.CENTER_CROP);
        if (used) thumbnail(slot, pic);
        GradientDrawable frame = new GradientDrawable();
        frame.setColor(0x33000000);
        frame.setCornerRadius(px(6));
        pic.setBackground(frame);
        pic.setClipToOutline(true);
        LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(px(112), px(63));
        plp.rightMargin = px(16);
        r.addView(pic, 0, plp);
        final int n = slot;
        // one width for both columns, wide enough for the language's words (French "Enregistrer" filled 96)
        TextView probe = smallButton(a.getString(R.string.opt_slot_save), true, null);
        float words = Math.max(probe.getPaint().measureText(a.getString(R.string.opt_slot_save)),
                probe.getPaint().measureText(a.getString(R.string.opt_slot_load)));
        LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(Math.max(px(96), Math.round(words) + px(24)), px(44));
        blp.leftMargin = px(10);
        if (canSave) {
            TextView save = smallButton(a.getString(R.string.opt_slot_save), true, v -> {
                if (!used) saveTo(n);
                else new GameDialog(getContext()).title(label).message(a.getString(R.string.state_overwrite, n))
                        .button(R.string.opt_cancel, null)
                        .button(R.string.opt_slot_save, () -> saveTo(n)).show();
            });
            r.addView(save, blp);
        } else if (!atStart) {
            r.addView(new View(getContext()), blp);  // the Load buttons stay in one column
        }
        TextView load = smallButton(a.getString(R.string.opt_slot_load), used && compatible, v -> {
            Native.loadState(n);
            if (atStart) {  // the game waits until it is loaded (a few seconds)
                loadChosen = true;
                android.widget.Toast.makeText(a, R.string.start_states_loading, android.widget.Toast.LENGTH_SHORT).show();
            }
            a.showStateResult();
            dismiss();  // see the game come back
        });
        r.addView(load, new LinearLayout.LayoutParams(blp));
        // the D-pad goes to the Save and Load buttons themselves, not the row
        r.setFocusable(false);
        r.setBackground(gameStyle ? new ColorDrawable(0) : slot(0x1F000000, 0));  // (not lit: its buttons are)
    }

    // a state is captured at the next frame boundary and written in the background (a few seconds
    // for its ~250 MB): show the slot again once its file has changed
    private void saveTo(int slot) {
        final String before = String.join("|", Native.saveSlotInfo(slot));
        final long until = android.os.SystemClock.uptimeMillis() + 20000;
        a.saveState(slot);
        rows.postDelayed(new Runnable() {
            @Override
            public void run() {
                if (!isShowing()) return;
                if (!String.join("|", Native.saveSlotInfo(slot)).equals(before)) {
                    fill();
                    rows.postDelayed(() -> { if (isShowing()) fill(); }, 1500);  // its picture is written separately
                }
                else if (android.os.SystemClock.uptimeMillis() < until) rows.postDelayed(this, 300);
            }
        }, 300);
    }

    // slot pictures: decoded in the background (a full-size PNG each: ~8 of them held up the menu's opening by a
    // quarter of a second) and kept while their file is unchanged, so the next opening has them at once
    private static final java.util.HashMap<String, android.graphics.Bitmap> thumbs = new java.util.HashMap<>();
    private static final java.util.concurrent.ExecutorService thumbLoader = java.util.concurrent.Executors.newSingleThreadExecutor();

    private void thumbnail(int slot, ImageView pic) {
        java.io.File f = MainActivity.stateFile(a.baseDir(), slot, "png");
        String key = f.getPath() + "@" + f.lastModified();
        synchronized (thumbs) {
            if (thumbs.containsKey(key)) {
                pic.setImageBitmap(thumbs.get(key));
                return;
            }
        }
        thumbLoader.execute(() -> {
            android.graphics.Bitmap b = null;
            if (f.exists()) {
                android.graphics.BitmapFactory.Options o = new android.graphics.BitmapFactory.Options();
                o.inSampleSize = 8;  // about 320 px wide is plenty for the slot picture
                b = android.graphics.BitmapFactory.decodeFile(f.getPath(), o);
            }
            synchronized (thumbs) {
                thumbs.keySet().removeIf(k -> k.startsWith(f.getPath() + "@"));
                thumbs.put(key, b);
            }
            android.graphics.Bitmap shown = b;
            if (shown != null) pic.post(() -> pic.setImageBitmap(shown));
        });
    }

    private TextView smallButton(String text, boolean enabled, View.OnClickListener click) {
        TextView b = barButton(text, click);
        sp(b, 15);
        b.setEnabled(enabled);
        b.setFocusable(enabled);
        if (!enabled) b.setAlpha(0.4f);
        return b;
    }

    private void graphics() {
        // the preset: the steps, and Custom while the settings match none of them
        int preset = GraphicsPresets.current(a);
        String[] steps = a.getResources().getStringArray(R.array.preset_steps);
        String[] presets = preset == GraphicsPresets.CUSTOM ? java.util.Arrays.copyOf(steps, steps.length + 1) : steps;
        if (preset == GraphicsPresets.CUSTOM) presets[steps.length] = a.getString(R.string.preset_custom);
        String guess = a.prefs.contains("preset_guess") ? steps[a.prefs.getInt("preset_guess", 0)] : null;
        choice(rowText(a.getString(R.string.opt_preset), guess == null ? a.getString(R.string.opt_preset_hint)
                        : a.getString(R.string.opt_preset_hint) + ". " + a.getString(R.string.preset_guess_hint, guess)), presets, preset == GraphicsPresets.CUSTOM ? steps.length : preset, i -> {
            if (i < steps.length) GraphicsPresets.apply(a, i);
        });
        indentNext = true;
        submenu(R.string.opt_bench, R.string.opt_bench_hint, "", () -> {
            dismiss();
            Benchmark.offer(a, null);
        });
        String[] res = new String[MainActivity.RES_SCALES.length];
        for (int i = 0; i < res.length; i++) res[i] = a.resolutionLabel(MainActivity.RES_SCALES[i]);
        choice(R.string.opt_resolution, 0, res, indexOf(MainActivity.RES_SCALES, a.prefs.getString("res_scale", "1"), 2), a::setResolution);
        int[] rates = a.interpRates();
        String[] rateNames = new String[rates.length];
        int rate = 0;
        for (int i = 0; i < rates.length; i++) {
            rateNames[i] = rates[i] == 0 ? a.getString(R.string.interp_off) : a.getString(R.string.interp_fps, rates[i]);
            if (rates[i] == Native.getOption("interp_fps")) rate = i;
        }
        choice(R.string.opt_interp, R.string.opt_interp_hint, rateNames, rate, i -> a.setInterpFps(rates[i]));
        submenu(R.string.opt_framegen, 0, a.frameGenLabel(), () -> openPage(this::frameGenPage));
        submenu(R.string.opt_page_res, R.string.opt_page_res_hint, "", () -> openPage(this::resolutionPage));
        submenu(R.string.opt_page_quality, R.string.opt_page_quality_hint, "", () -> openPage(this::qualityPage));
        submenu(R.string.opt_page_screen, R.string.opt_page_screen_hint, "", () -> openPage(this::screenPage));
        submenu(R.string.opt_perf, R.string.opt_perf_hint, a.getString(a.prefs.getBoolean("perf_hud", false) ? R.string.opt_on : R.string.opt_off),
                () -> openPage(this::perfPage));
        submenu(R.string.opt_page_advanced, R.string.opt_page_advanced_hint, "", () -> openPage(this::advancedPage));
    }

    // Graphics > Resolution & upscaling
    private void resolutionPage() {
        pageTitle(R.string.opt_page_res);
        String[] dyn = new String[MainActivity.DYN_RES_MINS.length];
        int dynCur = 0;
        for (int i = 0; i < dyn.length; i++) {
            int m = MainActivity.DYN_RES_MINS[i];
            dyn[i] = m == 0 ? a.getString(R.string.opt_off) : a.getString(R.string.dyn_res_down_to, String.valueOf(m / 100f), Math.round(7.2f * m) + "p");
            if (m == a.prefs.getInt("dyn_res_min", 0)) dynCur = i;
        }
        choice(R.string.opt_dyn_res, R.string.opt_dyn_res_hint, dyn, dynCur, a::setDynResMin);
        choice(R.string.opt_upscale, R.string.opt_upscale_hint, a.getResources().getStringArray(R.array.upscale_modes),
               Native.getOption("upscale"), i -> {
                   a.prefs.edit().putInt("upscale", i).apply();
                   Native.setOption("upscale", i);
               });
        choice(R.string.opt_display_res, R.string.opt_display_res_hint, a.getResources().getStringArray(R.array.display_res_modes),
               a.prefs.getInt("display_res", MainActivity.DISPLAY_RES_AUTO), a::setDisplayRes);
    }

    // Graphics > Picture quality
    private void qualityPage() {
        pageTitle(R.string.opt_page_quality);
        choice(R.string.opt_ao, 0, a.getResources().getStringArray(R.array.ao_modes), Native.getOption("ao_mode"), a::setAo);
        toggle(R.string.opt_ao_hires, 0, Native.getOption("ao_hires") != 0, on -> a.setBool("ao_hires", on));
        toggle(R.string.opt_aniso, 0, Native.getOption("aniso") != 0, on -> a.setBool("aniso", on));
    }

    // Graphics > Screen: the picture's shape and zoom, the TV and GamePad screens' places
    private void screenPage() {
        pageTitle(R.string.opt_page_screen);
        choice(R.string.opt_render_aspect, R.string.opt_render_aspect_hint, a.getResources().getStringArray(R.array.render_aspect_modes),
               a.prefs.getInt("render_aspect", 0), a::setRenderAspect);
        // the 3D view zoomed out; a value pinched on the picture shows as one more choice
        int zoom = a.viewZoomPercent(), zoomCur = -1;
        int[] zooms = MainActivity.VIEW_ZOOMS;
        for (int i = 0; i < zooms.length; i++) if (zooms[i] == zoom) zoomCur = i;
        if (zoomCur < 0) {
            zooms = java.util.Arrays.copyOf(zooms, zooms.length + 1);
            zoomCur = zooms.length - 1;
            zooms[zoomCur] = zoom;
        }
        String[] zoomNames = new String[zooms.length];
        for (int i = 0; i < zooms.length; i++)
            zoomNames[i] = zooms[i] == 100 ? a.getString(R.string.opt_view_zoom_original)
                    : String.format(java.util.Locale.ROOT, "%.2f×", zooms[i] / 100f);
        int[] zoomValues = zooms;
        choice(R.string.opt_view_zoom, R.string.opt_view_zoom_hint, zoomNames, zoomCur, i -> a.setViewZoom(zoomValues[i]));
        choice(R.string.opt_aspect, 0, a.getResources().getStringArray(R.array.aspect_modes), a.prefs.getInt("tv_aspect", 0), i -> {
            a.prefs.edit().putInt("tv_aspect", i).apply();
            Native.setOption("tv_aspect", i);
            a.updateLayout();
        });
        if (a.hasSecondDisplay()) {
            String[] where = {a.getString(R.string.drc_second_display), a.getString(R.string.drc_in_layout)};
            choice(R.string.opt_drc_display, R.string.opt_drc_display_hint, where, a.drcOnSecondDisplay() ? 0 : 1,
                    i -> a.setDrcOnSecondDisplay(i == 0));
            if (a.drcOnSecondDisplay()) {
                String[] shapes = {a.getString(R.string.drc_shape_fill), a.getString(R.string.drc_shape_original)};
                indentNext = true;
                choice(R.string.opt_drc_shape, R.string.opt_drc_shape_hint, shapes, a.drcFill() ? 0 : 1, i -> a.setDrcFill(i == 0));
            }
        }
        String[] layouts = a.getResources().getStringArray(R.array.layouts);
        if (!MainActivity.NEW_TOUCH_CONTROLS) layouts = java.util.Arrays.copyOf(layouts, MainActivity.LAYOUT_ON_DEMAND);
        int layout = a.layoutSetting();
        if (layout >= layouts.length) layout = MainActivity.LAYOUT_INSET;
        choice(R.string.opt_layout, 0, layouts, layout, i -> {
            a.prefs.edit().putInt("layout", i).apply();
            a.updateLayout();
        });
    }

    // Graphics > Advanced: the GPU driver, VSync, the recording thread, the shader cache
    private void advancedPage() {
        pageTitle(R.string.opt_page_advanced);
        if (GpuDrivers.supported())
            submenu(R.string.opt_gpu_driver, R.string.opt_gpu_driver_hint, a.gpuDriverLabel(), () -> openPage(this::gpuDriverPage));
        toggle(R.string.opt_vsync, R.string.opt_vsync_hint, Native.getOption("vsync") != 0, on -> a.setBool("vsync", on));
        choice(R.string.opt_record_thread, R.string.opt_record_thread_hint, a.getResources().getStringArray(R.array.record_thread_modes),
                a.prefs.getInt("record_thread", 0), a::setRecordThread);
        submenu(R.string.opt_shaders, R.string.opt_shaders_hint, "", a::askClearShaders);
        if (a.debuggable())  // a debugging aid: debug builds only
            submenu(R.string.opt_capture, R.string.opt_capture_hint, "", () -> Native.setOption("capture", 1));
    }

    // the game itself: its language (the release's languages; applies after a restart)
    private void game() {
        int[] langs = a.gameLanguages();
        String[] names = new String[langs.length];
        int curLang = 0;
        for (int i = 0; i < langs.length; i++) {
            names[i] = MainActivity.LANGUAGE_NAMES[langs[i]];
            if (MainActivity.LANGUAGES[langs[i]].equals(a.gameLanguage())) curLang = i;
        }
        choice(R.string.opt_language, R.string.opt_language_hint, names, curLang, i -> a.setGameLanguage(MainActivity.LANGUAGES[langs[i]]));
        modToggle(3, R.string.opt_mod_quick_doors, R.string.opt_mod_speed_hint);
        modToggle(4, R.string.opt_mod_fast_scenes, R.string.opt_mod_speed_hint);
        modToggle(5, R.string.opt_mod_ff_cutscenes, R.string.opt_mod_ff_hint);
        modToggle(6, R.string.opt_mod_ff_dialogues, R.string.opt_mod_ff_hint);
        modToggle(8, R.string.opt_mod_picto_pages, R.string.opt_mod_picto_pages_hint);
        toggle(R.string.opt_picto_subject, R.string.opt_picto_subject_hint, a.prefs.getBoolean("picto_subject", true),
                on -> a.prefs.edit().putBoolean("picto_subject", on).apply());
        toggle(R.string.opt_postbox, R.string.opt_postbox_hint, a.prefs.getBoolean("postbox_mail", true), on -> {
            a.prefs.edit().putBoolean("postbox_mail", on).apply();
            Native.setOption("postbox_mail", on ? 1 : 0);
        });
        indentNext = true;  // a setting of the one above
        toggle(R.string.opt_postbox_same_day, R.string.opt_postbox_same_day_hint, a.prefs.getBoolean("postbox_same_day", false), on -> {
            a.prefs.edit().putBoolean("postbox_same_day", on).apply();
            Native.setOption("postbox_same_day", on ? 1 : 0);
        });
        String[] clothes = {a.getString(R.string.clothes_story), a.getString(R.string.clothes_hero), a.getString(R.string.clothes_casual)};
        choice(R.string.opt_link_clothes, R.string.opt_link_clothes_hint, clothes, a.prefs.getInt("link_clothes", 0), i -> {
            a.prefs.edit().putInt("link_clothes", i).apply();
            clothesChoice = i;  // (applied once the menu is closed: the game runs behind it, the puff would go unseen)
        });
        submenu(R.string.opt_page_songs, R.string.opt_page_songs_hint, "", () -> openPage(this::songsPage));
    }

    // a mod of MainActivity.MODS by its index there
    private void modToggle(int i, int label, int hint) {
        String key = MainActivity.MODS[i];
        toggle(label, hint, a.prefs.getBoolean(key, false), on -> a.setMod(key, on));
    }

    // Game > Songs: conducted songs replayed, songs sung into the microphone (SongListener, experimental), play buttons
    private void songsPage() {
        pageTitle(R.string.opt_page_songs);
        modToggle(7, R.string.opt_mod_song_replay, R.string.opt_mod_song_replay_hint);
        toggle(R.string.opt_sing_songs, R.string.opt_sing_songs_hint, a.prefs.getInt("sing_songs", SongListener.OFF) != SongListener.OFF,
                on -> a.setSingSongs(on ? SongListener.ON : SongListener.OFF));
        indentNext = true;  // a setting of the one above
        toggle(R.string.opt_sing_button, R.string.opt_sing_button_hint, a.prefs.getBoolean("sing_button", true), a::setSingButton);
        toggle(R.string.opt_song_list_play, R.string.opt_song_list_play_hint, a.prefs.getBoolean("song_list_play", true),
                on -> a.prefs.edit().putBoolean("song_list_play", on).apply());
        toggle(R.string.opt_song_scene_play, R.string.opt_song_scene_play_hint, a.prefs.getBoolean("song_scene_play", true),
                on -> a.prefs.edit().putBoolean("song_scene_play", on).apply());
    }

    // Link's moves: jumps, climbing, faster moves; then the cheats, mod packs and warp (the camera mods are in
    // Controls > Camera, the game's pace and songs in Game)
    private void mods() {
        String[] runs = new String[MainActivity.RUN_SPEEDS.length];
        int curRun = 0, curSwim = 0, curClimb = 0, curSidle = 0;
        for (int k = 0; k < runs.length; k++) {
            runs[k] = MainActivity.RUN_SPEEDS[k] == 100 ? a.getString(R.string.opt_mod_run_off)
                    : String.format(java.util.Locale.ROOT, "%.2f×", MainActivity.RUN_SPEEDS[k] / 100f).replace("0×", "×");
            if (MainActivity.RUN_SPEEDS[k] == a.prefs.getInt("mod_run_speed", 100)) curRun = k;
            if (MainActivity.RUN_SPEEDS[k] == a.prefs.getInt("mod_swim_speed", 100)) curSwim = k;
            if (MainActivity.RUN_SPEEDS[k] == a.prefs.getInt("mod_climb_speed", 100)) curClimb = k;
            if (MainActivity.RUN_SPEEDS[k] == a.prefs.getInt("mod_sidle_speed", 100)) curSidle = k;
        }
        // jump anywhere on its button (not with the moon jump on the same button) and its double jump
        boolean jump = a.prefs.getBoolean("mod_jump", false);
        toggle(R.string.opt_mod_jump, R.string.opt_mod_jump_hint, jump, a::setJumpAnywhere);
        if (jump) {
            indentNext = true;
            ownButtonRow(InputMapper.OWN_JUMP);
            indentNext = true;
            toggle(R.string.opt_mod_jump_double, R.string.opt_mod_jump_double_hint, a.prefs.getBoolean("mod_jump_double", false),
                    on -> a.setMod("mod_jump_double", on));
            indentNext = true;
            toggle(R.string.opt_mod_jump_noauto, R.string.opt_mod_jump_noauto_hint, a.prefs.getBoolean("mod_jump_noauto", false),
                    on -> a.setMod("mod_jump_noauto", on));
        }
        // the moon jump (hold L), and while it's on its rise and flight speed (percent of the tested values),
        // no fall damage and the lift-off poses
        boolean moon = a.prefs.getBoolean("cheat_moon_jump", false);
        toggle(R.string.cheat_moon_jump, R.string.cheat_moon_jump_hint, moon, a::setMoonJump);
        if (moon) {
            indentNext = true;
            ownButtonRow(InputMapper.OWN_MOON);
            String[] moonKey = {"cheat_moon_rise", "cheat_moon_fly"};
            int[] moonLabel = {R.string.cheat_moon_rise, R.string.cheat_moon_fly};
            int[][] moonSpeeds = {MainActivity.MOON_RISE_SPEEDS, MainActivity.MOON_FLY_SPEEDS};
            for (int i = 0; i < moonKey.length; i++) {
                String key = moonKey[i];
                int[] speeds = moonSpeeds[i];
                String[] pct = new String[speeds.length];
                for (int j = 0; j < pct.length; j++) pct[j] = speeds[j] + "%";
                int cur = java.util.Arrays.binarySearch(speeds, a.prefs.getInt(key, 100));
                if (cur < 0) cur = java.util.Arrays.binarySearch(speeds, 100);
                indentNext = true;
                choice(moonLabel[i], 0, pct, cur, k -> {
                    a.prefs.edit().putInt(key, speeds[k]).apply();
                    Native.setOption(key, speeds[k]);
                });
            }
            for (String key : new String[] {"cheat_moon_nofall", "cheat_moon_anims"}) {
                boolean nofall = key.equals("cheat_moon_nofall");
                indentNext = true;
                toggle(nofall ? R.string.cheat_moon_nofall : R.string.cheat_moon_anims,
                       nofall ? R.string.cheat_moon_nofall_hint : R.string.cheat_moon_anims_hint, a.prefs.getBoolean(key, false), on -> {
                    a.prefs.edit().putBoolean(key, on).apply();
                    Native.setOption(key, on ? 1 : 0);
                });
            }
        }
        modToggle(2, R.string.opt_mod_climb, R.string.opt_mod_climb_hint);
        moveSpeed("mod_run", R.string.opt_mod_run, R.string.opt_mod_run_hint, runs, curRun);
        moveSpeed("mod_swim", R.string.opt_mod_swim, R.string.opt_mod_swim_hint, runs, curSwim);
        moveSpeed("mod_climb", R.string.opt_mod_climb_fast, R.string.opt_mod_climb_fast_hint, runs, curClimb);
        moveSpeed("mod_sidle", R.string.opt_mod_sidle, R.string.opt_mod_sidle_hint, runs, curSidle);
        submenu(R.string.opt_cheats, R.string.opt_cheats_hint, "", () -> openPage(this::cheatsPage));
        int packsOn = 0;
        for (String f : ModPacks.list(a.baseDir())) if (a.prefs.getBoolean("pack_on_" + f, false)) packsOn++;
        submenu(R.string.opt_packs, R.string.opt_packs_hint, packsOn > 0 ? a.getString(R.string.packs_on_count, packsOn) : "",
                () -> openPage(this::packsPage));
        if (developer()) submenu(R.string.opt_warp, R.string.opt_warp_hint, "", () -> openPage(this::warpPage));
    }

    // ---- developer options: always in debug builds, else switched by a long press on About
    private boolean developer() { return a.debuggable() || a.prefs.getBoolean("developer", false); }

    private void developerSwitch(View about) {
        about.setOnLongClickListener(v -> {
            boolean on = !a.prefs.getBoolean("developer", false);
            a.prefs.edit().putBoolean("developer", on).apply();
            android.widget.Toast.makeText(a, on ? R.string.dev_on : R.string.dev_off, android.widget.Toast.LENGTH_SHORT).show();
            fill();
            return true;
        });
    }

    // warp (mods/warp.cpp): the places of WarpCatalog by category; a place opens its spawn points, a
    // story / minigame / boss entry warps at once
    private void warpPage() {
        pageTitle(R.string.opt_warp);
        String[] where = Native.warpWhere().split(",");
        String about = a.getString(R.string.warp_about);
        if (where.length == 4) about += "\n\n" + a.getString(R.string.warp_where, where[0], where[1], where[2], where[3]);
        note(about);
        String[] layers = new String[13];
        layers[0] = a.getString(R.string.warp_layer_auto);
        for (int i = 0; i < 12; i++) layers[i + 1] = Integer.toString(i, 16);
        choice(R.string.warp_layer, R.string.warp_layer_hint, layers, WarpCatalog.layer + 1, i -> WarpCatalog.layer = i - 1);
        toggle(R.string.warp_test, R.string.warp_test_hint, a.prefs.getBoolean("warp_test", false), on -> {
            a.setWarpTest(on);
            if (on) dismiss();  // the game, with the overlay on it
        });
        submenu(R.string.preset_title, R.string.preset_hint, Integer.toString(StoryPresets.list(a).size()), () -> openPage(this::presetsPage));
        org.json.JSONArray cats = WarpCatalog.categories(a, () -> { if (isShowing()) fill(); });
        if (cats == null) {
            note(a.getString(R.string.warp_reading));
            return;
        }
        for (int i = 0; i < cats.length(); i++) {
            org.json.JSONObject c = cats.optJSONObject(i);
            submenu(c.optString("name"), null, Integer.toString(c.optJSONArray("items").length()), () -> openPage(() -> warpCategory(c)));
        }
    }

    // story presets (StoryPresets): a save's story part plus a place; warping there loads that story
    private void presetsPage() {
        pageTitle(R.string.preset_title);
        note(a.getString(R.string.preset_about));
        submenu(R.string.preset_import, R.string.preset_import_hint, "", a::pickPresetSave);
        submenu(R.string.preset_save, R.string.preset_save_hint, "", () -> {
            byte[] story = Native.storyBlock();
            String[] where = Native.warpWhere().split(",");
            if (story == null || where.length != 4) {
                android.widget.Toast.makeText(a, R.string.warp_no_save, android.widget.Toast.LENGTH_SHORT).show();
                return;
            }
            a.askName(a.getString(R.string.preset_save), where[0] + " " + where[1], name -> {
                StoryPresets.save(a, name, where[0], Integer.parseInt(where[1]), Integer.parseInt(where[2]), -1, story);
                fill();
            });
        });
        if (songScenePreset("34") != null) submenu(R.string.preset_song_scenes, R.string.preset_song_scenes_hint, "", () -> openPage(this::songScenesPage));
        for (StoryPresets.Preset p : StoryPresets.list(a)) {
            String hint = p.stage + " · " + a.getString(R.string.warp_spawn, p.room, p.spawn) + " · ♥ " + p.hearts() + "/" + p.maxHearts();
            submenu(p.name, hint, "", () -> {
                GameDialog d = new GameDialog(getContext()).title(p.name).message(a.getString(R.string.preset_ask, hint));
                if (p.file != null)  // made on this device (the built-in ones stay)
                    d.button(R.string.preset_delete, () -> {
                        //noinspection ResultOfMethodCallIgnored
                        p.file.delete();
                        fill();
                    });
                d.button(android.R.string.cancel, null).button(R.string.preset_warp, () -> warpPreset(p)).show();
            });
        }
    }

    // song scenes (the built-in presets, debug builds): the story just before a song is learned, Link put in front of
    // its stone tablet / teacher (positions from the stage files: MknjD, Hsh, Tt; Native.warpPlace; spawns without a
    // walk-in). Each: preset number, song (melody), stage, room, spawn, x, y, z, angle (s16, Link facing it).
    // Wind's Requiem (Dragon Roost) not yet: its tablet's place isn't in the room's actor list
    private static final Object[][] SONG_SCENES = {
            {"28", 2, "Siren", 7, 0, 0f, 700f, -8790f, -32768},          // Command Melody: the tablet (Hsh) in the Tower of the Gods (28 with it unlearned: SONG_UNDO)
            {"34", 3, "Edaichi", 0, 2, 0f, 300f, 110f, -32768},         // Earth God's Lyric: Headstone Island's tablet (MknjD)
            {"38", 4, "Ekaze", 0, 2, 0f, 300f, 86f, -32768},            // Wind God's Aria: Gale Isle's tablet (MknjD)
            {"38", 5, "sea", 11, 0, -2770f, 1110f, -203483f, 7464},     // Song of Passing: Tott on Windfall (Tt)
    };

    // Command Melody: no preset stands right before its tablet (27 is before the tower's puzzles, 28 after the song), so
    // 28 with the song undone: its learned bit (save +0xB4 + 9, bit 2), event flag 0x2510 (the tablet is gone once set:
    // daObj_hsh_c::create; dSv_event_c at +0x624) and the tablet's switch 0x29 in the tower's stage memory (table 5:
    // +0x380 + 5 * 0x24, switches from +4, big-endian words)
    private static byte[] songUndo(int song, byte[] story) {
        byte[] b = story.clone();
        if (song == 2) {
            b[0xB4 + 9] &= (byte) ~(1 << 2);
            b[0x624 + 0x25] &= (byte) ~0x10;
            int sw = 0x380 + 5 * 0x24 + 4 + 4 * (0x29 >> 5);  // the u32 holding switch 0x29: bit 9, its byte 3 from the end
            b[sw + 2] &= (byte) ~(1 << (9 - 8));
        }
        return b;
    }

    private StoryPresets.Preset songScenePreset(String number) {
        for (StoryPresets.Preset p : StoryPresets.list(a))
            if (p.file == null && p.name.startsWith(number + " ")) return p;
        return null;
    }

    private void songScenesPage() {
        pageTitle(R.string.preset_song_scenes);
        note(a.getString(R.string.preset_song_scenes_about));
        for (Object[] sc : SONG_SCENES) {
            StoryPresets.Preset p = songScenePreset((String) sc[0]);
            if (p == null) continue;
            String song = GameText.songName((Integer) sc[1]);
            submenu(song, p.name + " · " + sc[2] + " " + a.getString(R.string.warp_spawn, (Integer) sc[3], (Integer) sc[4]), "", () -> {
                Native.warpPlace((Float) sc[5], (Float) sc[6], (Float) sc[7], (Integer) sc[8]);
                if (!Native.warpPreset(songUndo((Integer) sc[1], p.story), (String) sc[2], (Integer) sc[3], (Integer) sc[4], -1)) {
                    android.widget.Toast.makeText(a, R.string.warp_no_save, android.widget.Toast.LENGTH_SHORT).show();
                    return;
                }
                android.widget.Toast.makeText(a, a.getString(R.string.warp_going, song), android.widget.Toast.LENGTH_SHORT).show();
                dismiss();
            });
        }
    }

    private void warpPreset(StoryPresets.Preset p) {
        if (!Native.warpPreset(p.story, p.stage, p.room, p.spawn, p.layer >= 0 ? p.layer : WarpCatalog.layer)) {
            android.widget.Toast.makeText(a, R.string.warp_no_save, android.widget.Toast.LENGTH_SHORT).show();
            return;
        }
        android.widget.Toast.makeText(a, a.getString(R.string.warp_going, p.name), android.widget.Toast.LENGTH_SHORT).show();
        dismiss();
    }

    private void warpCategory(org.json.JSONObject c) {
        pageTitle(c.optString("name"));
        org.json.JSONArray items = c.optJSONArray("items");
        for (int i = 0; i < items.length(); i++) {
            org.json.JSONObject p = items.optJSONObject(i);
            String name = p.optString("name"), stage = p.optString("stage");
            org.json.JSONArray spawns = p.optJSONArray("spawns");
            if (spawns == null) {  // a single spot
                int room = p.optInt("room"), spawn = p.optInt("spawn");
                submenu(name, stage + " · " + a.getString(R.string.warp_spawn, room, spawn), "", () -> warp(name, stage, room, spawn));
            } else {
                submenu(name, stage, Integer.toString(spawns.length()), () -> openPage(() -> warpPlace(p)));
            }
        }
    }

    private void warpPlace(org.json.JSONObject p) {
        String name = p.optString("name"), stage = p.optString("stage");
        pageTitle(name);
        org.json.JSONArray spawns = p.optJSONArray("spawns");
        for (int i = 0; i < spawns.length(); i++) {
            org.json.JSONArray s = spawns.optJSONArray(i);  // room, spawn, what it is
            int room = s.optInt(0), spawn = s.optInt(1);
            String what = s.optString(2);
            submenu(a.getString(R.string.warp_spawn, room, spawn), what.isEmpty() ? null : what, "", () -> warp(name, stage, room, spawn));
        }
    }

    private void warp(String name, String stage, int room, int spawn) {
        if (!Native.warp(stage, room, spawn, WarpCatalog.layer)) {
            android.widget.Toast.makeText(a, R.string.warp_no_save, android.widget.Toast.LENGTH_SHORT).show();
            return;
        }
        android.widget.Toast.makeText(a, a.getString(R.string.warp_going, name), android.widget.Toast.LENGTH_SHORT).show();
        dismiss();  // the game, to see it change
    }

    // cheats (mods/cheats.cpp, from the original project): edits of the save file in play; one-shot
    // ones by a tap, the infinite ones as switches (kept as settings); story cheats ask first
    // ---- mod packs (ModPacks, runtime/src/packs.h): Cemu graphic packs imported from zip files
    private void packsPage() {
        onPacksPage = true;
        pageTitle(R.string.opt_packs);
        note(a.getString(R.string.packs_about));
        submenu(R.string.packs_import, R.string.packs_import_hint, "", () -> a.pickPacks(this::fill));
        java.io.File base = a.baseDir();
        java.util.List<String> folders = ModPacks.list(base);
        if (folders.isEmpty()) note(a.getString(R.string.packs_none));
        for (String f : folders) {
            ModPacks.Info info = ModPacks.describe(base, a.gameDir(), a.prefs, f);
            boolean on = a.prefs.getBoolean("pack_on_" + f, false);
            toggle(rowText(info.name, info.summary(a)), on, v -> {
                a.prefs.edit().putBoolean("pack_on_" + f, v).commit();
                ModPacks.writeEnabled(base, a.prefs);
                a.markPacksChanged();
            });
            if (on)
                for (String[] c : info.categories) {
                    String[] presets = java.util.Arrays.copyOfRange(c, 2, c.length);
                    int cur = java.util.Arrays.asList(presets).indexOf(c[1]);
                    indentNext = true;
                    choice(rowText(c[0].isEmpty() ? a.getString(R.string.packs_preset) : c[0], null), presets, cur, k -> {
                        ModPacks.select(a.prefs, f, c[0], presets[k]);
                        ModPacks.writeEnabled(base, a.prefs);
                        a.markPacksChanged();
                    });
                }
            indentNext = true;
            submenu(R.string.packs_details, 0, "", () -> openPage(() -> packDetailPage(f)));
        }
        if (a.packsChanged) {
            note(a.getString(R.string.packs_restart_note));
            note(a.getString(R.string.packs_title_hint));
            addAction(rows, R.string.packs_restart, a::restartForPacks);
        }
    }

    private void packDetailPage(String folder) {
        java.io.File base = a.baseDir();
        ModPacks.Info info = ModPacks.describe(base, a.gameDir(), a.prefs, folder);
        pageTitle(info.name);
        if (!info.description.isEmpty()) note(info.description);
        StringBuilder b = new StringBuilder(info.summary(a));
        if (info.hasPatches) b.append("\n").append(a.getString(R.string.packs_groups, info.groups, info.groupsOther));
        if (info.versions != null) b.append("\n").append(a.getString(R.string.packs_versions, info.versions));
        if (info.contentAdded > 0) {
            b.append("\n").append(a.getResources().getQuantityString(R.plurals.packs_added, info.contentAdded, info.contentAdded));
            for (String f : info.added) b.append("\n   ").append(f);
        }
        for (String u : info.unsupported) b.append("\n").append(a.getString(R.string.packs_unsupported, u));
        for (String n : info.notes) b.append("\n").append(n);
        b.append("\n").append(a.getString(R.string.packs_folder, folder));
        note(b.toString());
        addAction(rows, R.string.packs_delete, () -> new GameDialog(getContext()).title(info.name).message(R.string.packs_delete_confirm)
                .button(R.string.opt_cancel, null)
                .button(R.string.packs_delete, () -> {
                    boolean wasOn = a.prefs.getBoolean("pack_on_" + folder, false);
                    ModPacks.delete(base, a.prefs, folder);
                    if (wasOn) a.markPacksChanged();
                    page = pageStack.poll();
                    fill();
                }).show());
    }

    private void cheatsPage() {
        pageTitle(R.string.opt_cheats);
        note(a.getString(R.string.cheats_about));
        submenu(R.string.cheat_items, R.string.cheat_items_hint, "", () -> a.cheat(MainActivity.CHEAT_ITEMS, R.string.cheat_items_done));
        submenu(R.string.cheat_sword, R.string.cheat_sword_hint, "", () -> a.cheat(MainActivity.CHEAT_SWORD, R.string.cheat_sword_done));
        submenu(R.string.cheat_stats, R.string.cheat_stats_hint, "", () -> a.cheat(MainActivity.CHEAT_STATS, R.string.cheat_stats_done));
        submenu(R.string.cheat_bags, R.string.cheat_bags_hint, "", () -> a.cheat(MainActivity.CHEAT_BAGS, R.string.cheat_bags_done));
        int[] inf = {R.string.cheat_inf_health, R.string.cheat_inf_magic, R.string.cheat_inf_ammo, R.string.cheat_inf_rupees,
                     R.string.cheat_inf_bags, R.string.cheat_inf_breath, R.string.cheat_one_hit};
        int[] infHint = {0, 0, 0, 0, R.string.cheat_inf_bags_hint, R.string.cheat_inf_breath_hint, R.string.cheat_one_hit_hint};
        String[] infKey = {"cheat_inf_health", "cheat_inf_magic", "cheat_inf_ammo", "cheat_inf_rupees", "cheat_inf_bags", "cheat_inf_breath",
                           "cheat_one_hit"};
        for (int i = 0; i < inf.length; i++) {
            String key = infKey[i];
            toggle(inf[i], infHint[i], a.prefs.getBoolean(key, false), on -> {
                a.prefs.edit().putBoolean(key, on).apply();
                Native.setOption(key, on ? 1 : 0);
            });
        }
        note(a.getString(R.string.cheats_story_about));
        int[][] story = {{R.string.cheat_songs, MainActivity.CHEAT_SONGS, R.string.cheat_songs_done},
                         {R.string.cheat_triforce, MainActivity.CHEAT_TRIFORCE, R.string.cheat_triforce_done},
                         {R.string.cheat_dungeon, MainActivity.CHEAT_DUNGEON, R.string.cheat_dungeon_done},
                         {R.string.cheat_key, MainActivity.CHEAT_KEY, R.string.cheat_key_done}};
        for (int[] c : story)
            submenu(c[0], 0, "", () -> new GameDialog(getContext()).title(c[0]).message(R.string.cheats_story_confirm)
                    .button(R.string.opt_cancel, null)
                    .button(R.string.cheat_apply, () -> a.cheat(c[1], c[2])).show());
    }

    // faster running, swimming, climbing or sidling: its speed, and below it (when on) when it applies and how L3 works
    private void moveSpeed(String mod, int label, int hint, String[] values, int cur) {
        choice(label, hint, values, cur, k -> {
            a.prefs.edit().putInt(mod + "_speed", MainActivity.RUN_SPEEDS[k]).apply();
            Native.setOption(mod + "_speed", MainActivity.RUN_SPEEDS[k]);
        });
        if (MainActivity.RUN_SPEEDS[cur] == 100) return;
        boolean withL3 = a.prefs.getBoolean(mod + "_l3", false), hold = a.prefs.getBoolean(mod + "_l3_hold", false);
        String[] when = {a.getString(R.string.opt_mod_run_always), a.getString(R.string.opt_mod_run_with_l3)};
        indentNext = true;
        choice(R.string.opt_mod_run_when, 0, when, withL3 ? 1 : 0, k -> a.setMoveL3(mod, k == 1, hold));
        if (withL3) {
            String[] how = {a.getString(R.string.opt_mod_run_l3_switch), a.getString(R.string.opt_mod_run_l3_hold)};
            indentNext = true;
            choice(R.string.opt_mod_run_l3, 0, how, hold ? 1 : 0, k -> a.setMoveL3(mod, true, k == 1));
        }
    }

    // the Controls tab: the touch style and the layout editor, then a page per topic (no on/off for the touch
    // controls: a controller in use hides them, a touch brings them back, MainActivity.controllerUsed)
    private void controls() {
        if (MainActivity.NEW_TOUCH_CONTROLS) {
            String[] styles = {a.getString(R.string.controls_classic), a.getString(R.string.controls_flexible)};
            choice(R.string.opt_controls_style, R.string.opt_controls_style_hint, styles, a.prefs.getInt("controls_style", 0), i -> {
                a.prefs.edit().putInt("controls_style", i).apply();
                a.applyControlsAppearance();
            });
            submenu(R.string.opt_controls_edit, R.string.opt_controls_edit_hint, "", () -> {
                a.editControls();
                dismiss();  // the game, to arrange the controls on
            });
        }
        // (touch and controller alike: the device's or the controller's sensors and motors)
        // (a page with the source while a controller and this device both have them)
        if (a.sourceChoice(true)) submenu(R.string.opt_motion, R.string.opt_motion_hint, sourceValue("motion", "motion_from"), () -> openPage(this::motionPage));
        else toggle(R.string.opt_motion, R.string.opt_motion_hint, a.prefs.getBoolean("motion", true), a::setMotion);
        if (a.sourceChoice(false)) submenu(R.string.opt_rumble, R.string.opt_rumble_hint, sourceValue("rumble", "rumble_on"), () -> openPage(this::rumblePage));
        else toggle(R.string.opt_rumble, R.string.opt_rumble_hint, a.prefs.getBoolean("rumble", true), a::setRumble);
        submenu(R.string.opt_page_touch, R.string.opt_page_touch_hint, "", () -> openPage(this::touchPage));
        if (MainActivity.NEW_TOUCH_CONTROLS)
            submenu(R.string.opt_item_ring, R.string.opt_page_ring_hint,
                    a.getString(a.prefs.getBoolean("item_ring", true) ? R.string.opt_on : R.string.opt_off), () -> openPage(this::ringPage));
        submenu(R.string.opt_page_camera, R.string.opt_page_camera_hint, "", () -> openPage(this::cameraPage));
        if (MainActivity.NEW_TOUCH_CONTROLS) {
            submenu(R.string.opt_page_minigames, R.string.opt_page_minigames_hint, "", () -> openPage(this::minigamesPage));
            submenu(R.string.opt_page_hud, R.string.opt_page_hud_hint, "", () -> openPage(this::hudPage));
        }
        submenu(R.string.opt_page_controller, R.string.opt_page_controller_hint, "", () -> openPage(this::controllerPage));
    }

    private String[] sources(boolean both) {
        String[] l = {a.getString(R.string.opt_src_auto), a.getString(R.string.opt_src_device), a.getString(R.string.opt_src_controller),
                a.getString(R.string.opt_src_both)};
        return both ? l : java.util.Arrays.copyOf(l, 3);
    }

    /** the Controls tab's value of Motion controls / Rumble with a source choice: OFF, or where from */
    private String sourceValue(String on, String from) {
        if (!a.prefs.getBoolean(on, true)) return a.getString(R.string.opt_off);
        String[] l = sources(true);
        return l[Math.max(0, Math.min(l.length - 1, a.prefs.getInt(from, MainActivity.SRC_AUTO)))];
    }

    // Controls > Motion controls (a controller and this device both have motion sensors): on / off, and whose
    private void motionPage() {
        pageTitle(R.string.opt_motion);
        boolean on = a.prefs.getBoolean("motion", true);
        toggle(R.string.opt_motion, R.string.opt_motion_hint, on, a::setMotion);
        if (on) choice(R.string.cd_motion_from, R.string.opt_motion_from_hint, sources(false),
                a.prefs.getInt("motion_from", MainActivity.SRC_AUTO), a::setMotionFrom);
    }

    // Controls > Rumble (a controller and this device both rumble): on / off, and which (both at once too)
    private void rumblePage() {
        pageTitle(R.string.opt_rumble);
        boolean on = a.prefs.getBoolean("rumble", true);
        toggle(R.string.opt_rumble, R.string.opt_rumble_hint, on, a::setRumble);
        if (on) choice(R.string.cd_rumble_on, R.string.opt_rumble_on_hint, sources(true),
                a.prefs.getInt("rumble_on", MainActivity.SRC_AUTO), a::setRumbleOn);
    }

    // Controls > Touch: the touch controls' size, sticks, buttons and icons
    private void touchPage() {
        pageTitle(R.string.opt_page_touch);
        String[] sizes = new String[MainActivity.CONTROL_SIZES.length];
        int cur = 2;
        for (int i = 0; i < sizes.length; i++) {
            sizes[i] = Math.round(MainActivity.CONTROL_SIZES[i] * 100) + "%";
            if (Math.abs(MainActivity.CONTROL_SIZES[i] - a.prefs.getFloat("controls_scale", 1f)) < 0.01f) cur = i;
        }
        choice(R.string.opt_onscreen_size, 0, sizes, cur, i -> {
            a.prefs.edit().putFloat("controls_scale", MainActivity.CONTROL_SIZES[i]).apply();
            a.applyControlsAppearance();
        });
        toggle(R.string.opt_menu_overlay, R.string.opt_menu_overlay_hint, a.prefs.getBoolean("menu_overlay", false), on -> {
            a.setMenuOverlay(on);
            showPlus();
            if (on && inInventory) closeToGame();  // (no options page in the inventory any more: back to the items)
        });
        if (!MainActivity.NEW_TOUCH_CONTROLS) return;
        toggle(R.string.opt_stick_follows, R.string.opt_stick_follows_hint, a.prefs.getBoolean("stick_follows", true), on -> {
            a.prefs.edit().putBoolean("stick_follows", on).apply();
            a.applyControlsAppearance();
        });
        if (a.prefs.getInt("controls_style", 0) == 1) {
            toggle(R.string.opt_show_sticks, R.string.opt_show_sticks_hint, a.prefs.getBoolean("controls_show_sticks", true), on -> {
                a.prefs.edit().putBoolean("controls_show_sticks", on).apply();
                a.applyControlsAppearance();
            });
            toggle(R.string.opt_context_buttons, R.string.opt_context_buttons_hint, a.prefs.getBoolean("context_buttons", false), on -> {
                a.prefs.edit().putBoolean("context_buttons", on).apply();
                a.applyControlsAppearance();
            });
            // the bow, boomerang, hookshot and grappling hook by taps (ControlsView: item buttons by tap), or held as in the game
            String[] itemUse = {a.getString(R.string.opt_item_tap_tap), a.getString(R.string.opt_item_tap_hold)};
            choice(R.string.opt_item_tap, R.string.opt_item_tap_hint, itemUse, a.prefs.getBoolean("item_tap", true) ? 0 : 1, i -> {
                a.prefs.edit().putBoolean("item_tap", i == 0).apply();
                a.applyControlsAppearance();
            });
            indentNext = true;
            toggle(R.string.opt_item_tap_focus, R.string.opt_item_tap_focus_hint, a.prefs.getBoolean("item_tap_focus", true), on -> {
                a.prefs.edit().putBoolean("item_tap_focus", on).apply();
                a.applyControlsAppearance();
            });
        }
        // a talk held by touch (flexible with context buttons: a tap goes on there): goes on up to a question; fast forward with it
        if (a.prefs.getInt("controls_style", 0) == 1 && a.prefs.getBoolean("context_buttons", false) && a.prefs.getBoolean("minigame_touch", true)) {
            boolean hold = a.prefs.getBoolean("talk_hold", true);
            toggle(R.string.opt_talk_hold, R.string.opt_talk_hold_hint, hold, on -> {
                a.prefs.edit().putBoolean("talk_hold", on).apply();
                a.applyControlsAppearance();
            });
            if (hold) {
                indentNext = true;
                toggle(R.string.opt_talk_hold_ff, R.string.opt_talk_hold_ff_hint, a.prefs.getBoolean("talk_hold_ff", false), on -> {
                    a.prefs.edit().putBoolean("talk_hold_ff", on).apply();
                    a.applyControlsAppearance();
                });
            }
        }
        toggle(R.string.opt_link_poses, R.string.opt_link_poses_hint, a.prefs.getBoolean("link_poses", true), on -> {
            a.prefs.edit().putBoolean("link_poses", on).apply();
            a.applyControlsAppearance();
        });
        String[] defeat = {a.getString(R.string.opt_zl_defeat_release), a.getString(R.string.opt_zl_defeat_next)};
        choice(R.string.opt_zl_defeat, R.string.opt_zl_defeat_hint, defeat, a.prefs.getInt("zl_after_defeat", 0), i -> {
            a.prefs.edit().putInt("zl_after_defeat", i).apply();
            a.applyControlsAppearance();
        });
    }

    // Controls > Item ring: on / off, the game while it is open, the item buttons' names
    private void ringPage() {
        pageTitle(R.string.opt_item_ring);
        boolean ring = a.prefs.getBoolean("item_ring", true);
        toggle(R.string.opt_item_ring, R.string.opt_item_ring_hint, ring, on -> {
            a.prefs.edit().putBoolean("item_ring", on).apply();
            Native.setOption("ring_no_baton", on ? 1 : 0);
            fill();
            // d-pad directions taken off for a jump: only the ring had their Wind Waker / cannon / salvage arm
            java.util.List<Integer> off = a.inputMapper().ringTakenOff();
            if (!on && !off.isEmpty()) {
                StringBuilder w = new StringBuilder();
                for (int j : off) w.append(w.length() == 0 ? "" : ", ").append(buttonNames()[j]);
                new GameDialog(getContext()).title(R.string.opt_item_ring).message(a.getString(R.string.cs_ring_off_warn, w))
                        .button(R.string.opt_ok, null).show();
            }
        });
        if (ring) {
            indentNext = true;
            choice(R.string.opt_item_ring_pace, 0, a.getResources().getStringArray(R.array.item_ring_paces),
                    a.prefs.getInt("item_ring_pace", 1), k -> a.prefs.edit().putInt("item_ring_pace", k).apply());
        }
        // the item buttons' names on the game's HUD and drop slots (MainActivity.applySlotLabels)
        choice(R.string.opt_slot_labels, R.string.opt_slot_labels_hint, a.getResources().getStringArray(R.array.slot_labels),
                a.prefs.getInt("slot_labels", 0), a::setSlotLabelMode);
    }

    // Controls > Camera: up / down, the walking direction and its camera swing, the right half's swipe camera (Flexible)
    private void cameraPage() {
        pageTitle(R.string.opt_page_camera);
        choice(R.string.opt_invert_camera_y, R.string.opt_invert_camera_y_hint, a.getResources().getStringArray(R.array.invert_camera_y_modes),
               a.prefs.getInt("invert_camera_y_mode", 0), a::setInvertCameraY);
        modToggle(0, R.string.opt_mod_direct_camera, R.string.opt_mod_direct_camera_hint);
        String[] camSpeeds = new String[MainActivity.CAMERA_SPEEDS.length];
        int camCur = 1;
        for (int k = 0; k < camSpeeds.length; k++) {
            camSpeeds[k] = String.format(java.util.Locale.ROOT, "%.1f×", MainActivity.CAMERA_SPEEDS[k] / 100f);
            if (MainActivity.CAMERA_SPEEDS[k] == a.prefs.getInt("mod_camera_speed", 100)) camCur = k;
        }
        indentNext = true;  // a setting of the mod above
        choice(R.string.opt_mod_camera_speed, 0, camSpeeds, camCur, k -> {
            a.prefs.edit().putInt("mod_camera_speed", MainActivity.CAMERA_SPEEDS[k]).apply();
            Native.setOption("mod_camera_speed", MainActivity.CAMERA_SPEEDS[k]);
        });
        modToggle(1, R.string.opt_mod_first_person, 0);
        choice(R.string.opt_steer, R.string.opt_steer_hint, a.getResources().getStringArray(R.array.steer_modes),
               a.prefs.getInt("steer_mode", 0), a::setSteerMode);
        if (a.prefs.getInt("steer_mode", 0) == 2) {
            String[] degs = new String[20];
            for (int k = 0; k < degs.length; k++) degs[k] = (k + 1) + "°";
            indentNext = true;
            choice(R.string.opt_steer_resync, R.string.opt_steer_resync_hint, degs, a.prefs.getInt("steer_resync", 1) - 1, k -> {
                a.prefs.edit().putInt("steer_resync", k + 1).apply();
                Native.setOption("steer_resync", k + 1);
            });
        }
        if (a.prefs.getInt("steer_mode", 0) != 0) {  // needs Link to keep his direction while it turns
            indentNext = true;
            toggle(R.string.opt_steer_swing, R.string.opt_steer_swing_hint, a.prefs.getBoolean("steer_swing", false),
                    on -> a.setMod("steer_swing", on));
            if (a.prefs.getBoolean("steer_swing", false)) {
                String[] waits = new String[MainActivity.SWING_DELAYS.length];
                int curWait = 1;
                for (int k = 0; k < waits.length; k++) {
                    waits[k] = String.format(java.util.Locale.ROOT, "%.2f", MainActivity.SWING_DELAYS[k] / 1000f).replaceAll("\\.?0+$", "") + " s";
                    if (MainActivity.SWING_DELAYS[k] == a.prefs.getInt("steer_swing_delay", 1000)) curWait = k;
                }
                indentNext = true;
                choice(R.string.opt_steer_swing_delay, R.string.opt_steer_swing_delay_hint, waits, curWait, k -> {
                    a.prefs.edit().putInt("steer_swing_delay", MainActivity.SWING_DELAYS[k]).apply();
                    Native.setOption("steer_swing_delay", MainActivity.SWING_DELAYS[k]);
                });
            }
        }
        if (!MainActivity.NEW_TOUCH_CONTROLS || a.prefs.getInt("controls_style", 0) != 1) return;
        String[] cams = {a.getString(R.string.touch_camera_stick), a.getString(R.string.touch_camera_swipe)};
        choice(R.string.opt_touch_camera, R.string.opt_touch_camera_hint, cams, a.prefs.getInt("touch_camera", 0), i -> {
            a.prefs.edit().putInt("touch_camera", i).apply();
            a.applyControlsAppearance();
        });
        if (a.prefs.getInt("touch_camera", 0) == 1) {
            String[] speeds = new String[MainActivity.SWIPE_SPEEDS.length];
            int cur = 7;
            for (int i = 0; i < speeds.length; i++) {
                int rel = Math.round((MainActivity.SWIPE_SPEEDS[i] - MainActivity.SWIPE_SPEED_DEFAULT) * 100);
                speeds[i] = rel > 0 ? "+" + rel : String.valueOf(rel);
                if (Math.abs(MainActivity.SWIPE_SPEEDS[i] - a.prefs.getFloat("swipe_speed", MainActivity.SWIPE_SPEED_DEFAULT)) < 0.01f) cur = i;
            }
            indentNext = true;
            choice(R.string.opt_swipe_speed, 0, speeds, cur, i -> {
                a.prefs.edit().putFloat("swipe_speed", MainActivity.SWIPE_SPEEDS[i]).apply();
                a.applyControlsAppearance();
            });
            String[] boat = new String[MainActivity.SWIPE_BOAT.length];
            int bcur = 0;
            for (int i = 0; i < boat.length; i++) {
                boat[i] = String.format(java.util.Locale.ROOT, "%.2f×", MainActivity.SWIPE_BOAT[i] / 100f);
                if (MainActivity.SWIPE_BOAT[i] == a.prefs.getInt("swipe_boat", 100)) bcur = i;
            }
            indentNext = true;
            choice(R.string.opt_swipe_boat, R.string.opt_swipe_boat_hint, boat, bcur, i -> {
                a.prefs.edit().putInt("swipe_boat", MainActivity.SWIPE_BOAT[i]).apply();
                Native.setOption("swipe_boat", MainActivity.SWIPE_BOAT[i]);
            });
        }
    }

    // Controls > Minigames & hints
    private void minigamesPage() {
        pageTitle(R.string.opt_page_minigames);
        toggle(R.string.opt_minigame_touch, R.string.opt_minigame_touch_hint, a.prefs.getBoolean("minigame_touch", true), on -> {
            a.prefs.edit().putBoolean("minigame_touch", on).apply();
            a.applyControlsAppearance();
        });
        toggle(R.string.opt_conduct_pads, R.string.opt_conduct_pads_hint, a.prefs.getBoolean("conduct_pads", true), on -> {
            a.prefs.edit().putBoolean("conduct_pads", on).apply();
            a.applyControlsAppearance();
        });
        toggle(R.string.opt_touch_hints, R.string.opt_touch_hints_hint, GameHint.enabled(), GameHint::setEnabled);
    }

    // Controls > Screen (HUD): hearts and rupees shown only when needed, the large Picto Box view (with context buttons)
    private void hudPage() {
        pageTitle(R.string.opt_page_hud);
        toggle(R.string.opt_hearts_combat, R.string.opt_hearts_combat_hint, a.prefs.getBoolean("hearts_combat", false), on -> {
            a.prefs.edit().putBoolean("hearts_combat", on).apply();
            a.applyControlsAppearance();
        });
        toggle(R.string.opt_rupees_change, R.string.opt_rupees_change_hint, a.prefs.getBoolean("rupees_on_change", false), on -> {
            a.prefs.edit().putBoolean("rupees_on_change", on).apply();
            a.applyControlsAppearance();
        });
        if (a.prefs.getInt("controls_style", 0) == 1 && a.prefs.getBoolean("context_buttons", false))
            toggle(R.string.opt_picto_big, R.string.opt_picto_big_hint, a.prefs.getBoolean("picto_big", true), on -> {
                a.prefs.edit().putBoolean("picto_big", on).apply();
                a.applyControlsAppearance();
            });
    }

    // Controls > Controller: the setup steps (ControllerSetup) and buttons for the controller in use, motion, rumble
    private void controllerPage() {
        pageTitle(R.string.opt_page_controller);
        submenu(R.string.cs_title, R.string.cs_menu_hint, "", () -> {
            if (!a.startControllerSetup(this::fill))
                new GameDialog(getContext()).title(R.string.cs_title).message(R.string.cs_no_controller).button(R.string.opt_ok, null).show();
        });
        submenu(R.string.opt_buttons, R.string.opt_buttons_hint,
                a.getString(a.inputMapper().isDefaultMap() ? R.string.opt_buttons_default : R.string.opt_buttons_custom),
                () -> openPage(this::buttonsPage));
        if (!ControllerInfo.connected(null).isEmpty())
            submenu(R.string.cd_title, R.string.cd_hint, "", () -> openPage(detailsPage));
    }

    // Controls > Controller > Controller details: every connected controller (the one in use first), as Android
    // reports it (ControllerInfo); the page follows controllers connecting and going (devices)
    private final Runnable detailsPage = this::controllerDetailsPage;

    private void controllerDetailsPage() {
        pageTitle(R.string.cd_title);
        InputDevice inUse = a.controllerInUse();
        java.util.List<InputDevice> pads = ControllerInfo.connected(inUse);
        for (InputDevice d : pads) {
            if (pads.size() > 1) heading(d.getName(), true);
            for (ControllerInfo.Section s : ControllerInfo.describe(a, d, inUse != null && d.getId() == inUse.getId())) {
                heading(s.title, false);
                for (String[] r : s.rows) rowText(r[0], r[1]);
            }
        }
    }

    private void heading(String text, boolean big) {
        TextView t = GameUi.text(getContext(), gameStyle);
        t.setText(text);
        t.setTextColor(INK);
        sp(t, big ? 20 : 17);
        t.setTypeface(Typeface.create(big ? "sans-serif-black" : "sans-serif-medium", Typeface.BOLD));
        t.setPadding(px(12), px(big ? 18 : 12), px(12), px(6));
        rows.addView(t, new LinearLayout.LayoutParams(-1, -2));
    }

    // a controller connecting or going: the Controller page shows or hides Controller details, the details page
    // updates (and closes with the last controller)
    private final android.hardware.input.InputManager.InputDeviceListener devices = new android.hardware.input.InputManager.InputDeviceListener() {
        @Override public void onInputDeviceAdded(int id) { changed(); }
        @Override public void onInputDeviceRemoved(int id) { changed(); }
        @Override public void onInputDeviceChanged(int id) { changed(); }

        private void changed() {
            if (page == detailsPage && ControllerInfo.connected(null).isEmpty()) onBackPressed();
            else if (page != null) fill();
        }
    };

    // Mods > Jump anywhere / Moon jump > Controller button (on touch they have buttons of their own)
    private void ownButtonRow(int own) {
        InputMapper m = a.inputMapper();
        LinearLayout r = rowText(a.getString(R.string.opt_jump_button), a.getString(R.string.opt_jump_button_hint));
        buttonValue(r, m.map[own]);
        r.setOnClickListener(x -> pickButton(own, buttonNames()[own]));
    }

    /** the Buttons page's names: the Wii U buttons, then Jump anywhere and the moon jump (InputMapper.WIIU order) */
    private String[] buttonNames() {
        String[] wiiu = a.getResources().getStringArray(R.array.wiiu_buttons);
        String[] n = java.util.Arrays.copyOf(wiiu, InputMapper.WIIU.length);
        n[InputMapper.OWN_JUMP] = a.getString(R.string.opt_mod_jump);
        n[InputMapper.OWN_MOON] = a.getString(R.string.cheat_moon_jump);
        return n;
    }

    private void buttonValue(LinearLayout r, int code) {
        TextView v = GameUi.text(getContext(), gameStyle);
        v.setText(InputMapper.buttonName(code) + "  ›");
        v.setTextColor(INK);
        sp(v, 16);
        v.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        r.addView(v, new LinearLayout.LayoutParams(-2, -2));
    }

    // waits for the controller button for entry i (InputMapper.WIIU order); one of our own inputs on a button the game
    // uses in play asks first: move that Wii U button away (InputMapper.moveTarget), or take an item button off
    private void pickButton(int i, String name) {
        InputMapper m = a.inputMapper();
        new GameDialog(getContext()).title(name)
                .message(a.getString(R.string.opt_buttons_press, name, InputMapper.buttonName(m.map[i])))
                .button(R.string.opt_cancel, null)
                .captureButton(code -> {
                    java.util.List<Integer> clash = InputMapper.own(i) ? m.clashes(code) : java.util.Collections.emptyList();
                    if (clash.isEmpty()) {
                        if (InputMapper.own(i)) a.assignOwnButton(i, code, false);
                        else a.assignButton(i, code);
                        fill();
                        return;
                    }
                    askClash(i, name, code, clash);
                })
                .show();
    }

    private void askClash(int i, String name, int code, java.util.List<Integer> clash) {
        InputMapper m = a.inputMapper();
        String[] names = buttonNames();
        StringBuilder w = new StringBuilder();
        boolean ring = a.itemRingOn(), items = true, dpad = false;
        for (int j : clash) {
            w.append(w.length() == 0 ? "" : ", ").append(names[j]);
            if (!InputMapper.removable(j, ring)) items = false;
            if (InputMapper.ringCovers(j)) dpad = true;
        }
        int target = m.moveTarget(i, code);
        String msg = a.getString(R.string.opt_jump_clash, InputMapper.buttonName(code), w, name);
        msg += "\n\n" + (target != 0 ? a.getString(R.string.opt_jump_clash_move, w, InputMapper.buttonName(target))
                : a.getString(R.string.opt_jump_clash_nofree, w));
        if (items) msg += "\n\n" + a.getString(dpad ? R.string.opt_jump_clash_remove_ring : R.string.opt_jump_clash_remove, w);
        // an item button: it can go onto L1 / d-pad down instead (InputMapper.setExtraItem), where none is yet
        java.util.List<Integer> extra = new java.util.ArrayList<>();
        if (InputMapper.itemsOnly(clash))
            for (int c : InputMapper.EXTRA_ITEM_CODES)
                if (c != code && m.itemOn(c) < 0) extra.add(c);
        if (!extra.isEmpty()) {
            msg = a.getString(R.string.opt_jump_clash, InputMapper.buttonName(code), w, name)
                    + "\n\n" + a.getString(R.string.opt_jump_clash_extra, w);
            if (items) msg += "\n\n" + a.getString(dpad ? R.string.opt_jump_clash_remove_ring : R.string.opt_jump_clash_remove, w);
        }
        GameDialog d = new GameDialog(getContext()).title(name).message(msg);
        for (int c : extra)
            d.button(c == KeyEvent.KEYCODE_BUTTON_L1 ? R.string.opt_jump_move_l1 : R.string.opt_jump_move_down,
                    () -> { a.assignOwnMovingItem(i, code, c); fill(); });
        if (target != 0 && extra.isEmpty()) d.button(R.string.opt_jump_move, () -> { a.assignOwnButton(i, code, false); fill(); });
        if (items) d.button(R.string.opt_jump_remove, () -> { a.assignOwnButton(i, code, true); fill(); });
        d.button(R.string.opt_cancel, null).show();
    }

    // controller buttons: each Wii U button, Jump anywhere and the moon jump, and the controller button that presses it
    private void buttonsPage() {
        pageTitle(R.string.opt_buttons);
        String pad = a.padName();
        note((pad != null ? a.getString(R.string.cs_buttons_for, pad) + "\n\n" : "") + a.getString(R.string.opt_buttons_note));
        InputMapper m = a.inputMapper();
        // L1 / d-pad down as an item button: off, X, Y, R (InputMapper.setExtraItem)
        String[] wiiu = a.getResources().getStringArray(R.array.wiiu_buttons);
        String[] items = {a.getString(R.string.opt_off), wiiu[2], wiiu[3], wiiu[5]};
        int[][] extra = {{KeyEvent.KEYCODE_BUTTON_L1, R.string.opt_extra_l1, R.string.opt_extra_l1_hint},
                {KeyEvent.KEYCODE_DPAD_DOWN, R.string.opt_extra_down, R.string.opt_extra_down_hint}};
        for (int[] e : extra) {
            int on = m.itemOn(e[0]), cur = 0;
            for (int k = 0; k < 3; k++) if (InputMapper.ITEM_INDEX[k] == on) cur = k + 1;
            choice(e[1], e[2], items, cur, v -> a.setExtraItem(e[0], v == 0 ? -1 : InputMapper.ITEM_INDEX[v - 1]));
        }
        String[] names = buttonNames();
        for (int i = 0; i < InputMapper.WIIU.length; i++) {
            final int n = i;
            LinearLayout r = rowText(names[i], null);
            buttonValue(r, m.map[i]);
            r.setOnClickListener(x -> pickButton(n, names[n]));
        }
        submenu(R.string.opt_buttons_reset, 0, "", () -> { a.resetButtons(); fill(); });
    }

    private static int indexOf(String[] arr, String v, int fallback) {
        for (int i = 0; i < arr.length; i++)
            if (arr[i].equals(v)) return i;
        return fallback;
    }

    // ---- rows
    interface IntSetter { void set(int v); }
    interface BoolSetter { void set(boolean v); }

    private boolean indentNext;

    private LinearLayout row(int label, int hint) {
        return rowText(a.getString(label), hint != 0 ? a.getString(hint) : null);
    }

    private LinearLayout rowText(String label, String hint) {
        LinearLayout r = new LinearLayout(getContext());
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(px(18), px(10), px(14), px(10));
        r.setMinimumHeight(px(58));
        r.setFocusable(true);
        if (gameStyle) r.setBackground(pressed(new GameUi.OptionCursor()));  // (on the paper, as the game's Options; focused: FocusCursor)
        else r.setBackground(states(slot(0x1F000000, 0), slot(GLOW, YELLOW)));
        LinearLayout text = new LinearLayout(getContext());
        text.setOrientation(LinearLayout.VERTICAL);
        TextView t = GameUi.text(getContext(), gameStyle);
        t.setText(label);
        t.setTextColor(gameStyle ? GameUi.OPTION_TEXT : INK);
        sp(t, 18);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        text.addView(t);
        if (hint != null) {
            TextView h = GameUi.text(getContext(), gameStyle);
            h.setText(hint);
            h.setTextColor(gameStyle ? GameUi.OPTION_HINT : HINT);
            sp(h, 13);
            text.addView(h);
        }
        LinearLayout.LayoutParams tlp = new LinearLayout.LayoutParams(0, -2, 1);
        tlp.rightMargin = px(12);  // (a long hint ran up to the switch's edge)
        r.addView(text, tlp);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = px(8);
        if (indentNext) lp.leftMargin = px(36);
        indentNext = false;
        rows.addView(r, lp);
        return r;
    }

    private void toggle(int label, int hint, boolean on, BoolSetter set) {
        toggle(row(label, hint), on, set);
    }

    private void toggle(LinearLayout r, boolean on, BoolSetter set) {
        TextView pill = GameUi.pill(getContext(), on, gameStyle);
        sp(pill, 16);
        r.addView(pill, new LinearLayout.LayoutParams(px(gameStyle ? 104 : 92), px(gameStyle ? 46 : 38)));
        r.setOnClickListener(v -> { set.set(!on); fill(); });
    }

    private void choice(int label, int hint, String[] values, int cur, IntSetter set) {
        choice(row(label, hint), values, cur, set);
    }

    private void choice(LinearLayout r, String[] values, int cur, IntSetter set) {
        int idx = Math.max(0, Math.min(values.length - 1, cur));
        TextView prev = arrow("◀"), next = arrow("▶");
        TextView value = GameUi.text(getContext(), gameStyle);
        value.setText(values[idx]);
        value.setTextColor(INK);
        sp(value, 16);
        value.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        value.setGravity(Gravity.CENTER);
        value.setMinWidth(px(150));
        if (gameStyle) {  // the game's Options: its value button between its arrows
            prev.setText("");
            next.setText("");
            prev.setBackground(new GameUi.OptionArrow(getContext(), true));
            next.setBackground(new GameUi.OptionArrow(getContext(), false));
            value.setTextColor(GameUi.OPTION_BUTTON_TEXT);
            value.setBackground(new GameUi.GameValueButton(getContext(), true, false));
            value.setPadding(px(16), 0, px(20), px(4));
            value.setMinHeight(px(44));
        }
        r.addView(prev, new LinearLayout.LayoutParams(px(40), px(40)));
        r.addView(value, new LinearLayout.LayoutParams(-2, -2));
        r.addView(next, new LinearLayout.LayoutParams(px(40), px(40)));
        Runnable dec = () -> { set.set((idx + values.length - 1) % values.length); fill(); };
        Runnable inc = () -> { set.set((idx + 1) % values.length); fill(); };
        prev.setOnClickListener(v -> dec.run());
        next.setOnClickListener(v -> inc.run());
        r.setOnClickListener(v -> inc.run());
        r.setOnKeyListener((v, code, e) -> {
            if (e.getAction() != KeyEvent.ACTION_DOWN) return false;
            if (code == KeyEvent.KEYCODE_DPAD_LEFT) { dec.run(); return true; }
            if (code == KeyEvent.KEYCODE_DPAD_RIGHT) { inc.run(); return true; }
            return false;
        });
    }

    private void submenu(int label, int hint, String value, Runnable open) {
        submenu(row(label, hint), value, open);
    }

    private void submenu(String label, String hint, String value, Runnable open) {
        submenu(rowText(label, hint), value, open);
    }

    private void submenu(LinearLayout r, String value, Runnable open) {
        TextView v = GameUi.text(getContext(), gameStyle);
        v.setText(value.isEmpty() ? "›" : value + "  ›");
        v.setTextColor(INK);
        sp(v, 16);
        v.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        if (gameStyle) {  // the game's value button with its arrow at the end: it leads on (a press nudges it that way)
            v.setText(value);
            v.setTextColor(GameUi.OPTION_BUTTON_TEXT);
            v.setGravity(Gravity.CENTER);
            v.setBackground(new GameUi.GameValueButton(getContext(), true, false));
            android.graphics.drawable.Drawable arrow = new GameUi.OptionArrow(getContext(), false, GameUi.OPTION_BUTTON_TEXT);
            arrow.setBounds(0, 0, px(26), px(20));
            v.setCompoundDrawables(null, null, arrow, null);
            v.setCompoundDrawablePadding(value.isEmpty() ? 0 : px(8));
            v.setPadding(px(value.isEmpty() ? 22 : 18), 0, px(20), px(4));
            v.setMinHeight(px(44));
            v.setMinWidth(px(76));
            r.addView(v, new LinearLayout.LayoutParams(-2, -2));
            r.setOnClickListener(x -> v.animate().translationX(px(10)).setDuration(70).withEndAction(() ->
                    v.animate().translationX(0).setDuration(90).withEndAction(open).start()).start());
            return;
        }
        r.addView(v, new LinearLayout.LayoutParams(-2, -2));
        r.setOnClickListener(x -> open.run());
    }

    /** a controller's press in the direction of the view's arrow presses it, as the right button does (a held
     * direction only once) */
    private static void pressOn(View v, int dpad) {
        v.setOnKeyListener((x, code, e) -> {
            if (code != dpad) return false;
            if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) x.performClick();
            return true;
        });
    }

    private TextView arrow(String s) {
        TextView t = GameUi.text(getContext(), gameStyle);
        t.setText(s);
        t.setTextColor(0xFFD9B400);
        sp(t, 20);
        t.setGravity(Gravity.CENTER);
        return t;
    }

    private TextView addAction(LinearLayout col, int label, Runnable run) {
        TextView b = barButton(a.getString(label), v -> run.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, px(54));
        lp.bottomMargin = px(12);
        col.addView(b, lp);
        return b;
    }

    private TextView barButton(String text, View.OnClickListener click) {
        TextView b = GameUi.barButton(getContext(), text, click, gameStyle, gameStyle);  // (the menu's own buttons: green)
        sp(b, 18);
        if (!gameStyle) b.setPadding(px(16), 0, px(16), 0);
        return b;
    }

    private Drawable slot(int fill, int stroke) { return GameUi.slot(getContext(), fill, stroke); }

    private static StateListDrawable states(Drawable normal, Drawable lit) { return GameUi.states(normal, lit); }

    private static StateListDrawable pressed(Drawable lit) {
        StateListDrawable s = new StateListDrawable();
        s.addState(new int[] {android.R.attr.state_pressed}, lit);
        s.addState(new int[] {}, new ColorDrawable(0));
        return s;
    }

    private FocusCursor cursor;

    /** the game's menu cursor (GameUi.MenuCursor) around the focused view, as on the game's own pages; over the whole
     * page, so it follows the rows' scrolling and the tabs */
    private final class FocusCursor extends View {
        private final android.graphics.Rect fr = new android.graphics.Rect();
        private final RectF r = new RectF();

        FocusCursor() {
            super(OptionsMenu.this.getContext());
            setFocusable(false);
        }

        @Override
        protected void onDraw(Canvas canvas) {
            View f = getCurrentFocus();
            if (f == null || isInTouchMode() || !f.isShown() || f == this) return;
            f.getDrawingRect(fr);
            root.offsetDescendantRectToMyCoords(f, fr);
            r.set(fr);
            // tabs and foot plates reach past the screen's edges: their part on it
            float size = Math.min(40 * unit, Math.min(r.width(), r.height()) * 0.6f), o = size * 0.3f;
            r.top = Math.max(r.top, pageArea.top + o);
            r.bottom = Math.min(r.bottom, pageArea.bottom - o);
            if (r.height() <= 0) return;
            size = Math.min(size, r.height() * 0.6f);
            if (GameUi.MenuCursor.draw(getContext(), canvas, r, size, () -> post(this::invalidate))) postInvalidateDelayed(250);
        }
    }
}
