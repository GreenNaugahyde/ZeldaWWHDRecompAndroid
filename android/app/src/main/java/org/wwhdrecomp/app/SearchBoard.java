package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.Color;
import android.graphics.LinearGradient;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RadialGradient;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.SystemClock;
import android.view.View;

import java.util.ArrayList;
import java.util.List;
import java.util.Random;

/**
 * The search for other devices as a sea battle (PadActivity's search page): an 8 x 8 sea board on a wooden frame,
 * shots fired at it one after another. While nothing is found every shot misses (a splash, a white peg); a device
 * found gets a ship of its own hidden on the board and the next shots hit it (an explosion, red pegs) until it shows,
 * its name on a flag. A device gone: its ship sinks. All drawn here (no pictures of the game: the screen-only app has
 * no game files).
 */
final class SearchBoard extends View {
    private static final int N = 8;
    private static final long SHOT_MS = 1100, HIT_SHOT_MS = 520, FLIGHT_MS = 430, SPLASH_MS = 650, BOOM_MS = 800, SINK_MS = 900;

    private static final class Ship {
        final String name;
        final int r, c, len;
        final boolean across;
        int hits;
        long shownAt, goneAt;

        Ship(String name, int r, int c, int len, boolean across) {
            this.name = name;
            this.r = r;
            this.c = c;
            this.len = len;
            this.across = across;
        }

        int cellR(int i) { return across ? r : r + i; }

        int cellC(int i) { return across ? c + i : c; }

        boolean covers(int rr, int cc) {
            for (int i = 0; i < len; i++) if (cellR(i) == rr && cellC(i) == cc) return true;
            return false;
        }
    }

    private static final class Shot {
        final int r, c;
        final Ship ship;  // null: a miss
        final long at;

        Shot(int r, int c, Ship ship, long at) {
            this.r = r;
            this.c = c;
            this.ship = ship;
            this.at = at;
        }
    }

    private final int[][] cells = new int[N][N];  // 0 untried, 1 a miss, 2 a hit
    private final long[][] markedAt = new long[N][N];
    private final List<Ship> ships = new ArrayList<>();
    private final List<Shot> shots = new ArrayList<>();
    private final Random rnd = new Random();
    private long nextShot, clearAt;
    private boolean searching = true;

    private final Paint p = new Paint(Paint.ANTI_ALIAS_FLAG), stroke = new Paint(Paint.ANTI_ALIAS_FLAG),
            label = new Paint(Paint.ANTI_ALIAS_FLAG);
    private final RectF board = new RectF(), tmp = new RectF();
    private final Path path = new Path();
    private float cell;

    SearchBoard(Context c) {
        super(c);
        stroke.setStyle(Paint.Style.STROKE);
        label.setTextAlign(Paint.Align.CENTER);
        label.setFakeBoldText(true);
        nextShot = SystemClock.uptimeMillis() + 500;
    }

    /** the devices found now (by name): a ship each, hidden and then hit; the others' ships sink */
    void setDevices(List<String> names) {
        long now = SystemClock.uptimeMillis();
        for (Ship s : ships)
            if (s.goneAt == 0 && !names.contains(s.name)) s.goneAt = now;
        for (String n : names) {
            boolean have = false;
            for (Ship s : ships) if (s.name.equals(n) && s.goneAt == 0) have = true;
            if (!have) hide(n);
        }
        nextShot = Math.min(nextShot, now + 250);
        invalidate();
    }

    /** how long the ship of this device has shown (ms; -1: not yet, or no such ship) */
    long shownFor(String name) {
        for (Ship s : ships)
            if (s.name.equals(name) && s.goneAt == 0 && s.shownAt != 0) return SystemClock.uptimeMillis() - s.shownAt;
        return -1;
    }

    /** the search goes on (shots keep coming) or has stopped (connecting: the board stays as it is) */
    void setSearching(boolean on) {
        searching = on;
        invalidate();
    }

    private int liveShips() {
        int n = 0;
        for (Ship s : ships) if (s.goneAt == 0) n++;
        return n;
    }

    /** a ship for this device somewhere not shot at yet (the misses cleared first if there is no room) */
    private void hide(String name) {
        int len = liveShips() % 2 == 0 ? 3 : 2;
        for (int round = 0; round < 2; round++) {
            for (int tries = 0; tries < 200; tries++) {
                boolean across = rnd.nextBoolean();
                int r = rnd.nextInt(across ? N : N - len + 1), c = rnd.nextInt(across ? N - len + 1 : N);
                Ship s = new Ship(name, r, c, len, across);
                boolean free = true;
                for (int i = 0; i < len && free; i++) {
                    int rr = s.cellR(i), cc = s.cellC(i);
                    if (cells[rr][cc] != 0) free = false;
                    for (Ship o : ships)
                        if (o.goneAt == 0 && (o.covers(rr, cc) || o.covers(rr - 1, cc) || o.covers(rr + 1, cc)
                                || o.covers(rr, cc - 1) || o.covers(rr, cc + 1))) free = false;
                }
                if (free) {
                    ships.add(s);
                    return;
                }
            }
            clearMisses();
        }
    }

    private void clearMisses() {
        for (int r = 0; r < N; r++)
            for (int c = 0; c < N; c++)
                if (cells[r][c] == 1) cells[r][c] = 0;
    }

    private boolean hitByShip(int r, int c) {
        for (Ship s : ships) if (s.goneAt == 0 && s.covers(r, c)) return true;
        return false;
    }

    /** the next shot: a ship not shown yet first (its cells in turn), else a cell no ship is on */
    private void fire(long now) {
        for (Ship s : ships) {
            if (s.goneAt != 0 || s.hits >= s.len) continue;
            boolean flying = false;
            for (Shot sh : shots) if (sh.ship == s) flying = true;
            if (flying) return;  // (one at a time on a ship: they come in turn)
            int i = s.hits;
            shots.add(new Shot(s.cellR(i), s.cellC(i), s, now));
            nextShot = now + HIT_SHOT_MS;
            return;
        }
        if (!searching) return;
        List<int[]> free = new ArrayList<>();
        for (int r = 0; r < N; r++)
            for (int c = 0; c < N; c++)
                if (cells[r][c] == 0 && !hitByShip(r, c)) {
                    boolean flying = false;
                    for (Shot sh : shots) if (sh.r == r && sh.c == c) flying = true;
                    if (!flying) free.add(new int[] {r, c});
                }
        int misses = 0;
        for (int r = 0; r < N; r++) for (int c = 0; c < N; c++) if (cells[r][c] == 1) misses++;
        if (free.size() < 6 || misses > N * N * 2 / 3) {  // the board full of misses: a fresh sea
            if (clearAt == 0) clearAt = now;
            nextShot = now + SHOT_MS;
            return;
        }
        int[] t = free.get(rnd.nextInt(free.size()));
        shots.add(new Shot(t[0], t[1], null, now));
        nextShot = now + SHOT_MS + rnd.nextInt(400);
    }

    @Override
    protected void onDraw(Canvas cv) {
        long now = SystemClock.uptimeMillis();
        if (now >= nextShot) fire(now);
        if (clearAt != 0 && now - clearAt > 700) {
            clearMisses();
            clearAt = 0;
        }
        for (int i = ships.size() - 1; i >= 0; i--)
            if (ships.get(i).goneAt != 0 && now - ships.get(i).goneAt > SINK_MS) {
                Ship s = ships.remove(i);
                for (int k = 0; k < s.len; k++) cells[s.cellR(k)][s.cellC(k)] = 0;
            }
        layoutBoard();
        drawFrame(cv);
        drawSea(cv, now);
        drawShips(cv, now);
        drawPegs(cv, now);
        drawShots(cv, now);
        postInvalidateOnAnimation();
    }

    private void layoutBoard() {
        float w = getWidth(), h = getHeight(), side = Math.min(w, h) * 0.94f;
        float frame = side * 0.07f;
        cell = (side - 2 * frame) / N;
        float left = (w - side) / 2 + frame, top = (h - side) / 2 + frame;
        board.set(left, top, left + cell * N, top + cell * N);
    }

    private float cx(int c) { return board.left + (c + 0.5f) * cell; }

    private float cy(int r) { return board.top + (r + 0.5f) * cell; }

    // the wooden frame with the letters and numbers
    private void drawFrame(Canvas cv) {
        float f = cell * 0.56f;
        tmp.set(board.left - f, board.top - f, board.right + f, board.bottom + f);
        p.setColor(Color.BLACK);  // (opaque: with a shader the paint's alpha still counts)
        p.setShader(new LinearGradient(tmp.left, tmp.top, tmp.right, tmp.bottom, 0xFFB07A44, 0xFF6A4220, Shader.TileMode.CLAMP));
        cv.drawRoundRect(tmp, f * 0.5f, f * 0.5f, p);
        p.setShader(null);
        stroke.setStrokeWidth(Math.max(2, f * 0.08f));
        stroke.setColor(0xFF3A2410);
        cv.drawRoundRect(tmp, f * 0.5f, f * 0.5f, stroke);
        // the planks' grain
        stroke.setStrokeWidth(Math.max(1, f * 0.03f));
        stroke.setColor(0x403A2410);
        for (int k = 1; k < 4; k++) {
            float t = f * k / 4;
            cv.drawLine(tmp.left + f * 0.4f, tmp.top + t, tmp.right - f * 0.4f, tmp.top + t, stroke);
            cv.drawLine(tmp.left + f * 0.4f, tmp.bottom - t, tmp.right - f * 0.4f, tmp.bottom - t, stroke);
        }
        label.setTextSize(f * 0.55f);
        label.setColor(0xFFFFE8B8);
        for (int i = 0; i < N; i++) {
            cv.drawText(String.valueOf((char) ('A' + i)), cx(i), board.top - f * 0.3f, label);
            cv.drawText(String.valueOf(i + 1), board.left - f * 0.5f, cy(i) + label.getTextSize() * 0.36f, label);
        }
    }

    // the sea in the cells, waves going by
    private void drawSea(Canvas cv, long now) {
        p.setColor(Color.BLACK);
        p.setShader(new LinearGradient(0, board.top, 0, board.bottom, 0xFF3A9BE0, 0xFF1A5C9E, Shader.TileMode.CLAMP));
        cv.drawRect(board, p);
        p.setShader(null);
        stroke.setStrokeWidth(Math.max(1, cell * 0.035f));
        float t = (now % 4000) / 4000f;
        for (int r = 0; r < N; r++)
            for (int c = 0; c < N; c++) {
                float x = board.left + c * cell, y = board.top + r * cell;
                float ph = ((r * 3 + c * 5) % 7) / 7f, a = (float) (0.5 + 0.5 * Math.sin((t + ph) * Math.PI * 2));
                stroke.setColor(Color.argb(Math.round(30 + 50 * a), 255, 255, 255));
                float wy = y + cell * (0.35f + 0.3f * ((t + ph) % 1f));
                path.reset();
                path.moveTo(x + cell * 0.2f, wy);
                path.quadTo(x + cell * 0.35f, wy - cell * 0.08f, x + cell * 0.5f, wy);
                path.quadTo(x + cell * 0.65f, wy + cell * 0.08f, x + cell * 0.8f, wy);
                cv.drawPath(path, stroke);
            }
        stroke.setStrokeWidth(Math.max(1, cell * 0.03f));
        stroke.setColor(0x60FFFFFF);
        for (int i = 1; i < N; i++) {
            cv.drawLine(board.left + i * cell, board.top, board.left + i * cell, board.bottom, stroke);
            cv.drawLine(board.left, board.top + i * cell, board.right, board.top + i * cell, stroke);
        }
        stroke.setStrokeWidth(Math.max(2, cell * 0.05f));
        stroke.setColor(0xFF12385E);
        cv.drawRect(board, stroke);
    }

    // a ship once all its cells are hit (a hull, a mast, the device's name on a flag); sinking when gone
    private void drawShips(Canvas cv, long now) {
        for (Ship s : ships) {
            if (s.hits < s.len || s.shownAt == 0) continue;
            float in = Math.min(1, (now - s.shownAt) / 400f), sink = s.goneAt == 0 ? 0 : Math.min(1, (now - s.goneAt) / (float) SINK_MS);
            int alpha = Math.round(255 * in * (1 - sink));
            float x0 = board.left + s.c * cell, y0 = board.top + s.r * cell;
            float w = s.across ? s.len * cell : cell, h = s.across ? cell : s.len * cell;
            cv.save();
            cv.translate(0, sink * cell * 0.6f);
            float m = cell * 0.06f;
            tmp.set(x0 + m, y0 + m, x0 + w - m, y0 + h - m);
            path.reset();  // the hull: pointed at the bow
            if (s.across) {
                path.moveTo(tmp.left, tmp.top + tmp.height() * 0.1f);
                path.lineTo(tmp.right - tmp.height() * 0.5f, tmp.top + tmp.height() * 0.1f);
                path.lineTo(tmp.right, tmp.centerY());
                path.lineTo(tmp.right - tmp.height() * 0.5f, tmp.bottom - tmp.height() * 0.1f);
                path.lineTo(tmp.left, tmp.bottom - tmp.height() * 0.1f);
            } else {
                path.moveTo(tmp.left + tmp.width() * 0.1f, tmp.bottom);
                path.lineTo(tmp.left + tmp.width() * 0.1f, tmp.top + tmp.width() * 0.5f);
                path.lineTo(tmp.centerX(), tmp.top);
                path.lineTo(tmp.right - tmp.width() * 0.1f, tmp.top + tmp.width() * 0.5f);
                path.lineTo(tmp.right - tmp.width() * 0.1f, tmp.bottom);
            }
            path.close();
            p.setColor(Color.argb(alpha, 120, 72, 34));
            cv.drawPath(path, p);
            stroke.setStrokeWidth(Math.max(2, cell * 0.05f));
            stroke.setColor(Color.argb(alpha, 58, 34, 14));
            cv.drawPath(path, stroke);
            // the deck's planks
            stroke.setStrokeWidth(Math.max(1, cell * 0.025f));
            stroke.setColor(Color.argb(alpha / 2, 255, 230, 190));
            if (s.across) cv.drawLine(tmp.left + m, tmp.centerY(), tmp.right - tmp.height() * 0.6f, tmp.centerY(), stroke);
            else cv.drawLine(tmp.centerX(), tmp.top + tmp.width() * 0.6f, tmp.centerX(), tmp.bottom - m, stroke);
            cv.restore();
        }
        // the flags with the names on top of everything else on the board
        for (Ship s : ships) {
            if (s.hits < s.len || s.shownAt == 0) continue;
            float in = Math.min(1, (now - s.shownAt) / 400f), sink = s.goneAt == 0 ? 0 : Math.min(1, (now - s.goneAt) / (float) SINK_MS);
            int alpha = Math.round(255 * in * (1 - sink));
            // (the flag above the ship: over its middle across, over its bow (top) when it lies down the board)
            float mx = s.across ? board.left + (s.c + s.len / 2f) * cell : cx(s.c), my = s.across ? cy(s.r) : board.top + (s.r + 0.1f) * cell;
            label.setTextSize(cell * 0.36f);
            String n = s.name;
            float maxW = cell * 4.2f;
            while (n.length() > 3 && label.measureText(n) > maxW) n = n.substring(0, n.length() - 2) + "…";
            float tw = label.measureText(n), fh = cell * 0.5f;
            float fy = my - (s.across ? cell * 0.55f : cell * 0.15f) - fh;
            float fx = Math.max(board.left + tw / 2 + cell * 0.2f, Math.min(board.right - tw / 2 - cell * 0.2f, mx));
            if (fy < board.top) fy = s.across ? my + cell * 0.55f : board.top + (s.r + s.len) * cell + cell * 0.1f;
            stroke.setStrokeWidth(Math.max(2, cell * 0.05f));
            stroke.setColor(Color.argb(alpha, 70, 44, 20));
            cv.drawLine(mx, my, mx, fy + fh / 2, stroke);  // the mast
            tmp.set(fx - tw / 2 - cell * 0.18f, fy, fx + tw / 2 + cell * 0.18f, fy + fh);
            p.setColor(Color.argb(alpha, 255, 246, 214));
            cv.drawRoundRect(tmp, fh * 0.25f, fh * 0.25f, p);
            stroke.setColor(Color.argb(alpha, 200, 60, 40));
            cv.drawRoundRect(tmp, fh * 0.25f, fh * 0.25f, stroke);
            label.setColor(Color.argb(alpha, 60, 30, 10));
            cv.drawText(n, fx, fy + fh * 0.7f, label);
        }
    }

    // white pegs for misses, red ones (with a flicker of fire) for hits
    private void drawPegs(Canvas cv, long now) {
        float fade = clearAt == 0 ? 1 : Math.max(0, 1 - (now - clearAt) / 700f);
        for (int r = 0; r < N; r++)
            for (int c = 0; c < N; c++) {
                if (cells[r][c] == 0) continue;
                float x = cx(c), y = cy(r), pop = Math.min(1, (now - markedAt[r][c]) / 200f), rad = cell * 0.17f * (0.6f + 0.4f * pop);
                boolean hit = cells[r][c] == 2;
                int a = Math.round(255 * (hit ? 1 : fade));
                p.setColor(Color.argb(a / 3, 0, 0, 0));
                cv.drawCircle(x + rad * 0.15f, y + rad * 0.2f, rad, p);
                p.setShader(new RadialGradient(x - rad * 0.3f, y - rad * 0.3f, rad * 1.3f,
                        hit ? 0xFFFF9A7A : 0xFFFFFFFF, hit ? 0xFFC0281A : 0xFFB8C8D8, Shader.TileMode.CLAMP));
                p.setAlpha(a);
                cv.drawCircle(x, y, rad, p);
                p.setShader(null);
                if (hit) {  // a little fire
                    float fl = (float) (0.75 + 0.25 * Math.sin((now + r * 97 + c * 61) / 90.0));
                    p.setColor(Color.argb(180, 255, 170, 40));
                    path.reset();
                    path.moveTo(x - rad * 0.6f, y - rad * 0.6f);
                    path.quadTo(x, y - rad * (1.2f + 1.4f * fl), x + rad * 0.6f, y - rad * 0.6f);
                    path.close();
                    cv.drawPath(path, p);
                }
            }
    }

    // the shots: a cannonball coming from the viewer in an arc, shrinking, then the splash or the explosion
    private void drawShots(Canvas cv, long now) {
        for (int i = shots.size() - 1; i >= 0; i--) {
            Shot s = shots.get(i);
            long t = now - s.at;
            float tx = cx(s.c), ty = cy(s.r);
            if (t < FLIGHT_MS) {
                float k = t / (float) FLIGHT_MS;
                float sx = getWidth() / 2f, sy = getHeight() + cell;
                float x = sx + (tx - sx) * k, y = sy + (ty - sy) * k - (float) Math.sin(k * Math.PI) * cell * 3.2f;
                float rad = cell * (0.55f - 0.38f * k);
                p.setColor(Color.argb(Math.round(90 * k), 0, 0, 0));  // its shadow on the water, nearer as it falls
                cv.drawOval(tx - rad, ty - rad * 0.45f + rad * 0.2f, tx + rad, ty + rad * 0.45f + rad * 0.2f, p);
                p.setColor(Color.BLACK);
                p.setShader(new RadialGradient(x - rad * 0.35f, y - rad * 0.35f, rad * 1.4f, 0xFF8A8A8A, 0xFF101010, Shader.TileMode.CLAMP));
                cv.drawCircle(x, y, rad, p);
                p.setShader(null);
                continue;
            }
            if (s.ship == null ? cells[s.r][s.c] == 0 : cells[s.r][s.c] != 2) land(s, now);
            long e = t - FLIGHT_MS;
            if (s.ship == null) {
                if (e > SPLASH_MS) {
                    shots.remove(i);
                    continue;
                }
                drawSplash(cv, tx, ty, e / (float) SPLASH_MS);
            } else {
                if (e > BOOM_MS) {
                    shots.remove(i);
                    continue;
                }
                drawBoom(cv, tx, ty, e / (float) BOOM_MS, s.r * 31 + s.c);
            }
        }
    }

    private void land(Shot s, long now) {
        cells[s.r][s.c] = s.ship == null ? 1 : 2;
        markedAt[s.r][s.c] = now;
        if (s.ship != null) {
            s.ship.hits++;
            if (s.ship.hits >= s.ship.len) s.ship.shownAt = now;
            performHapticFeedback(android.view.HapticFeedbackConstants.CONFIRM);
        }
    }

    private void drawSplash(Canvas cv, float x, float y, float k) {
        for (int ring = 0; ring < 2; ring++) {
            float kk = Math.max(0, k - ring * 0.2f);
            stroke.setStrokeWidth(Math.max(1.5f, cell * 0.06f * (1 - kk)));
            stroke.setColor(Color.argb(Math.round(220 * (1 - kk)), 255, 255, 255));
            cv.drawOval(x - cell * 0.6f * kk, y - cell * 0.3f * kk, x + cell * 0.6f * kk, y + cell * 0.3f * kk, stroke);
        }
        // the column of water and its drops
        float up = (float) Math.sin(Math.min(1, k * 1.6f) * Math.PI), a = 1 - k;
        p.setColor(Color.argb(Math.round(200 * a), 225, 245, 255));
        path.reset();
        path.moveTo(x - cell * 0.22f, y);
        path.quadTo(x, y - cell * 1.3f * up, x + cell * 0.22f, y);
        path.close();
        cv.drawPath(path, p);
        for (int d = 0; d < 7; d++) {
            double ang = Math.PI * (0.15 + 0.7 * d / 6.0);
            float dist = cell * 0.9f * k, dx = (float) (Math.cos(ang) * dist), dy = (float) (-Math.sin(ang) * dist * 1.4f + cell * 1.6f * k * k);
            cv.drawCircle(x + dx, y + dy, cell * 0.06f * (1 - k * 0.5f), p);
        }
        if (k < 0.75f) word(cv, getContext().getString(R.string.search_splash), x, y - cell * 1.1f, k / 0.75f, 0xFFFFFFFF, 0xFF1A5C9E);
    }

    private void drawBoom(Canvas cv, float x, float y, float k, int seed) {
        if (k < 0.15f) {  // the flash
            p.setColor(Color.argb(Math.round(200 * (1 - k / 0.15f)), 255, 255, 220));
            cv.drawCircle(x, y, cell * 1.4f, p);
        }
        float r = cell * (0.3f + 0.9f * (float) Math.sqrt(k)), a = 1 - k;
        p.setShader(new RadialGradient(x, y, Math.max(1, r), new int[] {0xFFFFF4B0, 0xFFFF9A1A, 0x00C02010}, new float[] {0, 0.45f, 1},
                Shader.TileMode.CLAMP));
        p.setAlpha(Math.round(255 * a));
        cv.drawCircle(x, y, r, p);
        p.setShader(null);
        Random sr = new Random(seed);
        for (int d = 0; d < 10; d++) {  // sparks and smoke
            double ang = sr.nextDouble() * Math.PI * 2;
            float dist = cell * (0.6f + sr.nextFloat()) * k * 1.3f;
            float px = x + (float) Math.cos(ang) * dist, py = y + (float) Math.sin(ang) * dist - cell * 0.6f * k;
            if (d % 2 == 0) {
                p.setColor(Color.argb(Math.round(230 * a), 255, 200, 60));
                cv.drawCircle(px, py, cell * 0.07f, p);
            } else {
                p.setColor(Color.argb(Math.round(120 * a), 60, 60, 60));
                cv.drawCircle(px, py - cell * 0.4f * k, cell * (0.12f + 0.25f * k), p);
            }
        }
        if (k < 0.8f) word(cv, getContext().getString(R.string.search_boom), x, y - cell * 1.2f, k / 0.8f, 0xFFFFE04A, 0xFFB01E10);
    }

    /** a word popping out above a shot */
    private void word(Canvas cv, String w, float x, float y, float k, int color, int edge) {
        float s = k < 0.2f ? k / 0.2f * 1.15f : k < 0.3f ? 1.15f - (k - 0.2f) * 1.5f : 1f;
        int a = Math.round(255 * (k > 0.7f ? (1 - k) / 0.3f : 1));
        label.setTextSize(cell * 0.55f * s);
        x = Math.max(board.left + label.measureText(w) / 2, Math.min(board.right - label.measureText(w) / 2, x));
        y = Math.max(board.top + label.getTextSize(), y);
        stroke.setStrokeWidth(cell * 0.09f * s);
        stroke.setColor(edge);
        stroke.setAlpha(a);
        stroke.setTextAlign(Paint.Align.CENTER);
        stroke.setTextSize(label.getTextSize());
        stroke.setFakeBoldText(true);
        stroke.setStrokeJoin(Paint.Join.ROUND);
        cv.drawText(w, x, y, stroke);
        label.setColor(color);
        label.setAlpha(a);
        cv.drawText(w, x, y, label);
    }
}
