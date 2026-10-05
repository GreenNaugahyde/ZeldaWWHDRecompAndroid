package org.wwhdrecomp.app;

import android.app.Dialog;
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
    private int tab;
    private LinearLayout rows;
    private ScrollView scroll;
    private static final int TABS = 4;
    // Saves, Graphics, Mods, Controls
    private static final GameUi.Palette[] TAB_COLORS = {GameUi.GREEN, GameUi.BLUE, GameUi.AMBER, GameUi.CORAL};
    private final TextView[] tabs = new TextView[TABS];

    OptionsMenu(MainActivity a) {
        super(a, android.R.style.Theme_Translucent_NoTitleBar_Fullscreen);
        this.a = a;
        dp = a.getResources().getDisplayMetrics().density;
        tab = lastTab;
    }

    private int px(float v) { return Math.round(v * dp); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        Window w = getWindow();
        w.setBackgroundDrawable(new ColorDrawable(SLATE));
        w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);
        w.getAttributes().layoutInDisplayCutoutMode = android.view.WindowManager.LayoutParams.LAYOUT_IN_DISPLAY_CUTOUT_MODE_SHORT_EDGES;

        FrameLayout root = new FrameLayout(getContext());
        LinearLayout panel = new LinearLayout(getContext());
        panel.setOrientation(LinearLayout.VERTICAL);
        panel.setBackground(new GameUi.Parchment(getContext()));
        panel.setPadding(px(28), px(18), px(28), px(14));
        FrameLayout.LayoutParams plp = new FrameLayout.LayoutParams(-1, -1);
        plp.setMargins(px(26), px(18), px(26), px(18));
        root.addView(panel, plp);

        // tabs and Back
        LinearLayout top = new LinearLayout(getContext());
        top.setGravity(Gravity.CENTER_VERTICAL);
        int[] names = {R.string.opt_saves, R.string.opt_graphics, R.string.opt_mods, R.string.opt_controls};
        for (int i = 0; i < TABS; i++) {
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
            top.addView(v, lp);
        }
        View spacer = new View(getContext());
        top.addView(spacer, new LinearLayout.LayoutParams(0, 1, 1));
        TextView back = barButton(a.getString(R.string.opt_back), v -> dismiss());
        top.addView(back, new LinearLayout.LayoutParams(px(130), px(46)));
        panel.addView(top, new LinearLayout.LayoutParams(-1, -2));

        // actions on the left, the tab's settings on the right
        LinearLayout body = new LinearLayout(getContext());
        LinearLayout left = new LinearLayout(getContext());
        left.setOrientation(LinearLayout.VERTICAL);
        left.setPadding(0, px(14), px(18), 0);
        addAction(left, R.string.opt_about, a::showLicenses);
        addAction(left, R.string.opt_quit, a::quitApp);
        body.addView(left, new LinearLayout.LayoutParams(px(220), -1));
        scroll = new ScrollView(getContext());
        rows = new LinearLayout(getContext());
        rows.setOrientation(LinearLayout.VERTICAL);
        rows.setPadding(0, px(10), 0, px(10));
        scroll.addView(rows, new FrameLayout.LayoutParams(-1, -2));
        body.addView(scroll, new LinearLayout.LayoutParams(0, -1, 1));
        panel.addView(body, new LinearLayout.LayoutParams(-1, 0, 1));

        TextView hint = new TextView(getContext());
        hint.setText(R.string.opt_pad_hint);
        hint.setTextColor(HINT);
        hint.setTextSize(13);
        hint.setGravity(Gravity.END);
        panel.addView(hint, new LinearLayout.LayoutParams(-1, -2));

        setContentView(root);
        WindowInsetsController ic = w.getInsetsController();  // the window's views exist from here on
        if (ic != null) {
            ic.hide(WindowInsets.Type.systemBars());
            ic.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
        selectTab(tab);
    }

    // sub-dialogs (frame generation, save states...) open on top; show their changes when back
    @Override
    public void onWindowFocusChanged(boolean hasFocus) {
        super.onWindowFocusChanged(hasFocus);
        if (hasFocus) fill();
    }

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_L1: selectTab((tab + TABS - 1) % TABS); return true;
            case KeyEvent.KEYCODE_BUTTON_R1: selectTab((tab + 1) % TABS); return true;
            // by position, as in the game: the bottom button (Android's A) is the Wii U's B, back;
            // the right button (Android's B) is the Wii U's A, select
            case KeyEvent.KEYCODE_BUTTON_A: onBackPressed(); return true;
            case KeyEvent.KEYCODE_BUTTON_MODE: return true;  // closes on release (below): the game opens on release
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

    private void openPage(Runnable p) {
        page = p;
        fill();
        scroll.scrollTo(0, 0);
        if (rows.getChildCount() > 1) rows.getChildAt(1).requestFocus();
    }

    @Override
    public void onBackPressed() {
        if (page != null) {
            page = null;
            fill();
        } else {
            super.onBackPressed();
        }
    }

    private void pageTitle(int title) {
        TextView t = new TextView(getContext());
        t.setText("‹  " + a.getString(title));
        t.setTextColor(INK);
        t.setTextSize(20);
        t.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
        t.setPadding(px(12), px(6), px(12), px(10));
        t.setFocusable(true);
        t.setBackground(states(new ColorDrawable(0), slot(GLOW, YELLOW)));
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
        if (a.prefs.getBoolean("fg_enabled", false) && !err.isEmpty()) note(a.getString(R.string.fg_error, err));
        toggle(R.string.opt_framegen, 0, a.prefs.getBoolean("fg_enabled", false), on -> a.setFrameGen("fg_enabled", on));
        String[] mult = new String[3];
        for (int m = 2; m <= 4; m++) mult[m - 2] = a.getString(R.string.fg_multiplier_item, m, 30 * m);
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

    // a paragraph of explanation on a page
    private void note(String text) {
        TextView t = new TextView(getContext());
        t.setText(text);
        t.setTextColor(INK);
        t.setTextSize(16);
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

    private final Runnable selectHeld = this::dismiss;

    @Override
    public boolean onKeyUp(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) {
            rows.removeCallbacks(selectHeld);
            return true;
        }
        // Home / Guide: on release, so the release doesn't reach the game screen and reopen the menu
        if (code == KeyEvent.KEYCODE_BUTTON_MODE) {
            dismiss();
            return true;
        }
        return super.onKeyUp(code, e);
    }

    @Override
    public void dismiss() {
        rows.removeCallbacks(selectHeld);
        super.dismiss();
    }

    private void selectTab(int t) {
        page = null;
        tab = lastTab = t;
        for (int i = 0; i < TABS; i++) {
            boolean on = i == t;
            GameUi.Palette pal = TAB_COLORS[i];
            tabs[i].setBackground(new GameUi.TabPlate(getContext(), on, pal));
            tabs[i].setTextSize(on ? 26 : 21);
            ((GameUi.OutlinedText) tabs[i]).outline = on ? pal.dark : pal.muted().dark;
        }
        fill();
        scroll.scrollTo(0, 0);
    }

    // ---- the tabs' settings
    private void fill() {
        if (rows == null) return;
        View focused = getCurrentFocus();
        int focusIndex = focused != null && focused.getParent() == rows ? rows.indexOfChild(focused) : -1;
        rows.removeAllViews();
        if (page != null) page.run();
        else if (tab == 0) saves();
        else if (tab == 1) graphics();
        else if (tab == 2) mods();
        else controls();
        if (focusIndex >= 0 && focusIndex < rows.getChildCount()) rows.getChildAt(focusIndex).requestFocus();
    }

    // save states (5 slots with their picture, time and place) and the import / export of saves
    private void saves() {
        for (int slot = 1; slot <= 5; slot++) {
            String[] info = Native.saveSlotInfo(slot);
            boolean used = info[0].equals("1"), compatible = info[1].equals("1");
            String desc = !used ? a.getString(R.string.opt_slot_empty)
                    : info[3].isEmpty() ? info[2] : info[2] + " · " + info[3];
            if (used && !compatible) desc = a.getString(R.string.opt_slot_incompatible, desc);
            LinearLayout r = rowText(a.getString(R.string.opt_slot, slot), desc);
            ImageView pic = new ImageView(getContext());
            pic.setScaleType(ImageView.ScaleType.CENTER_CROP);
            android.graphics.Bitmap bmp = used ? thumbnail(slot) : null;
            if (bmp != null) pic.setImageBitmap(bmp);
            GradientDrawable frame = new GradientDrawable();
            frame.setColor(0x33000000);
            frame.setCornerRadius(px(6));
            pic.setBackground(frame);
            pic.setClipToOutline(true);
            LinearLayout.LayoutParams plp = new LinearLayout.LayoutParams(px(112), px(63));
            plp.rightMargin = px(16);
            r.addView(pic, 0, plp);
            final int n = slot;
            TextView save = smallButton(a.getString(R.string.opt_slot_save), true, v -> {
                if (!used) saveTo(n);
                else new GameDialog(getContext()).title(a.getString(R.string.opt_slot, n)).message(a.getString(R.string.state_overwrite, n))
                        .button(R.string.opt_cancel, null)
                        .button(R.string.opt_slot_save, () -> saveTo(n)).show();
            });
            TextView load = smallButton(a.getString(R.string.opt_slot_load), used && compatible, v -> {
                Native.loadState(n);
                a.showStateResult();
                dismiss();  // see the game come back
            });
            LinearLayout.LayoutParams blp = new LinearLayout.LayoutParams(px(96), px(44));
            blp.leftMargin = px(10);
            r.addView(save, blp);
            r.addView(load, new LinearLayout.LayoutParams(blp));
            // the D-pad goes to the Save and Load buttons themselves, not the row
            r.setFocusable(false);
            r.setBackground(slot(0x1F000000, 0));
        }
        submenu(R.string.opt_export, R.string.opt_export_hint, "", a::chooseExport);
        submenu(R.string.opt_import, R.string.opt_import_hint, "", () -> a.pickFolder(MainActivity.PICK_IMPORT));
    }

    // a state is written at the next frame boundary: show the new slot once it's there
    private void saveTo(int slot) {
        a.saveState(slot);
        rows.postDelayed(this::fill, 1500);
    }

    private android.graphics.Bitmap thumbnail(int slot) {
        java.io.File f = new java.io.File(new java.io.File(a.baseDir(), "states"), "slot" + slot + ".png");
        if (!f.exists()) return null;
        android.graphics.BitmapFactory.Options o = new android.graphics.BitmapFactory.Options();
        o.inSampleSize = 8;  // about 320 px wide is plenty for the slot picture
        return android.graphics.BitmapFactory.decodeFile(f.getPath(), o);
    }

    private TextView smallButton(String text, boolean enabled, View.OnClickListener click) {
        TextView b = barButton(text, click);
        b.setTextSize(15);
        b.setEnabled(enabled);
        b.setFocusable(enabled);
        if (!enabled) b.setAlpha(0.4f);
        return b;
    }

    private void graphics() {
        String[] res = new String[MainActivity.RES_SCALES.length];
        for (int i = 0; i < res.length; i++) res[i] = a.resolutionLabel(MainActivity.RES_SCALES[i]);
        int lang = 0;
        for (int i = 0; i < MainActivity.LANGUAGES.length; i++) if (MainActivity.LANGUAGES[i] == a.prefs.getInt("language", 1)) lang = i;
        choice(R.string.opt_language, 0, a.getResources().getStringArray(R.array.languages), lang, a::setLanguage);
        choice(R.string.opt_fps, R.string.opt_fps_hint, a.getResources().getStringArray(R.array.fps_modes),
               a.prefs.getBoolean("fg_enabled", false) ? 0 : a.prefs.getInt("fps_mode", 0), a::setFpsMode);
        choice(R.string.opt_resolution, 0, res, indexOf(MainActivity.RES_SCALES, a.prefs.getString("res_scale", "1"), 2), a::setResolution);
        submenu(R.string.opt_framegen, 0, a.frameGenLabel(), () -> openPage(this::frameGenPage));
        choice(R.string.opt_aspect, 0, a.getResources().getStringArray(R.array.aspect_modes), a.prefs.getInt("tv_aspect", 0), i -> {
            a.prefs.edit().putInt("tv_aspect", i).apply();
            Native.setOption("tv_aspect", i);
        });
        choice(R.string.opt_layout, 0, a.getResources().getStringArray(R.array.layouts), a.prefs.getInt("layout", MainActivity.LAYOUT_INSET), i -> {
            a.prefs.edit().putInt("layout", i).apply();
            a.updateLayout();
        });
        choice(R.string.opt_ao, 0, a.getResources().getStringArray(R.array.ao_modes), Native.getOption("ao_mode"), a::setAo);
        toggle(R.string.opt_ao_hires, 0, Native.getOption("ao_hires") != 0, on -> a.setBool("ao_hires", on));
        toggle(R.string.opt_aniso, 0, Native.getOption("aniso") != 0, on -> a.setBool("aniso", on));
        submenu(R.string.opt_perf, R.string.opt_perf_hint, a.getString(a.prefs.getBoolean("perf_hud", false) ? R.string.opt_on : R.string.opt_off),
                () -> openPage(this::perfPage));
        submenu(R.string.opt_shaders, R.string.opt_shaders_hint, "", a::askClearShaders);
        if (a.debuggable())  // a debugging aid: debug builds only
            submenu(R.string.opt_capture, R.string.opt_capture_hint, "", () -> Native.setOption("capture", 1));
    }

    private void mods() {
        int[] labels = {R.string.opt_mod_direct_camera, R.string.opt_mod_first_person, R.string.opt_mod_climb,
                        R.string.opt_mod_quick_doors, R.string.opt_mod_fast_scenes};
        int[] hints = {R.string.opt_mod_direct_camera_hint, 0, R.string.opt_mod_climb_hint, R.string.opt_mod_speed_hint,
                       R.string.opt_mod_speed_hint};
        for (int i = 0; i < MainActivity.MODS.length; i++) {
            String key = MainActivity.MODS[i];
            toggle(labels[i], hints[i], a.prefs.getBoolean(key, false), on -> a.setMod(key, on));
            if (i == 0) {  // its speed right below it
                String[] speeds = new String[MainActivity.CAMERA_SPEEDS.length];
                int cur = 1;
                for (int k = 0; k < speeds.length; k++) {
                    speeds[k] = String.format(java.util.Locale.ROOT, "%.1f×", MainActivity.CAMERA_SPEEDS[k] / 100f);
                    if (MainActivity.CAMERA_SPEEDS[k] == a.prefs.getInt("mod_camera_speed", 100)) cur = k;
                }
                indentNext = true;  // a setting of the mod above
                choice(R.string.opt_mod_camera_speed, 0, speeds, cur, k -> {
                    a.prefs.edit().putInt("mod_camera_speed", MainActivity.CAMERA_SPEEDS[k]).apply();
                    Native.setOption("mod_camera_speed", MainActivity.CAMERA_SPEEDS[k]);
                });
            }
        }
    }

    private void controls() {
        toggle(R.string.opt_onscreen, 0, a.prefs.getBoolean("controls_visible", true), a::setControlsVisible);
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
        submenu(R.string.opt_layout_edit, R.string.opt_layout_edit_hint, "", () -> {
            dismiss();
            a.editTouchLayout();
        });
        String[] sens = new String[MainActivity.CAMERA_SENSITIVITIES.length];
        int curSens = 2;
        for (int i = 0; i < sens.length; i++) {
            sens[i] = Math.round(MainActivity.CAMERA_SENSITIVITIES[i] * 100) + "%";
            if (Math.abs(MainActivity.CAMERA_SENSITIVITIES[i] - a.prefs.getFloat("camera_sensitivity", 1f)) < 0.01f) curSens = i;
        }
        choice(R.string.opt_camera_sensitivity, R.string.opt_camera_sensitivity_hint, sens, curSens, i -> {
            a.prefs.edit().putFloat("camera_sensitivity", MainActivity.CAMERA_SENSITIVITIES[i]).apply();
            a.applyControlsAppearance();
        });
        choice(R.string.opt_combat_buttons, R.string.opt_combat_buttons_hint, a.getResources().getStringArray(R.array.combat_buttons),
               a.prefs.getInt("combat_buttons", 0), i -> {
                   a.prefs.edit().putInt("combat_buttons", i).apply();
                   a.applyControlsAppearance();
               });
        toggle(R.string.opt_haptics, 0, a.prefs.getBoolean("haptics", true), on -> {
            a.prefs.edit().putBoolean("haptics", on).apply();
            a.applyControlsAppearance();
        });
        String[] kinds = {a.getString(R.string.controller_gamepad), a.getString(R.string.controller_pro)};
        choice(R.string.opt_controller, 0, kinds, Native.getOption("pro_controller") != 0 ? 1 : 0, i -> a.setBool("pro_controller", i == 1));
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
        r.setBackground(states(slot(0x1F000000, 0), slot(GLOW, YELLOW)));
        LinearLayout text = new LinearLayout(getContext());
        text.setOrientation(LinearLayout.VERTICAL);
        TextView t = new TextView(getContext());
        t.setText(label);
        t.setTextColor(INK);
        t.setTextSize(18);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.NORMAL));
        text.addView(t);
        if (hint != null) {
            TextView h = new TextView(getContext());
            h.setText(hint);
            h.setTextColor(HINT);
            h.setTextSize(13);
            text.addView(h);
        }
        r.addView(text, new LinearLayout.LayoutParams(0, -2, 1));
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.bottomMargin = px(8);
        if (indentNext) lp.leftMargin = px(36);
        indentNext = false;
        rows.addView(r, lp);
        return r;
    }

    private void toggle(int label, int hint, boolean on, BoolSetter set) {
        LinearLayout r = row(label, hint);
        TextView pill = GameUi.pill(getContext(), on);
        r.addView(pill, new LinearLayout.LayoutParams(px(92), px(38)));
        r.setOnClickListener(v -> { set.set(!on); fill(); });
    }

    private void choice(int label, int hint, String[] values, int cur, IntSetter set) {
        LinearLayout r = row(label, hint);
        int idx = Math.max(0, Math.min(values.length - 1, cur));
        TextView prev = arrow("◀"), next = arrow("▶");
        TextView value = new TextView(getContext());
        value.setText(values[idx]);
        value.setTextColor(INK);
        value.setTextSize(16);
        value.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        value.setGravity(Gravity.CENTER);
        value.setMinWidth(px(150));
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
        LinearLayout r = row(label, hint);
        TextView v = new TextView(getContext());
        v.setText(value.isEmpty() ? "›" : value + "  ›");
        v.setTextColor(INK);
        v.setTextSize(16);
        v.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        r.addView(v, new LinearLayout.LayoutParams(-2, -2));
        r.setOnClickListener(x -> open.run());
    }

    private TextView arrow(String s) {
        TextView t = new TextView(getContext());
        t.setText(s);
        t.setTextColor(0xFFD9B400);
        t.setTextSize(20);
        t.setGravity(Gravity.CENTER);
        return t;
    }

    private void addAction(LinearLayout col, int label, Runnable run) {
        TextView b = barButton(a.getString(label), v -> run.run());
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, px(54));
        lp.bottomMargin = px(12);
        col.addView(b, lp);
    }

    private TextView barButton(String text, View.OnClickListener click) { return GameUi.barButton(getContext(), text, click); }

    private Drawable slot(int fill, int stroke) { return GameUi.slot(getContext(), fill, stroke); }

    private static StateListDrawable states(Drawable normal, Drawable lit) { return GameUi.states(normal, lit); }
}
