package org.wwhdrecomp.app;

import android.content.SharedPreferences;
import android.graphics.Bitmap;
import android.graphics.Canvas;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Rect;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.MotionEvent;
import android.view.View;

import java.util.HashSet;
import java.util.Set;

/**
 * A hint on how a minigame is played by touch (ControlsView, Native.minigameMode), shown the first time each
 * kind comes up, in the look of the game's own messages: its message window (MsgWindowMain_00), its message
 * font, the text typed out as the game does, key words in the game's orange, and its two-line choice box
 * (MsgSubWindowC2_00) with the wobbling cursor (CrsorS_00 / 01): "Got it!" or "Don't show these again". The
 * game is paused meanwhile. Placed in the game's layout units (1280 x 720 around the TV picture's centre).
 * Controls > Touch hints switches them off; on again shows each once more.
 */
final class GameHint {
    private static final String PREF_ON = "touch_hints", PREF_SEEN = "touch_hints_seen";
    private static final int ORANGE = 0xFFFF7A2E;
    // the windows, as the game places its own (P_MsgWindow_00 at y -200, the choices above its right end)
    private static final float WIN_Y = -200, WIN_W = 900, WIN_H = 280, LINE = 44, TEXT = 38;
    private static final float CHOICE_X = 330, CHOICE_Y = -24, CHOICE_W = 400, CHOICE_H = 150, CHOICE_LINE = 52;
    private static final long TYPE_MS = 16;  // per letter

    private final View host;
    private int mode;            // the hint shown (minigame mode), 0 none
    private String message;      // its text, "*...*" in orange
    private String[] lines;      // ... in lines that fit the window
    private String[] choices;
    private int chosen;          // the cursor's line
    private long shownAt;

    GameHint(View host) { this.host = host; }

    boolean showing() { return mode != 0; }

    private static SharedPreferences prefs() { return MainActivity.instance == null ? null : MainActivity.instance.prefs; }

    static boolean enabled() {
        SharedPreferences p = prefs();
        return p != null && p.getBoolean(PREF_ON, true);
    }

    /** Controls > Touch hints: on again shows every hint once more */
    static void setEnabled(boolean on) {
        SharedPreferences p = prefs();
        if (p == null) return;
        SharedPreferences.Editor e = p.edit().putBoolean(PREF_ON, on);
        if (on) e.remove(PREF_SEEN);
        e.apply();
    }

    private static boolean seen(int m) {
        SharedPreferences p = prefs();
        return p == null || p.getStringSet(PREF_SEEN, new HashSet<>()).contains(Integer.toString(m));
    }

    private static void markSeen(int m) {
        SharedPreferences p = prefs();
        if (p == null) return;
        Set<String> s = new HashSet<>(p.getStringSet(PREF_SEEN, new HashSet<>()));
        s.add(Integer.toString(m));
        p.edit().putStringSet(PREF_SEEN, s).apply();
    }

    private static String nameOf(int m) {
        switch (m) {
            case 1: return "hint_mg_board";
            case 2: return "hint_mg_puzzle";
            case 3: return "hint_mg_mail";
            case 4: return "hint_mg_cannon";
            case 5: return "hint_mg_auction";
            case 6: return "hint_mg_choice";
            case 7: return "hint_mg_number";
            case 8: return "hint_mg_shop";
            case 9: return "hint_mg_figure";
            case 10: return "hint_mg_conduct";
            case 11: return "hint_mg_wind";
            case ControlsView.HINT_CONDUCT_PAD: return "hint_mg_conduct_pad";
            case ControlsView.HINT_LEAF_GLIDE: return "hint_leaf_glide";
            case ControlsView.HINT_RING: return "hint_ring";
            case SongListener.HINT_BUTTON: return "hint_songs_button";
            case SongListener.HINT_AUTO: return "hint_songs_auto";
            case SongListener.HINT_TRY: return "hint_songs_try";
            default: return null;
        }
    }

    /** a hint text, in the game's language (MainActivity's resources: res/values-xx) */
    private String text(String name) {
        android.content.Context c = host.getContext();
        int id = c.getResources().getIdentifier(name, "string", c.getPackageName());
        return id == 0 ? "" : c.getString(id);
    }

    /** the hint for this minigame mode, if it has one, hints are on and it wasn't shown yet: true if it shows now */
    boolean offer(int m) {
        if (showing() || !enabled() || seen(m) || nameOf(m) == null) return false;
        return show(m, text(nameOf(m)));
    }

    /** a question in the hints' look that comes each time it is asked (not a hint: shown with hints off too):
     *  SongListener.HINT_PLAY, a song recognised in practice, play it for real? */
    boolean ask(int m, String text) {
        if (showing()) return false;
        return show(m, text);
    }

    private boolean show(int m, String text) {
        mode = m;
        message = text;
        lines = null;  // wrapped once the font is there (draw)
        choices = m == SongListener.HINT_TRY ? new String[] {text("hint_try_yes"), text("hint_try_no")}
                : m == SongListener.HINT_PLAY ? new String[] {text("song_play_yes"), text("song_play_no")}
                : m == ControlsView.HINT_POSTBOX ? new String[] {text("postbox_ask_yes"), text("postbox_ask_no")}
                : new String[] {text("hint_ok"), text("hint_never")};
        chosen = 0;
        shownAt = SystemClock.uptimeMillis();
        Native.setGamePace(2);  // the game waits, as for its own messages
        host.invalidate();
        return true;
    }

    /** line: the choice taken (0 Got it, 1 Don't show these again; the songs' questions: 0 try it / play it, 1 not
     *  now / keep practising) */
    private void close(int line) {
        int was = mode;
        boolean question = was == SongListener.HINT_TRY || was == SongListener.HINT_PLAY || was == ControlsView.HINT_POSTBOX;
        if (was != SongListener.HINT_PLAY && was != ControlsView.HINT_POSTBOX) markSeen(was);
        if (line == 1 && !question) setEnabled(false);
        mode = 0;
        Native.setGamePace(0);
        host.invalidate();
        // singing: after the mic button's hint, a question whether to try it first (practice: SongListener)
        if (was == SongListener.HINT_BUTTON && line == 0) offer(SongListener.HINT_TRY);
        else if (was == SongListener.HINT_TRY && line == 0 && MainActivity.instance != null) MainActivity.instance.startSongPractice();
        else if (was == SongListener.HINT_PLAY && line == 0 && MainActivity.instance != null) MainActivity.instance.onSongPracticeEnd();
        else if (was == ControlsView.HINT_POSTBOX && line == 0) Native.setOption("postbox_present", 1);  // the game's picto chooser
    }

    // ---- touch: a tap on a choice takes it (the cursor moves there first, as the game's own choices by touch)
    private int downLine = -1;

    boolean onTouch(MotionEvent e, RectF tv) {
        if (!showing()) return false;
        int action = e.getActionMasked();
        float x = e.getX(e.getActionIndex()), y = e.getY(e.getActionIndex());
        if (action == MotionEvent.ACTION_DOWN) {
            downLine = choiceAt(tv, x, y);
            if (downLine >= 0) {
                chosen = downLine;
                host.invalidate();
            }
        } else if (action == MotionEvent.ACTION_UP) {
            int line = choiceAt(tv, x, y);
            if (line >= 0 && line == downLine && typedOut()) close(line);
            downLine = -1;
        }
        return true;  // the game gets no touches while the hint shows
    }

    // ---- a controller / the keyboard: up / down (d-pad or stick) moves the cursor, A takes the marked line, B
    // "Got it!" (a question's last line: no); afterwards the buttons stay away from the game until they are let go
    private int padPrev, dirPrev;
    private boolean padHold;

    boolean onPad(int buttons, float stickY) {
        if (!showing()) {
            if (!padHold) return false;
            if (buttons == 0 && Math.abs(stickY) < 0.3f) padHold = false;
            padPrev = buttons;
            return padHold;
        }
        int dir = (buttons & Native.UP) != 0 || stickY > 0.6f ? -1 : (buttons & Native.DOWN) != 0 || stickY < -0.6f ? 1 : 0;
        int pressed = buttons & ~padPrev;
        padPrev = buttons;
        if (dir != 0 && dir != dirPrev && choices != null) {
            chosen = Math.max(0, Math.min(choices.length - 1, chosen + dir));
            host.invalidate();
        }
        dirPrev = dir;
        if (typedOut() && (pressed & (Native.A | Native.B)) != 0) {
            padHold = true;
            boolean question = mode == SongListener.HINT_TRY || mode == SongListener.HINT_PLAY;
            close((pressed & Native.A) != 0 ? chosen : question && choices != null ? choices.length - 1 : 0);
        }
        return true;
    }

    private boolean typedOut() { return SystemClock.uptimeMillis() - shownAt >= letters() * TYPE_MS; }

    private int letters() {
        if (lines == null) return Integer.MAX_VALUE / 2;
        int n = 0;
        for (String l : lines) n += l.replace("*", "").length();
        return n;
    }

    // the text in lines no wider than the window's inside (font units at this scale): its own line breaks kept,
    // longer lines broken at spaces (or between any two letters where it has none: Japanese)
    private static final float TEXT_W = 820;

    private String[] wrap() {
        java.util.List<String> out = new java.util.ArrayList<>();
        float max = TEXT_W / (TEXT / GameFont.MSG.height());  // in font units
        for (String para : message.split("\n")) {
            StringBuilder line = new StringBuilder();
            float w = 0;
            int lastSpace = -1;
            float wAtSpace = 0;
            for (int i = 0; i < para.length(); i++) {
                char ch = para.charAt(i);
                float a = ch == '*' ? 0 : GameFont.MSG.advance(ch);
                if (w + a > max && line.length() > 0) {
                    if (lastSpace > 0) {  // break at the last space
                        String rest = line.substring(lastSpace + 1);
                        out.add(line.substring(0, lastSpace));
                        line = new StringBuilder(rest);
                        w -= wAtSpace + GameFont.MSG.advance(' ');
                    } else {
                        out.add(line.toString());
                        line = new StringBuilder();
                        w = 0;
                    }
                    lastSpace = -1;
                }
                if (ch == ' ') {
                    lastSpace = line.length();
                    wAtSpace = w;
                }
                line.append(ch);
                w += a;
            }
            out.add(line.toString());
        }
        return out.toArray(new String[0]);
    }

    private int choiceAt(RectF tv, float x, float y) {
        float k = tv.height() / 720f;
        float lx = (x - tv.centerX()) / k, ly = (tv.centerY() - y) / k;
        if (Math.abs(lx - CHOICE_X) > CHOICE_W / 2) return -1;
        for (int i = 0; i < choices.length; i++)
            if (Math.abs(ly - choiceY(i)) < CHOICE_LINE / 2) return i;
        return -1;
    }

    private static float choiceY(int i) { return CHOICE_Y + CHOICE_LINE / 2 - i * CHOICE_LINE; }

    // ---- drawing
    private final Paint img = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG), text = new Paint(Paint.FILTER_BITMAP_FLAG | Paint.ANTI_ALIAS_FLAG);
    private final RectF dst = new RectF();
    private final Rect src = new Rect();
    private final Matrix m = new Matrix();

    void draw(Canvas c, RectF tv) {
        if (!showing() || tv.isEmpty()) return;
        Runnable redraw = () -> host.post(host::invalidate);
        Bitmap win = GameIcons.get(host.getContext(), "MsgWindowMain_00^t", redraw);
        Bitmap sub = GameIcons.get(host.getContext(), "MsgSubWindowC2_00^t", redraw);
        boolean second = (SystemClock.uptimeMillis() / 250) % 2 == 1;
        Bitmap cur = GameIcons.get(host.getContext(), second ? "CrsorS_01^t" : "CrsorS_00^t", redraw);
        if (!GameFont.MSG.isReady(host.getContext(), redraw)) return;
        float k = tv.height() / 720f, cx = tv.centerX(), cy = tv.centerY();
        if (lines == null) {
            lines = wrap();
            shownAt = SystemClock.uptimeMillis();  // typed from when it can be drawn
        }
        long t = SystemClock.uptimeMillis() - shownAt;
        int alpha = (int) Math.min(255, t * 255 / 150);  // the windows fade in
        img.setAlpha(alpha * 200 / 255);                   // the game's message window lets the picture through
        // the message window
        if (win != null) {
            dst.set(cx - WIN_W / 2 * k, cy - (WIN_Y + WIN_H / 2) * k, cx + WIN_W / 2 * k, cy - (WIN_Y - WIN_H / 2) * k);
            c.drawBitmap(win, null, dst, img);
        }
        // the text, typed out
        int budget = (int) (t / TYPE_MS);
        float scale = TEXT * k / GameFont.MSG.height();
        float top = cy - (WIN_Y + LINE * lines.length / 2f) * k;
        boolean orange = false;  // (a marked phrase may go on in the next line)
        for (int i = 0; i < lines.length && budget > 0; i++) {
            String plain = lines[i].replace("*", "");
            float w = 0;
            for (int j = 0; j < plain.length(); j++) w += GameFont.MSG.advance(plain.charAt(j)) * scale;
            float x = cx - w / 2, y = top + i * LINE * k + (LINE - TEXT) / 2 * k;
            for (int j = 0; j < lines[i].length() && budget > 0; j++) {
                char ch = lines[i].charAt(j);
                if (ch == '*') {
                    orange = !orange;
                    continue;
                }
                text.setColor(0x90000000);  // a soft shadow under the letters
                text.setAlpha(alpha * 0x90 / 255);
                GameFont.MSG.draw(c, ch, x + 2 * k, y + 2 * k, scale, text);
                text.setColor(orange ? ORANGE : 0xFFFFFFFF);
                text.setAlpha(alpha);
                x += GameFont.MSG.draw(c, ch, x, y, scale, text);
                budget--;
            }
        }
        if (!typedOut()) {
            host.postInvalidateOnAnimation();
            return;
        }
        // the choices: the box's left half as the texture has it, its right half mirrored, the middle stretched
        float bx0 = cx + (CHOICE_X - CHOICE_W / 2) * k, bx1 = cx + (CHOICE_X + CHOICE_W / 2) * k;
        float by0 = cy - (CHOICE_Y + CHOICE_H / 2) * k, by1 = cy - (CHOICE_Y - CHOICE_H / 2) * k;
        if (sub != null) {
            float half = (by1 - by0) * sub.getWidth() / sub.getHeight();
            int edge = sub.getWidth() - 8;  // the texture's inner columns, stretched over the middle
            dst.set(bx0, by0, bx0 + half, by1);
            c.drawBitmap(sub, null, dst, img);
            src.set(edge, 0, sub.getWidth(), sub.getHeight());
            dst.set(bx0 + half, by0, bx1 - half, by1);
            c.drawBitmap(sub, src, dst, img);
            c.save();
            c.scale(-1, 1, (bx1 - half / 2), 0);
            dst.set(bx1 - half, by0, bx1, by1);
            c.drawBitmap(sub, null, dst, img);
            c.restore();
        }
        for (int i = 0; i < choices.length; i++) {
            String s = choices[i];
            float w = 0;
            for (int j = 0; j < s.length(); j++) w += GameFont.MSG.advance(s.charAt(j)) * scale;
            float x = cx + CHOICE_X * k - w / 2, y = cy - choiceY(i) * k - TEXT / 2 * k;
            for (int j = 0; j < s.length(); j++) {
                text.setColor(0x90000000);
                GameFont.MSG.draw(c, s.charAt(j), x + 2 * k, y + 2 * k, scale, text);
                text.setColor(0xFFFFFFFF);
                x += GameFont.MSG.draw(c, s.charAt(j), x, y, scale, text);
            }
        }
        // the cursor on the chosen line: a chevron on each side, pointing in
        if (cur != null) {
            float s = 34 * k, y = cy - choiceY(chosen) * k;
            dst.set(bx0 + 14 * k, y - s / 2, bx0 + 14 * k + s, y + s / 2);
            c.drawBitmap(cur, null, dst, null);
            c.save();
            c.scale(-1, 1, bx1 - 14 * k - s / 2, 0);
            dst.set(bx1 - 14 * k - s, y - s / 2, bx1 - 14 * k, y + s / 2);
            c.drawBitmap(cur, null, dst, null);
            c.restore();
        }
        host.postInvalidateDelayed(250);  // the cursor wobbles
    }
}
