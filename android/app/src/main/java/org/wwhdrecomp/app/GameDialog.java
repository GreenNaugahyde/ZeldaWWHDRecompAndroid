package org.wwhdrecomp.app;

import android.app.Dialog;
import android.content.Context;
import android.graphics.Color;
import android.graphics.Typeface;
import android.graphics.drawable.ColorDrawable;
import android.os.Bundle;
import android.view.Gravity;
import android.view.KeyEvent;
import android.view.View;
import android.view.ViewGroup;
import android.view.Window;
import android.view.WindowInsets;
import android.view.WindowInsetsController;
import android.widget.FrameLayout;
import android.widget.LinearLayout;
import android.widget.ScrollView;
import android.widget.TextView;

import java.util.ArrayList;
import java.util.List;

/**
 * A dialog in the style of the in-game menu (GameUi): a parchment card over the dimmed screen,
 * the title on a cyan plate, a message (scrolling when long), optional ON/OFF rows and bar buttons.
 * Controller buttons as in the menu and the game: the bottom button cancels, the right one presses
 * the button selected with the D-pad (none is preselected, so nothing is confirmed by accident).
 */
final class GameDialog extends Dialog {
    interface Toggle { void set(boolean on); }

    private CharSequence title, message;
    private View content;
    private final List<Object[]> toggles = new ArrayList<>();   // {label, boolean[] state, Toggle}
    private final List<Object[]> buttons = new ArrayList<>();   // {label, Runnable}, left to right
    private boolean cancelable = true;
    private boolean top;  // in the upper part of the screen (above a soft keyboard)

    GameDialog(Context c) { super(c, android.R.style.Theme_Translucent_NoTitleBar_Fullscreen); }

    GameDialog title(int res) { title = getContext().getString(res); return this; }
    GameDialog title(CharSequence t) { title = t; return this; }
    GameDialog message(int res) { message = getContext().getString(res); return this; }
    GameDialog message(CharSequence m) { message = m; return this; }
    GameDialog content(View v) { content = v; return this; }

    /** An ON/OFF row; `set` gets each change. */
    GameDialog toggle(CharSequence label, boolean on, Toggle set) {
        toggles.add(new Object[] {label, new boolean[] {on}, set});
        return this;
    }

    /** A button (they appear in the order added, the last one on the right); `run` may be null. */
    GameDialog button(int label, Runnable run) { return button(label, run, true); }

    /** The same, greyed out and without effect when `enabled` is false. */
    GameDialog button(int label, Runnable run, boolean enabled) {
        buttons.add(new Object[] {getContext().getString(label), run, enabled});
        return this;
    }

    GameDialog cancelable(boolean c) { cancelable = c; return this; }

    interface Capture { void got(int keyCode); }
    private Capture capture;
    private int captureCode;  // the controller button pressed, taken when it is released

    /**
     * Waits for a controller button (InputMapper.assignable; analog triggers and a hat D-pad count
     * as their buttons) and passes it on once released, so its release doesn't reach anything else;
     * Home / Guide cancels. While waiting, the controller doesn't operate the dialog.
     */
    GameDialog captureButton(Capture c) { capture = c; return this; }

    interface KeyWatch { void key(int keyCode, boolean down); }
    private KeyWatch watch;
    private final java.util.Set<Integer> analogHeld = new java.util.HashSet<>();

    /**
     * Every controller button goes to `w` (pressed and released; analog triggers and a hat D-pad as their buttons)
     * instead of operating the dialog; Home / Guide closes it.
     */
    GameDialog watchKeys(KeyWatch w) { watch = w; return this; }

    @Override
    public boolean dispatchKeyEvent(KeyEvent e) {
        int code = e.getKeyCode();
        if (watch != null && (InputMapper.isController(e.getDevice()) || KeyEvent.isGamepadButton(code))) {
            if (code == KeyEvent.KEYCODE_BACK) code = KeyEvent.KEYCODE_BUTTON_SELECT;
            if (code == KeyEvent.KEYCODE_BUTTON_MODE) {
                if (e.getAction() == KeyEvent.ACTION_UP) dismiss();
            } else if (e.getAction() == KeyEvent.ACTION_DOWN && e.getRepeatCount() == 0) watch.key(code, true);
            else if (e.getAction() == KeyEvent.ACTION_UP) watch.key(code, false);
            return true;
        }
        if (capture == null || !(InputMapper.isController(e.getDevice()) || KeyEvent.isGamepadButton(code)))
            return super.dispatchKeyEvent(e);
        if (code == KeyEvent.KEYCODE_BACK) code = KeyEvent.KEYCODE_BUTTON_SELECT;
        if (code == KeyEvent.KEYCODE_BUTTON_MODE) {
            if (e.getAction() == KeyEvent.ACTION_UP) dismiss();
            return true;
        }
        if (!InputMapper.assignable(code)) return true;
        if (e.getAction() == KeyEvent.ACTION_DOWN && captureCode == 0) captureCode = code;
        else if (e.getAction() == KeyEvent.ACTION_UP && code == captureCode) captured();
        return true;
    }

    @Override
    public boolean dispatchGenericMotionEvent(android.view.MotionEvent e) {
        if (watch != null && InputMapper.isController(e.getDevice())) {
            java.util.List<Integer> held = InputMapper.analogButtons(e);
            for (Integer c : new java.util.ArrayList<>(analogHeld))
                if (!held.contains(c)) { analogHeld.remove(c); watch.key(c, false); }
            for (int c : held)
                if (analogHeld.add(c)) watch.key(c, true);
            return true;
        }
        if (capture == null || !InputMapper.isController(e.getDevice())) return super.dispatchGenericMotionEvent(e);
        java.util.List<Integer> held = InputMapper.analogButtons(e);
        if (captureCode == 0 && !held.isEmpty()) captureCode = held.get(0);
        else if (captureCode != 0 && !held.contains(captureCode) && isAnalog(captureCode)) captured();
        return true;
    }

    private static boolean isAnalog(int code) {
        return code == KeyEvent.KEYCODE_BUTTON_L2 || code == KeyEvent.KEYCODE_BUTTON_R2 || code == KeyEvent.KEYCODE_DPAD_UP
                || code == KeyEvent.KEYCODE_DPAD_DOWN || code == KeyEvent.KEYCODE_DPAD_LEFT || code == KeyEvent.KEYCODE_DPAD_RIGHT;
    }

    private void captured() {
        Capture c = capture;
        int code = captureCode;
        capture = null;
        dismiss();
        c.got(code);
    }
    GameDialog top() { top = true; return this; }

    /** Presses the button at `index` (in the order added), as a tap would. */
    void press(int index) {
        if (index < buttonViews.size()) buttonViews.get(index).performClick();
    }

    private final List<View> buttonViews = new ArrayList<>();

    private int px(float v) { return GameUi.px(getContext(), v); }

    @Override
    protected void onCreate(Bundle state) {
        super.onCreate(state);
        setCancelable(cancelable);
        setCanceledOnTouchOutside(false);
        Window w = getWindow();
        w.setBackgroundDrawable(new ColorDrawable(0x99203448));
        w.setLayout(ViewGroup.LayoutParams.MATCH_PARENT, ViewGroup.LayoutParams.MATCH_PARENT);

        Context c = getContext();
        FrameLayout root = new FrameLayout(c);
        LinearLayout card = new LinearLayout(c);
        card.setOrientation(LinearLayout.VERTICAL);
        int screenW = c.getResources().getDisplayMetrics().widthPixels, screenH = c.getResources().getDisplayMetrics().heightPixels;
        // the GamePad menus' torn paper sheet (GameUi.GamePaper, in the menus' layout units fitted to the screen); its
        // torn edge and blue line take ~30 of its 48-unit frame
        float unit = Math.min(screenW / 1280f, screenH / 720f);
        card.setBackground(new GameUi.GamePaper(c, unit));
        int frame = Math.round(52 * unit);  // (clear of the blue line along it)
        card.setPadding(frame + px(20), frame + px(12), frame + px(20), frame + px(14));
        FrameLayout.LayoutParams clp = new FrameLayout.LayoutParams(Math.min(screenW - px(64), px(760)), -2,
                top ? Gravity.CENTER_HORIZONTAL | Gravity.TOP : Gravity.CENTER);
        clp.topMargin = top ? px(28) : px(8);
        clp.bottomMargin = px(8);  // (the card fits the screen: the scrolling part shrinks, the buttons stay in view)
        root.addView(card, clp);

        if (title != null) {
            GameUi.OutlinedText t = new GameUi.OutlinedText(c);
            t.setText(title);
            t.setTextColor(Color.WHITE);
            t.setTextSize(22);
            t.setTypeface(Typeface.create("sans-serif-black", Typeface.NORMAL));
            t.setPadding(px(24), px(6), px(26), px(12));  // (the plate leaves its shadow's room below)
            t.setBackground(new GameUi.GameTabPlate(c, false, GameUi.GREEN, unit));  // (as the menus' tabs)
            t.outline = GameUi.GREEN.dark;
            t.gameFontAuto = true;  // (the game's tab font at this size)
            LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, -2);
            lp.bottomMargin = px(14);
            card.addView(t, lp);
        }

        // message, content and toggles scroll together when they don't fit
        LinearLayout body = new LinearLayout(c);
        body.setOrientation(LinearLayout.VERTICAL);
        if (message != null) {
            TextView m = GameUi.text(c, true);
            m.setText(message);
            m.setTextColor(GameUi.INK);
            m.setTextSize(17);
            m.setLineSpacing(0, 1.1f);
            body.addView(m, new LinearLayout.LayoutParams(-1, -2));
        }
        if (content != null) body.addView(content, new LinearLayout.LayoutParams(-1, -2));
        for (Object[] t : toggles) body.addView(toggleRow(t));
        ScrollView scroll = new ScrollView(c) {
            @Override
            protected void onMeasure(int wSpec, int hSpec) {  // at most 55% of the screen
                super.onMeasure(wSpec, MeasureSpec.makeMeasureSpec((int) (screenH * 0.55f), MeasureSpec.AT_MOST));
            }
        };
        scroll.addView(body);
        card.addView(scroll, new LinearLayout.LayoutParams(-1, -2, 1));

        if (!buttons.isEmpty()) {
            LinearLayout row = new LinearLayout(c);
            row.setGravity(Gravity.END);
            row.setPadding(0, px(18), 0, 0);
            for (int i = 0; i < buttons.size(); i++) {
                Object[] b = buttons.get(i);
                Runnable run = (Runnable) b[1];
                // the last button confirms: green (a lone OK too)
                TextView v = GameUi.barButton(c, (String) b[0], x -> {
                    dismiss();
                    if (run != null) run.run();
                }, i == buttons.size() - 1, true);
                v.setMinWidth(px(130));
                if (!(Boolean) b[2]) {
                    v.setEnabled(false);
                    v.setFocusable(false);
                    v.setAlpha(0.4f);
                }
                buttonViews.add(v);
                LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-2, px(52));
                lp.leftMargin = px(12);
                row.addView(v, lp);
            }
            card.addView(row, new LinearLayout.LayoutParams(-1, -2));
        }
        setContentView(root);
        WindowInsetsController ic = w.getInsetsController();
        if (ic != null) {
            ic.hide(WindowInsets.Type.systemBars());
            ic.setSystemBarsBehavior(WindowInsetsController.BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE);
        }
    }

    private View toggleRow(Object[] t) {
        Context c = getContext();
        boolean[] on = (boolean[]) t[1];
        LinearLayout r = new LinearLayout(c);
        r.setGravity(Gravity.CENTER_VERTICAL);
        r.setPadding(px(18), px(8), px(14), px(8));
        r.setMinimumHeight(px(54));
        r.setFocusable(true);
        r.setBackground(GameUi.slotStates(c));
        TextView l = GameUi.text(c, true);
        l.setText((CharSequence) t[0]);
        l.setTextColor(GameUi.INK);
        l.setTextSize(17);
        r.addView(l, new LinearLayout.LayoutParams(0, -2, 1));
        TextView[] pill = {GameUi.pill(c, on[0], true)};  // (the one shown: each tap replaces it)
        r.addView(pill[0], new LinearLayout.LayoutParams(px(92), px(38)));
        r.setOnClickListener(v -> {
            on[0] = !on[0];
            ((Toggle) t[2]).set(on[0]);
            r.removeView(pill[0]);
            pill[0] = GameUi.pill(c, on[0], true);
            r.addView(pill[0], new LinearLayout.LayoutParams(px(92), px(38)));
        });
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = px(8);
        r.setLayoutParams(lp);
        return r;
    }

    // the bottom / right buttons pressed here: a release whose press came before the dialog (one shown by a press)
    // does nothing
    private final java.util.Set<Integer> downHere = new java.util.HashSet<>();

    @Override
    public boolean onKeyDown(int code, KeyEvent e) {
        if (code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_BUTTON_B) {  // acted on release
            downHere.add(code);
            return true;
        }
        return super.onKeyDown(code, e);
    }

    @Override
    public boolean onKeyUp(int code, KeyEvent e) {
        if ((code == KeyEvent.KEYCODE_BUTTON_A || code == KeyEvent.KEYCODE_BUTTON_B) && !downHere.remove(code)) return true;
        if (code == KeyEvent.KEYCODE_BUTTON_A) {  // bottom button: cancel
            if (cancelable) cancel();
            return true;
        }
        if (code == KeyEvent.KEYCODE_BUTTON_B) {  // right button: the selected button or row
            View f = getCurrentFocus();
            if (f != null) f.performClick();
            return true;
        }
        return super.onKeyUp(code, e);
    }
}
