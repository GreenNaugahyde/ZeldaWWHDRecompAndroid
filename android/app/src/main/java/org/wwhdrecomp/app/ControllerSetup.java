package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Typeface;
import android.view.Gravity;
import android.view.InputDevice;
import android.view.KeyEvent;
import android.view.View;
import android.widget.LinearLayout;
import android.widget.TextView;

/**
 * Setting a controller up, step by step (notes/controller-setup.md): what the remapping is (the game knows only the
 * Wii U GamePad; each of its buttons is pressed by one controller button, and while playing the game uses all of them
 * but L and d-pad down), the face buttons by position or by their letters (not asked for PlayStation and Nintendo controllers, whose
 * marks match the position), a preset (InputMapper.preset: Original, Jump anywhere on a face button with its item
 * button taken off, or with the button's Wii U button moved to L1), then a picture of the controller (the PlayStation
 * symbols on Sony's, else a generic one) where each pressed button lights up with what it does. Offered once when a
 * controller is first used (MainActivity.maybeOfferSetup), and from Controls. The game is paused meanwhile.
 */
final class ControllerSetup {
    static boolean active;  // a step shows

    private final MainActivity a;
    private final InputDevice dev;
    private final Runnable done;  // after the last step or a cancel (may be null)
    private final boolean sony, askLetters;
    private boolean letters;
    private int preset;
    private boolean advancing;  // a step closes for the next one, not a cancel

    ControllerSetup(MainActivity a, InputDevice dev, Runnable done) {
        this.a = a;
        this.dev = dev;
        this.done = done;
        sony = InputMapper.isSony(dev);
        askLetters = !sony && !InputMapper.isNintendo(dev);
        letters = a.inputMapper().byLetters();
    }

    private String s(int res, Object... args) { return a.getString(res, args); }

    /** "Set it up?" for a controller used for the first time (Later: asked again when the app next starts) */
    void offer() {
        begin();
        step(new GameDialog(a).title(R.string.cs_offer_title).message(s(R.string.cs_offer_text, dev.getName()))
                .button(R.string.cs_offer_never, this::asked)
                .button(R.string.cs_offer_later, null)
                .button(R.string.cs_offer_yes, then(() -> { asked(); intro(); })));
    }

    private void asked() { a.prefs.edit().putBoolean("pad_asked@" + dev.getDescriptor(), true).apply(); }

    void start() {
        asked();  // (set up: not offered any more)
        begin();
        intro();
    }

    private void begin() {
        active = true;
        a.usePadMap(dev);
        android.view.Choreographer.getInstance().postFrameCallback(hold);
    }

    // the game waits meanwhile (MainActivity.holdGame: each frame)
    private final android.view.Choreographer.FrameCallback hold = new android.view.Choreographer.FrameCallback() {
        @Override
        public void doFrame(long now) {
            if (!active) return;
            a.holdGame();
            android.view.Choreographer.getInstance().postFrameCallback(this);
        }
    };

    private void end() {
        active = false;
        if (done != null) done.run();
    }

    /** shows a step; closing it any other way than next() ends the setup */
    private void step(GameDialog d) {
        d.setOnDismissListener(x -> {
            if (advancing) advancing = false;
            else end();
        });
        d.show();
    }

    private GameDialog current;

    private void next(Runnable step) {
        if (current != null && current.isShowing()) {
            advancing = true;
            current.dismiss();
        }
        step.run();
    }

    private GameDialog dialog() { return current = new GameDialog(a); }

    // GameDialog's buttons close it before they run: they go on through this
    private Runnable then(Runnable step) { return () -> { advancing = true; step.run(); }; }

    private void intro() {
        step(dialog().title(R.string.cs_title).message(s(R.string.cs_intro, dev.getName()))
                .button(R.string.opt_cancel, null)
                .button(R.string.cs_next, then(askLetters ? this::faceButtons : this::presets)));
    }

    private void faceButtons() {
        step(dialog().title(R.string.cs_face_title).message(R.string.cs_face_text)
                .button(R.string.cs_face_letters, then(() -> { letters = true; presets(); }))
                .button(R.string.cs_face_position, then(() -> { letters = false; presets(); })));
    }

    private void presets() {
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        GameDialog d = dialog().title(R.string.cs_preset_title).message(R.string.cs_preset_text).content(box)
                .button(R.string.opt_cancel, null);
        int[][] rows = {{R.string.cs_preset_original, R.string.cs_preset_original_text},
                {R.string.cs_preset_takeoff, R.string.cs_preset_takeoff_text},
                {R.string.cs_preset_tol, R.string.cs_preset_tol_text}};
        for (int i = 0; i < rows.length; i++) {
            final int p = i;
            String text = s(rows[i][1]);
            if (i == InputMapper.PRESET_TAKE_OFF && a.itemRingOn()) text += " " + s(R.string.cs_takeoff_ring);
            box.addView(choiceRow(s(rows[i][0]), text, v -> next(() -> {
                preset = p;
                if (p == InputMapper.PRESET_ORIGINAL) apply(0);
                else pickJump(null);
            })));
        }
        step(d);
    }

    private View choiceRow(String title, String text, View.OnClickListener click) {
        LinearLayout r = new LinearLayout(a);
        r.setOrientation(LinearLayout.VERTICAL);
        r.setPadding(px(18), px(10), px(14), px(10));
        r.setFocusable(true);
        r.setBackground(GameUi.slotStates(a));
        TextView t = GameUi.text(a, true);
        t.setText(title);
        t.setTextColor(GameUi.INK);
        t.setTextSize(18);
        t.setTypeface(Typeface.create("sans-serif-medium", Typeface.BOLD));
        r.addView(t);
        TextView h = GameUi.text(a, true);
        h.setText(text);
        h.setTextColor(GameUi.HINT);
        h.setTextSize(15);
        r.addView(h);
        r.setOnClickListener(click);
        LinearLayout.LayoutParams lp = new LinearLayout.LayoutParams(-1, -2);
        lp.topMargin = px(8);
        r.setLayoutParams(lp);
        return r;
    }

    // the button for Jump anywhere (and the moon jump): pressed on the controller
    private void pickJump(String problem) {
        InputMapper base = new InputMapper();
        base.preset(letters, InputMapper.PRESET_ORIGINAL, 0);
        boolean ring = a.itemRingOn();
        String msg = preset == InputMapper.PRESET_TAKE_OFF
                ? s(R.string.cs_jump_takeoff, markName(base.map[3]), markName(base.map[2]), markName(base.map[5]))
                        + (ring ? "\n\n" + s(R.string.cs_takeoff_ring) : "")
                : s(R.string.cs_jump_tol);
        if (problem != null) msg = problem + "\n\n" + msg;
        step(dialog().title(R.string.cs_jump_title).message(msg).button(R.string.opt_cancel, null)
                .captureButton(code -> {
                    advancing = true;  // (the capture closes it)
                    if (code == KeyEvent.KEYCODE_BUTTON_SELECT || code == KeyEvent.KEYCODE_BUTTON_START) {
                        pickJump(s(R.string.cs_jump_not_menu, markName(code)));
                    } else if (code == KeyEvent.KEYCODE_BUTTON_L1) {
                        preset = InputMapper.PRESET_ORIGINAL;  // (Jump anywhere's place in the Original layout)
                        apply(0);
                    } else if (preset == InputMapper.PRESET_TAKE_OFF && !base.removableOnly(code, ring)) {
                        pickJump(s(ring ? R.string.cs_jump_not_item_ring : R.string.cs_jump_not_item, markName(code), gameNames(base, code)));
                    } else apply(code);
                }));
    }

    private void apply(int jump) {
        a.applyPadPreset(letters, preset, jump);
        tryIt();
    }

    // ---- try it: the picture and a list of what each button does, the pressed one lit
    private final java.util.Set<Integer> held = new java.util.HashSet<>();

    private void tryIt() {
        InputMapper m = a.inputMapper();
        LinearLayout box = new LinearLayout(a);
        box.setOrientation(LinearLayout.VERTICAL);
        PadPicture pic = new PadPicture(a);
        int h = Math.min(px(300), (int) (a.getResources().getDisplayMetrics().heightPixels * 0.3f));
        box.addView(pic, new LinearLayout.LayoutParams(-1, h));
        java.util.Map<Integer, TextView> rows = new java.util.HashMap<>();
        for (int code : PadPicture.CODES) {
            String what = gameNames(m, code);
            TextView r = GameUi.text(a, true);
            r.setText(markName(code) + "  →  " + what);
            r.setTextColor(GameUi.INK);
            r.setTextSize(15);
            r.setPadding(px(10), px(3), px(10), px(3));
            box.addView(r);
            rows.put(code, r);
        }
        // the Wii U buttons no controller button presses
        StringBuilder off = new StringBuilder();
        String[] wiiu = a.getResources().getStringArray(R.array.wiiu_buttons);
        for (int j = 0; j < InputMapper.WIIU_COUNT; j++)
            if (m.map[j] == 0) off.append(off.length() == 0 ? "" : ", ").append(wiiu[j]);
        if (off.length() > 0) {
            TextView r = GameUi.text(a, true);
            r.setText(s(R.string.cs_taken_off, off));
            r.setTextColor(GameUi.HINT);
            r.setTextSize(15);
            r.setPadding(px(10), px(6), px(10), px(3));
            box.addView(r);
        }
        held.clear();
        GameDialog d = dialog().title(R.string.cs_try_title)
                .message(s(R.string.cs_try_text, markName(KeyEvent.KEYCODE_BUTTON_START), markName(KeyEvent.KEYCODE_BUTTON_SELECT)))
                .content(box)
                .button(R.string.cs_change, then(askLetters ? this::faceButtons : this::presets))
                .button(R.string.cs_done, null);
        // every button is being tried here, so the bar's buttons are pressed by holding one: Start Done, Select Change
        // (shown pressed while held; 0.8 s)
        Runnable[] hold = {null};
        d.watchKeys((code, down) -> {
            int bar = code == KeyEvent.KEYCODE_BUTTON_START ? 1 : code == KeyEvent.KEYCODE_BUTTON_SELECT ? 0 : -1;
            if (bar >= 0) {
                View v = d.buttonView(bar);
                if (hold[0] != null) pic.removeCallbacks(hold[0]);
                hold[0] = null;
                if (v != null) v.setPressed(down);
                if (down && v != null) {
                    hold[0] = () -> { v.setPressed(false); d.press(bar); };
                    pic.postDelayed(hold[0], 800);
                }
            }
            if (down) held.add(code);
            else held.remove(code);
            pic.invalidate();
            TextView r = rows.get(code);
            if (r != null) {
                r.setBackground(down ? GameUi.slot(a, GameUi.GLOW, GameUi.YELLOW) : null);
                if (down) r.requestRectangleOnScreen(new android.graphics.Rect(0, 0, r.getWidth(), r.getHeight()));
            }
        });
        step(d);
    }

    /** what controller button `code` does in the game with map m: its Wii U buttons and our own inputs */
    private String gameNames(InputMapper m, int code) {
        String[] wiiu = a.getResources().getStringArray(R.array.wiiu_buttons);
        String[] act = a.getResources().getStringArray(R.array.cs_wiiu_actions);
        StringBuilder b = new StringBuilder();
        for (int j = 0; j < InputMapper.WIIU_COUNT; j++) {
            if (m.map[j] != code) continue;
            if (b.length() > 0) b.append(" · ");
            b.append(wiiu[j]);
            if (!act[j].isEmpty()) b.append(": ").append(act[j]);
            if (InputMapper.ringCovers(j) && a.itemRingOn()) b.append(" (").append(s(R.string.cs_in_ring)).append(')');
        }
        boolean jump = a.prefs.getBoolean("mod_jump", false), moon = a.prefs.getBoolean("cheat_moon_jump", false);
        if (jump && m.map[InputMapper.OWN_JUMP] == code) b.append(b.length() > 0 ? " · " : "").append(s(R.string.opt_mod_jump));
        if (moon && m.map[InputMapper.OWN_MOON] == code) b.append(b.length() > 0 ? " · " : "").append(s(R.string.cheat_moon_jump));
        if (b.length() == 0) b.append(s(R.string.cs_nothing));
        if (code == KeyEvent.KEYCODE_BUTTON_SELECT) b.append(" (").append(s(R.string.cs_select_hold)).append(')');
        return b.toString();
    }

    /** the controller's own mark for a button: the PlayStation symbols, else the letters as printed */
    private String markName(int code) {
        if (sony) {
            switch (code) {
                case KeyEvent.KEYCODE_BUTTON_A: return "✕";
                case KeyEvent.KEYCODE_BUTTON_B: return "○";
                case KeyEvent.KEYCODE_BUTTON_X: return "□";
                case KeyEvent.KEYCODE_BUTTON_Y: return "△";
                case KeyEvent.KEYCODE_BUTTON_SELECT: return "Create / Share";
                case KeyEvent.KEYCODE_BUTTON_START: return "Options";
            }
        }
        String f = PadPicture.faceLetter(code, InputMapper.isNintendo(dev));
        if (f != null) return f;
        switch (code) {
            case KeyEvent.KEYCODE_BUTTON_L1: return "L1 / LB";
            case KeyEvent.KEYCODE_BUTTON_R1: return "R1 / RB";
            case KeyEvent.KEYCODE_BUTTON_L2: return "L2 / LT";
            case KeyEvent.KEYCODE_BUTTON_R2: return "R2 / RT";
            case KeyEvent.KEYCODE_BUTTON_SELECT: return "Select / View";
            case KeyEvent.KEYCODE_BUTTON_START: return "Start / Menu";
            case KeyEvent.KEYCODE_BUTTON_THUMBL: return "L3";
            case KeyEvent.KEYCODE_BUTTON_THUMBR: return "R3";
            case KeyEvent.KEYCODE_DPAD_UP: return "↑";
            case KeyEvent.KEYCODE_DPAD_DOWN: return "↓";
            case KeyEvent.KEYCODE_DPAD_LEFT: return "←";
            case KeyEvent.KEYCODE_DPAD_RIGHT: return "→";
            default: return InputMapper.buttonName(code);
        }
    }

    private int px(float v) { return GameUi.px(a, v); }

    /**
     * A controller drawn in code (1000 x 600 units, fitted to the view): shoulders, d-pad, the face buttons with their
     * marks (PlayStation symbols in their colours, else letters), Select / Start and the sticks; beside each button
     * the short name of what it presses in the game (A1-A3 for the item buttons, Jump for ours). Held buttons light up.
     */
    private final class PadPicture extends View {
        static final int[] CODES = {KeyEvent.KEYCODE_BUTTON_A, KeyEvent.KEYCODE_BUTTON_B, KeyEvent.KEYCODE_BUTTON_X,
                KeyEvent.KEYCODE_BUTTON_Y, KeyEvent.KEYCODE_BUTTON_L1, KeyEvent.KEYCODE_BUTTON_R1, KeyEvent.KEYCODE_BUTTON_L2,
                KeyEvent.KEYCODE_BUTTON_R2, KeyEvent.KEYCODE_BUTTON_SELECT, KeyEvent.KEYCODE_BUTTON_START,
                KeyEvent.KEYCODE_BUTTON_THUMBL, KeyEvent.KEYCODE_BUTTON_THUMBR, KeyEvent.KEYCODE_DPAD_UP,
                KeyEvent.KEYCODE_DPAD_DOWN, KeyEvent.KEYCODE_DPAD_LEFT, KeyEvent.KEYCODE_DPAD_RIGHT};
        private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG);

        PadPicture(Context c) { super(c); }

        /** a face button's printed letter: Xbox-style (A bottom), Nintendo's (B bottom); null for other buttons */
        static String faceLetter(int code, boolean nintendo) {
            switch (code) {
                case KeyEvent.KEYCODE_BUTTON_A: return nintendo ? "B" : "A";
                case KeyEvent.KEYCODE_BUTTON_B: return nintendo ? "A" : "B";
                case KeyEvent.KEYCODE_BUTTON_X: return nintendo ? "Y" : "X";
                case KeyEvent.KEYCODE_BUTTON_Y: return nintendo ? "X" : "Y";
                default: return null;
            }
        }

        // the short game name beside a button: the Wii U letter (item buttons A1-A3), Jump / Moon for ours
        private String tag(int code) {
            InputMapper m = a.inputMapper();
            String[] shortWiiu = {"A", "B", "A2", "A1", "L", "A3", "ZL", "ZR", "−", "+", "L3", "R3", "▲", "▼", "◀", "▶"};
            StringBuilder b = new StringBuilder();
            for (int j = 0; j < InputMapper.WIIU_COUNT; j++)
                if (m.map[j] == code) b.append(b.length() > 0 ? "+" : "").append(shortWiiu[j]);
            if (a.prefs.getBoolean("mod_jump", false) && m.map[InputMapper.OWN_JUMP] == code)
                b.append(b.length() > 0 ? "+" : "").append(a.getString(R.string.cs_tag_jump));
            else if (a.prefs.getBoolean("cheat_moon_jump", false) && m.map[InputMapper.OWN_MOON] == code)
                b.append(b.length() > 0 ? "+" : "").append(a.getString(R.string.cs_tag_moon));
            return b.length() == 0 ? "—" : b.toString();
        }

        private float k, ox, oy;  // units to pixels, offset

        private float X(float u) { return ox + u * k; }
        private float Y(float u) { return oy + u * k; }

        @Override
        protected void onDraw(Canvas c) {
            k = Math.min(getWidth() / 1000f, getHeight() / 600f);
            ox = (getWidth() - 1000 * k) / 2;
            oy = (getHeight() - 600 * k) / 2;
            // the body: a wide rounded bar with two grips
            Path body = new Path();
            body.addRoundRect(new RectF(X(120), Y(130), X(880), Y(420)), 140 * k, 140 * k, Path.Direction.CW);
            body.addCircle(X(250), Y(410), 140 * k, Path.Direction.CW);
            body.addCircle(X(750), Y(410), 140 * k, Path.Direction.CW);
            p.setStyle(Paint.Style.FILL);
            p.setColor(0xFFDCD6BE);
            c.drawPath(body, p);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(4 * k);
            p.setColor(GameUi.INK);
            c.drawPath(body, p);

            shoulder(c, KeyEvent.KEYCODE_BUTTON_L2, 150, 30, sony ? "L2" : "LT", true);
            shoulder(c, KeyEvent.KEYCODE_BUTTON_L1, 150, 84, sony ? "L1" : "LB", true);
            shoulder(c, KeyEvent.KEYCODE_BUTTON_R2, 670, 30, sony ? "R2" : "RT", false);
            shoulder(c, KeyEvent.KEYCODE_BUTTON_R1, 670, 84, sony ? "R1" : "RB", false);
            // d-pad
            dpad(c, KeyEvent.KEYCODE_DPAD_UP, 250, 205, 0, -1);
            dpad(c, KeyEvent.KEYCODE_DPAD_DOWN, 250, 295, 0, 1);
            dpad(c, KeyEvent.KEYCODE_DPAD_LEFT, 205, 250, -1, 0);
            dpad(c, KeyEvent.KEYCODE_DPAD_RIGHT, 295, 250, 1, 0);
            // face buttons
            face(c, KeyEvent.KEYCODE_BUTTON_Y, 750, 180, 0, -1);
            face(c, KeyEvent.KEYCODE_BUTTON_A, 750, 320, 0, 1);
            face(c, KeyEvent.KEYCODE_BUTTON_X, 680, 250, -1, 0);
            face(c, KeyEvent.KEYCODE_BUTTON_B, 820, 250, 1, 0);
            // Select / Start
            small(c, KeyEvent.KEYCODE_BUTTON_SELECT, 440, 200, sony ? "Create" : "Select");
            small(c, KeyEvent.KEYCODE_BUTTON_START, 560, 200, sony ? "Options" : "Start");
            // sticks
            stick(c, KeyEvent.KEYCODE_BUTTON_THUMBL, 400, 370, "L3");
            stick(c, KeyEvent.KEYCODE_BUTTON_THUMBR, 600, 370, "R3");
        }

        private void fillFor(int code, int normal) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(held.contains(code) ? GameUi.YELLOW : normal);
        }

        private void outline() {
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(3 * k);
            p.setColor(GameUi.INK);
        }

        private void text(Canvas c, String t, float x, float y, float size, int color, boolean bold) {
            p.setStyle(Paint.Style.FILL);
            p.setColor(color);
            p.setTextSize(size * k);
            p.setTypeface(Typeface.create("sans-serif", bold ? Typeface.BOLD : Typeface.NORMAL));
            p.setTextAlign(Paint.Align.CENTER);
            c.drawText(t, x, y - (p.descent() + p.ascent()) / 2, p);
        }

        // the game name beside a button, on a small plate
        private void tagAt(Canvas c, int code, float ux, float uy) {
            String t = tag(code);
            p.setTextSize(30 * k);
            float w = p.measureText(t) + 16 * k, h = 38 * k;
            RectF r = new RectF(X(ux) - w / 2, Y(uy) - h / 2, X(ux) + w / 2, Y(uy) + h / 2);
            p.setStyle(Paint.Style.FILL);
            p.setColor(held.contains(code) ? GameUi.YELLOW : 0xFFFFFFFF);
            c.drawRoundRect(r, 10 * k, 10 * k, p);
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(2 * k);
            p.setColor(GameUi.CYAN_DARK);
            c.drawRoundRect(r, 10 * k, 10 * k, p);
            text(c, t, X(ux), Y(uy), 30, GameUi.CYAN_DARK, true);
        }

        private void shoulder(Canvas c, int code, float ux, float uy, String mark, boolean left) {
            RectF r = new RectF(X(ux), Y(uy), X(ux + 180), Y(uy + 46));
            fillFor(code, 0xFFC9C2A6);
            c.drawRoundRect(r, 18 * k, 18 * k, p);
            outline();
            c.drawRoundRect(r, 18 * k, 18 * k, p);
            text(c, mark, r.centerX(), r.centerY(), 28, GameUi.INK, true);
            tagAt(c, code, left ? ux - 70 : ux + 250, uy + 23);
        }

        private void dpad(Canvas c, int code, float ux, float uy, int dx, int dy) {
            RectF r = new RectF(X(ux - 30), Y(uy - 30), X(ux + 30), Y(uy + 30));
            fillFor(code, 0xFF55626B);
            c.drawRoundRect(r, 8 * k, 8 * k, p);
            outline();
            c.drawRoundRect(r, 8 * k, 8 * k, p);
            tagAt(c, code, ux + dx * 75, uy + dy * 68);
        }

        private void face(Canvas c, int code, float ux, float uy, int dx, int dy) {
            fillFor(code, 0xFF4A565E);
            c.drawCircle(X(ux), Y(uy), 33 * k, p);
            outline();
            c.drawCircle(X(ux), Y(uy), 33 * k, p);
            if (sony) symbol(c, code, X(ux), Y(uy));
            else text(c, faceLetter(code, InputMapper.isNintendo(dev)), X(ux), Y(uy), 34, 0xFFFFFFFF, true);
            tagAt(c, code, ux + dx * 85, uy + dy * 70);
        }

        // ✕ ○ □ △ in their colours
        private void symbol(Canvas c, int code, float x, float y) {
            p.setStyle(Paint.Style.STROKE);
            p.setStrokeWidth(5 * k);
            float r = 15 * k;
            switch (code) {
                case KeyEvent.KEYCODE_BUTTON_A:
                    p.setColor(0xFF8DB8F0);
                    c.drawLine(x - r, y - r, x + r, y + r, p);
                    c.drawLine(x - r, y + r, x + r, y - r, p);
                    break;
                case KeyEvent.KEYCODE_BUTTON_B:
                    p.setColor(0xFFFF7A7A);
                    c.drawCircle(x, y, r, p);
                    break;
                case KeyEvent.KEYCODE_BUTTON_X:
                    p.setColor(0xFFF0A0D0);
                    c.drawRect(x - r, y - r, x + r, y + r, p);
                    break;
                case KeyEvent.KEYCODE_BUTTON_Y:
                    p.setColor(0xFF5CD6B0);
                    Path t = new Path();
                    t.moveTo(x, y - r * 1.1f);
                    t.lineTo(x + r * 1.1f, y + r * 0.8f);
                    t.lineTo(x - r * 1.1f, y + r * 0.8f);
                    t.close();
                    c.drawPath(t, p);
                    break;
            }
        }

        private void small(Canvas c, int code, float ux, float uy, String mark) {
            RectF r = new RectF(X(ux - 40), Y(uy - 16), X(ux + 40), Y(uy + 16));
            fillFor(code, 0xFF55626B);
            c.drawRoundRect(r, 16 * k, 16 * k, p);
            outline();
            c.drawRoundRect(r, 16 * k, 16 * k, p);
            text(c, mark, X(ux), Y(uy + 42), 22, GameUi.INK, false);
            tagAt(c, code, ux, uy - 50);
        }

        private void stick(Canvas c, int code, float ux, float uy, String mark) {
            fillFor(code, 0xFF3D474E);
            c.drawCircle(X(ux), Y(uy), 50 * k, p);
            outline();
            c.drawCircle(X(ux), Y(uy), 50 * k, p);
            text(c, mark, X(ux), Y(uy), 26, 0xFFFFFFFF, true);
            tagAt(c, code, ux, uy + 85);
        }
    }
}
