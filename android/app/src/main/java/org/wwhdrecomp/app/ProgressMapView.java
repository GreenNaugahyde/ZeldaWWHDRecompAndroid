package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.ColorMatrix;
import android.graphics.ColorMatrixColorFilter;
import android.graphics.DashPathEffect;
import android.graphics.Paint;
import android.graphics.RectF;
import android.graphics.Shader;
import android.view.MotionEvent;
import android.view.View;

import java.util.HashMap;
import java.util.List;
import java.util.Map;

/**
 * The game progress's map (Game > Game progress > Map, OptionsMenu.progressMapPage): the whole sea chart in the
 * GamePad map's look (MiniMap's artwork: the paper, each square's island once the fishman filled it in, else a
 * question mark) with a mark at each place ProgressPlaces knows the spot of (the thing's own icon; grey and faint
 * where it is still missing; a question mark on those found but not sure which, MAYBE), and on each square how many of
 * its things are found (of how many, when missing ones show; green when all). A tap picks a square (Listener). A
 * dungeon's map (showDungeon): its floor's room pictures (MiniMapDungeons, placed by the stage's DMAP) with the
 * places inside it on that floor.
 */
final class ProgressMapView extends View {
    interface Listener { void square(int square); }

    static final int SHOW_FOUND = 0, SHOW_BOTH = 1, SHOW_MISSING = 2;
    private static final float SQUARE = 100000f;

    private final Listener listener;
    private byte[] save;
    private int[] states;
    // the places shown: the game progress's, or the randomizer's spoiler log's (setPlaces)
    private List<ProgressPlaces.Place> places = java.util.Arrays.asList(ProgressPlaces.ALL);
    private boolean allIslands;
    private int show, group, selected;
    private final Map<String, Bitmap> cache = new HashMap<>();
    private final Paint bmp = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG), fill = new Paint(Paint.ANTI_ALIAS_FLAG), paper = new Paint(Paint.FILTER_BITMAP_FLAG);
    private final RectF r = new RectF();
    private static final ColorMatrixColorFilter GREY;
    static {
        ColorMatrix m = new ColorMatrix();
        m.setSaturation(0);
        GREY = new ColorMatrixColorFilter(m);
    }

    // a dungeon shown (null: the sea)
    private String dungeonStage;
    private MiniMapDungeons.Dungeon dungeon;
    private float[] info;  // Native.miniMapStageInfo: DMAP origin x, z, scale (of the stage placing the pictures), FLOR pairs
    private int floor;
    private final java.util.concurrent.ExecutorService worker = java.util.concurrent.Executors.newSingleThreadExecutor();

    ProgressMapView(Context c, Listener l) {
        super(c);
        listener = l;
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
    }

    void set(byte[] d, int[] st, int show, int group, int selected) {
        save = d;
        states = st;
        this.show = show;
        this.group = group;
        this.selected = selected;
        invalidate();
    }

    void setPlaces(List<ProgressPlaces.Place> l) {
        places = l;
        allIslands = true;  // (the spoiler log: no save to say which squares are charted)
        invalidate();
    }

    private List<ProgressPlaces.Place> inSquare(int sq) {
        List<ProgressPlaces.Place> l = new java.util.ArrayList<>();
        for (ProgressPlaces.Place p : places) if (p.square == sq && p.inGroup(group)) l.add(p);
        return l;
    }

    void select(int square) {
        selected = square;
        invalidate();
    }

    /** a dungeon's map (its stage, as MiniMapDungeons has it), or the sea again (null); then done when its data is read */
    void showDungeon(String stage, Runnable done) {
        dungeonStage = stage;
        dungeon = stage != null ? MiniMapDungeons.of(stage) : null;
        info = null;
        invalidate();
        if (dungeon == null) return;
        String dir = MainActivity.instance != null ? MainActivity.instance.gameDir() : null;
        if (dir == null) return;
        MiniMapDungeons.Dungeon dn = dungeon;
        worker.execute(() -> {
            float[] i = Native.miniMapStageInfo(dir, stage);
            if (i != null && !dn.mapStage.equals(stage)) {
                float[] o = Native.miniMapStageInfo(dir, dn.mapStage);
                if (o != null) System.arraycopy(o, 0, i, 0, 3);
            }
            post(() -> {
                if (dn != dungeon) return;
                info = i;
                int[] f = floors();
                if (f.length > 0) floor = f[f.length - 1] <= 0 && f[0] >= 0 ? 0 : f[0];
                invalidate();
                if (done != null) done.run();
            });
        });
    }

    String dungeonStage() { return dungeonStage; }

    /** the dungeon's floors with pictures, top first (0 = 1F, -1 = B1 ...) */
    int[] floors() {
        if (dungeon == null) return new int[0];
        java.util.TreeSet<Integer> s = new java.util.TreeSet<>(java.util.Collections.reverseOrder());
        for (MiniMapDungeons.Pic p : dungeon.pics) s.add(p.floor);
        int[] out = new int[s.size()];
        int k = 0;
        for (int f : s) out[k++] = f;
        return out;
    }

    int floor() { return floor; }

    void setFloor(int f) {
        floor = f;
        invalidate();
    }

    @Override
    protected void onMeasure(int w, int h) {
        // square, at most 80 % of the screen's height (a phone held sideways sees it whole)
        int side = Math.min(MeasureSpec.getSize(w), (int) (getResources().getDisplayMetrics().heightPixels * 0.8f));
        setMeasuredDimension(side, side);
    }

    @Override
    public boolean onTouchEvent(MotionEvent e) {
        if (dungeon != null) return false;
        if (e.getAction() == MotionEvent.ACTION_DOWN) return true;
        if (e.getAction() == MotionEvent.ACTION_UP) {
            float side = getWidth() / 7f;
            int col = (int) (e.getX() / side), row = (int) (e.getY() / side);
            if (col >= 0 && col < 7 && row >= 0 && row < 7) {
                int sq = row * 7 + col + 1;
                select(sq);
                listener.square(sq);
                performClick();
            }
            return true;
        }
        return super.onTouchEvent(e);
    }

    @Override
    public boolean performClick() { return super.performClick(); }

    private Bitmap tinted(String texture, int black, int white, int alpha) {
        String key = texture + "#" + Integer.toHexString(black) + Integer.toHexString(white) + "/" + alpha;
        Bitmap b = cache.get(key);
        if (b != null) return b;
        Bitmap src = GameIcons.get(getContext(), texture, () -> post(this::invalidate));
        if (src == null) return null;
        b = MiniMap.tint(src, black, white, alpha);
        cache.put(key, b);
        return b;
    }

    /** the paper: the middle half of MapArea_00, tinted, mirrored over the whole */
    private void drawPaper(Canvas c, boolean sea) {
        String key = sea ? "#paper" : "#paper_in";
        Bitmap tile = cache.get(key);
        if (tile == null) {
            Bitmap t = sea ? tinted("MapArea_00^t", MiniMap.PAPER_BLACK, MiniMap.PAPER_WHITE, 0xFF)
                    : tinted("MapArea_00^t", MiniMap.INDOOR_PAPER_BLACK, MiniMap.INDOOR_PAPER_WHITE, 0xFF);
            if (t != null) {
                tile = Bitmap.createBitmap(t, t.getWidth() / 4, t.getHeight() / 4, t.getWidth() / 2, t.getHeight() / 2);
                cache.put(key, tile);
            }
        }
        r.set(0, 0, getWidth(), getHeight());
        if (tile == null) {
            fill.setColor(0xFF000000 | (sea ? MiniMap.PAPER_WHITE : MiniMap.INDOOR_PAPER_WHITE));
            c.drawRect(r, fill);
        } else {
            BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
            android.graphics.Matrix m = new android.graphics.Matrix();
            float k = getWidth() / 3.5f / tile.getWidth();
            m.setScale(k, k);
            sh.setLocalMatrix(m);
            paper.setShader(sh);
            c.drawRect(r, paper);
        }
        line.setStyle(Paint.Style.STROKE);
        line.setPathEffect(null);
        line.setStrokeWidth(Math.max(2, getWidth() * 0.006f));
        line.setColor(MiniMap.EDGE);
        c.drawRect(r, line);
    }

    @Override
    protected void onDraw(Canvas c) {
        if (dungeon != null) {
            drawDungeon(c);
            return;
        }
        drawPaper(c, true);
        float side = getWidth() / 7f;
        // the islands (filled in by the fishman) or a question mark
        for (int sq = 1; sq <= 49; sq++) {
            int col = (sq - 1) % 7, row = (sq - 1) / 7;
            r.set(col * side, row * side, (col + 1) * side, (row + 1) * side);
            boolean filled = allIslands || save != null && (save[0x104 + sq - 1] & 1) != 0;
            if (filled) {
                Bitmap isl = tinted(GameIcons.mapIsland(sq, false), MiniMap.ISLAND_BLACK, MiniMap.ISLAND_WHITE, 0xFF);
                if (isl != null) {
                    bmp.setAlpha(255);
                    bmp.setColorFilter(null);
                    c.drawBitmap(isl, null, r, bmp);
                }
            } else {
                text.setColor(MiniMap.UNKNOWN);
                text.setStyle(Paint.Style.FILL);
                text.setTextSize(side * 0.5f);
                c.drawText("?", r.centerX(), r.centerY() + side * 0.18f, text);
            }
        }
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(Math.max(1, getWidth() * 0.003f));
        line.setPathEffect(new DashPathEffect(new float[] {side * 0.08f, side * 0.06f}, 0));
        line.setColor(MiniMap.GRID);
        for (int i = 1; i < 7; i++) {
            c.drawLine(i * side, 0, i * side, getHeight(), line);
            c.drawLine(0, i * side, getWidth(), i * side, line);
        }
        line.setPathEffect(null);
        if (states == null) return;
        // the marks at their spots
        int point = save != null ? ProgressPlaces.salvagePoint(save) : 0;
        for (ProgressPlaces.Place p : places) {
            if (p.stage != null || p.kind == ProgressPlaces.SQUARE || !p.inGroup(group) || !shown(states[p.index])) continue;
            float x = p.x, z = p.z;
            int chart = p.sunkenChart();
            if (chart > 0) {
                float[] s = ProgressPlaces.sunkenSpot(chart, point);
                if (s == null) continue;
                x = s[0];
                z = s[1];
            }
            if (Float.isNaN(x)) continue;
            float px = (x + 3.5f * SQUARE) / (7 * SQUARE) * getWidth(), py = (z + 3.5f * SQUARE) / (7 * SQUARE) * getHeight();
            mark(c, p, px, py, side * 0.24f);
        }
        // each square's count; the one picked framed
        for (int sq = 1; sq <= 49; sq++) {
            int col = (sq - 1) % 7, row = (sq - 1) / 7;
            int found = 0, all = 0, shownN = 0;
            for (ProgressPlaces.Place p : inSquare(sq)) {
                int s = states[p.index];
                all++;
                if (s != ProgressPlaces.MISSING) found++;
                if (shown(s)) shownN++;
            }
            // (green: everything of the square found, also when only the found ones show)
            if (shownN > 0) badge(c, col * side, row * side, side, show == SHOW_MISSING ? String.valueOf(all - found)
                    : show == SHOW_BOTH ? found + "/" + all : String.valueOf(found), found == all);
            if (sq == selected) {
                r.set(col * side, row * side, (col + 1) * side, (row + 1) * side);
                r.inset(side * 0.03f, side * 0.03f);
                line.setStrokeWidth(Math.max(3, side * 0.05f));
                line.setColor(0xFFFF4020);
                c.drawRoundRect(r, side * 0.08f, side * 0.08f, line);
            }
        }
    }

    private boolean shown(int state) {
        switch (show) {
            case SHOW_FOUND: return state != ProgressPlaces.MISSING;
            case SHOW_MISSING: return state != ProgressPlaces.FOUND;
            default: return true;
        }
    }

    private void mark(Canvas c, ProgressPlaces.Place p, float px, float py, float size) {
        drawMark(c, getContext(), p, states[p.index], px, py, size, () -> post(this::invalidate));
    }

    private static final Paint MARK_BMP = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG), MARK_FILL = new Paint(Paint.ANTI_ALIAS_FLAG),
            MARK_TEXT = new Paint(Paint.ANTI_ALIAS_FLAG);
    private static final RectF MARK_R = new RectF();
    static {
        MARK_TEXT.setTextAlign(Paint.Align.CENTER);
        MARK_TEXT.setFakeBoldText(true);
    }

    /** a place's mark: its icon on a small light disc (grey and faint while missing, a question mark when MAYBE); also
     *  ControlsView's on the game's own sea chart. UI thread only */
    static void drawMark(Canvas c, Context ctx, ProgressPlaces.Place p, int s, float px, float py, float size, Runnable ready) {
        String tex = ProgressPlaces.icon(p);
        Bitmap b = tex != null ? GameIcons.get(ctx, tex, ready) : null;
        MARK_FILL.setStyle(Paint.Style.FILL);
        MARK_FILL.setColor(s == ProgressPlaces.MISSING ? 0x66302010 : 0xB0FFFFE7);
        c.drawCircle(px, py, size * 0.55f, MARK_FILL);
        if (b != null) {
            float k = size * 0.9f / Math.max(b.getWidth(), b.getHeight()), w = b.getWidth() * k, h = b.getHeight() * k;
            MARK_R.set(px - w / 2, py - h / 2, px + w / 2, py + h / 2);
            MARK_BMP.setColorFilter(s == ProgressPlaces.MISSING ? GREY : null);
            MARK_BMP.setAlpha(s == ProgressPlaces.MISSING ? 110 : 255);
            c.drawBitmap(b, null, MARK_R, MARK_BMP);
        }
        if (s == ProgressPlaces.MAYBE) {
            float ts = size * 0.55f, x = px + size * 0.35f, base = py - size * 0.2f - (MARK_TEXT.descent() + MARK_TEXT.ascent()) / 2;
            MARK_TEXT.setTextSize(ts);
            MARK_TEXT.setStyle(Paint.Style.STROKE);
            MARK_TEXT.setStrokeWidth(ts * 0.14f);
            MARK_TEXT.setColor(0xFF000000);
            c.drawText("?", x, base, MARK_TEXT);
            MARK_TEXT.setStyle(Paint.Style.FILL);
            MARK_TEXT.setColor(0xFFFFFFFF);
            c.drawText("?", x, base, MARK_TEXT);
        }
    }

    /** the sea world's spot of a place on the sea (a sunken treasure: the file's salvage point), null if none */
    static float[] seaSpot(ProgressPlaces.Place p, int point) {
        if (p.stage != null || p.kind == ProgressPlaces.SQUARE) return null;
        int chart = p.sunkenChart();
        if (chart > 0) return ProgressPlaces.sunkenSpot(chart, point);
        return Float.isNaN(p.x) ? null : new float[] {p.x, p.z};
    }

    /** whether a place in state s shows with marks MiniMap.MARKS_MISSING / MARKS_ALL */
    static boolean marked(int marks, int s) {
        return marks == MiniMap.MARKS_ALL || marks == MiniMap.MARKS_MISSING && s != ProgressPlaces.FOUND;
    }

    /** a count in a square's top right corner on a dark (all found: green) plate */
    private void badge(Canvas c, float x0, float y0, float side, String t, boolean all) {
        text.setTextSize(side * 0.22f);
        float w = Math.max(side * 0.3f, text.measureText(t) + side * 0.12f), h = side * 0.28f;
        r.set(x0 + side - w - side * 0.04f, y0 + side * 0.04f, x0 + side - side * 0.04f, y0 + side * 0.04f + h);
        fill.setStyle(Paint.Style.FILL);
        fill.setColor(all ? GameUi.GREEN.mid : 0xCC2A1C08);
        c.drawRoundRect(r, h / 2, h / 2, fill);
        text.setStyle(Paint.Style.FILL);
        text.setColor(0xFFFFFFFF);
        c.drawText(t, r.centerX(), r.centerY() - (text.descent() + text.ascent()) / 2, text);
    }

    private void outlined(Canvas c, String t, float x, float y, float size, int color) {
        text.setTextSize(size);
        float base = y - (text.descent() + text.ascent()) / 2;
        text.setStyle(Paint.Style.STROKE);
        text.setStrokeWidth(size * 0.14f);
        text.setColor(0xFF000000);
        c.drawText(t, x, base, text);
        text.setStyle(Paint.Style.FILL);
        text.setColor(color);
        c.drawText(t, x, base, text);
    }

    // ---- a dungeon's floor

    /** the FLOR entry a height is on (-1: the stage has none) */
    private int floorIndex(float y) {
        if (info == null || info.length < 5) return -1;
        int f = 0;
        for (int i = 1; 3 + 2 * i < info.length; i++)
            if (y >= info[3 + 2 * i]) f = i;
        return f;
    }

    private void drawDungeon(Canvas c) {
        drawPaper(c, false);
        if (info == null || info[2] <= 0) return;
        float u = MiniMapDungeons.TEXEL * info[2], ox = info[0], oz = info[1];
        // the pictures of all floors fit the view (the same scale on every floor)
        float x0 = Float.MAX_VALUE, z0 = Float.MAX_VALUE, x1 = -Float.MAX_VALUE, z1 = -Float.MAX_VALUE;
        for (MiniMapDungeons.Pic p : dungeon.pics) {
            float wx = ox + (p.x - p.w / 2) * u, wz = oz - (p.y + p.h / 2) * u;
            x0 = Math.min(x0, wx);
            z0 = Math.min(z0, wz);
            x1 = Math.max(x1, wx + p.w * u);
            z1 = Math.max(z1, wz + p.h * u);
        }
        float pad = getWidth() * 0.06f, k = Math.min((getWidth() - 2 * pad) / (x1 - x0), (getHeight() - 2 * pad) / (z1 - z0));
        float bx = (getWidth() - (x1 - x0) * k) / 2 - x0 * k, bz = (getHeight() - (z1 - z0) * k) / 2 - z0 * k;
        for (MiniMapDungeons.Pic p : dungeon.pics) {
            if (p.floor != floor) continue;
            Bitmap b = tinted(p.texture, MiniMap.ROOM_BLACK, MiniMap.ROOM_WHITE, MiniMap.ROOM_ALPHA);
            if (b == null) continue;
            float wx = ox + (p.x - p.w / 2) * u, wz = oz - (p.y + p.h / 2) * u;
            r.set(bx + wx * k, bz + wz * k, bx + (wx + p.w * u) * k, bz + (wz + p.h * u) * k);
            bmp.setAlpha(255);
            c.drawBitmap(b, null, r, bmp);
        }
        if (states == null) return;
        // the places inside on this floor (a stage turned against the pictures' stage: its spots turned back)
        double t = Math.toRadians(dungeon.turn);
        float cos = (float) Math.cos(t), sin = (float) Math.sin(t);
        List<ProgressPlaces.Place> in = new java.util.ArrayList<>();
        for (ProgressPlaces.Place p : places) if (dungeonStage.equals(p.stage) && p.inGroup(group)) in.add(p);
        for (ProgressPlaces.Place p : in) {
            if (Float.isNaN(p.x) || !shown(states[p.index])) continue;
            int fi = floorIndex(p.y + 50);
            int f = fi >= 0 ? (int) info[4 + 2 * fi] : 0;
            if (f != floor) continue;
            float lx = cos * p.x + sin * p.z, lz = -sin * p.x + cos * p.z;
            mark(c, p, bx + lx * k, bz + lz * k, getWidth() * 0.075f);
        }
    }
}
