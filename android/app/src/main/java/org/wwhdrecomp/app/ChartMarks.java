package org.wwhdrecomp.app;

import android.content.Context;
import android.graphics.Canvas;
import android.graphics.RectF;
import android.os.SystemClock;
import android.view.View;

/**
 * The game progress's places on the game's own sea chart (Game > Game progress > Map > On the game's map, pref
 * chart_marks): MainActivity gives the chart's area (the game's B_Area_00 touch box: 7 x 7 squares of 100,000 units)
 * and a square's view while one is open (its box, the square drawn over it), on the game's 16:9 GamePad screen (0..1);
 * the marks as on the progress map (ProgressMapView.drawMark) at their spots, those of the open square larger in its
 * view. Drawn over the GamePad picture wherever it shows: the main window's (ControlsView) or a second display's
 * (DrcDisplay). UI thread.
 */
final class ChartMarks {
    static final ChartMarks I = new ChartMarks();

    private int marks;
    private boolean on;
    private final RectF area = new RectF(), view = new RectF(), box = new RectF(), hole = new RectF();
    private int square;  // the square (1..49) whose view is open, 0 none
    private int[] states;
    private int point;
    private long statesAt;
    private final java.util.List<View> views = new java.util.ArrayList<>();  // the views drawing them

    private ChartMarks() {}

    void attach(View v) { if (!views.contains(v)) views.add(v); }

    void detach(View v) { views.remove(v); }

    private void invalidate() { for (View v : views) v.invalidate(); }

    void setMarks(int m) {
        marks = m;
        states = null;
        statesAt = 0;
        invalidate();
    }

    int marks() { return marks; }

    // + Menu map (pref chart_pictures): the game's chart drawn anew in a Map pictures style (MiniMap.PICTURES_*) over the
    // game's own, its squares where the game has them (the game's touches stay as they are); -1: the game's own
    static final int GAME_CHART = -1;
    private int pictures = GAME_CHART;
    private boolean names = true;
    private float[] state;
    private long stateAt;

    void setPictures(int p, boolean names) {
        pictures = p;
        this.names = names;
        invalidate();
    }

    /** anything to draw on the game's chart (the boxes are followed for it) */
    boolean active() { return marks != MiniMap.MARKS_OFF || pictures != GAME_CHART; }

    /** a full map button on the game's chart (FullMap; its box on the screen, empty without) */
    final RectF fullMapButton = new RectF();

    /** the chart's area (x, y centre, w, h) and the open square view's (square 0: none) */
    void set(boolean on, float x, float y, float w, float h, int square, float vx, float vy, float vw, float vh) {
        boolean same = on == this.on && this.square == square && area.left == x - w / 2 && area.top == y - h / 2
                && area.right == x + w / 2 && view.left == vx - vw / 2 && view.top == vy - vh / 2 && view.right == vx + vw / 2;
        this.on = on;
        this.square = square;
        area.set(x - w / 2, y - h / 2, x + w / 2, y + h / 2);
        view.set(vx - vw / 2, vy - vh / 2, vx + vw / 2, vy + vh / 2);
        if (on && marks != MiniMap.MARKS_OFF) refreshStates();
        if (!same) invalidate();
    }

    // ---- the game's dungeon map (DungeonMap_00 with the dungeon's DungeonMapFloor_0x): the floor map's box and the floors
    // drawn there ({floor (0 = 1F, -1 = B1), centre x, y, global scale x, y} each; their room pictures' space, y up,
    // 1280 x 720 layout units), the stage's DMAP (as MiniMapDungeons places the pictures) and FLOR heights
    private final RectF dungeonBox = new RectF();

    /** a figure the game draws on its dungeon map (aspect.cpp note_drc_pane): its picture pane's name, its middle and
     *  size in the GamePad picture (0..1), its turn (degrees, counter-clockwise) and alpha */
    static final class Fig {
        final String name;
        final float x, y, w, h, turn;
        final int alpha;
        Fig(String name, float x, float y, float w, float h, float turn, int alpha) {
            this.name = name; this.x = x; this.y = y; this.w = w; this.h = h; this.turn = turn; this.alpha = alpha;
        }
    }
    private java.util.List<Fig> figs = java.util.Collections.emptyList();

    void setFigures(java.util.List<Fig> f) {
        if (f.isEmpty() && figs.isEmpty()) return;
        figs = new java.util.ArrayList<>(f);
        if (floors != null && pictures != GAME_CHART) invalidate();
    }
    private float[] floors;
    private String dungeonStage;            // the stage the data below is for
    private float[] dungeonInfo;            // Native.miniMapStageInfo: DMAP of the pictures' stage, then FLOR pairs
    private MiniMapDungeons.Dungeon dungeon;
    private boolean dungeonLoading;

    void setDungeon(float x, float y, float w, float h, float[] fl) {
        boolean was = floors != null;
        if (fl == null) {
            floors = null;
            if (was) invalidate();
            return;
        }
        boolean same = java.util.Arrays.equals(fl, floors) && dungeonBox.left == x - w / 2 && dungeonBox.top == y - h / 2;
        floors = fl;
        dungeonBox.set(x - w / 2, y - h / 2, x + w / 2, y + h / 2);
        if (marks != MiniMap.MARKS_OFF) {
            refreshStates();
            String st = Native.miniMapStage();
            if (st != null && !st.isEmpty() && !st.equals(dungeonStage) && !dungeonLoading) loadDungeon(st);
        }
        if (!same) invalidate();
    }

    private void loadDungeon(String st) {
        String dir = MainActivity.instance != null ? MainActivity.instance.gameDir() : null;
        MiniMapDungeons.Dungeon d = MiniMapDungeons.of(st);
        if (dir == null || d == null) {
            dungeonStage = st;
            dungeon = null;
            dungeonInfo = null;
            return;
        }
        dungeonLoading = true;
        new Thread(() -> {
            float[] i = Native.miniMapStageInfo(dir, st);
            if (i != null && !d.mapStage.equals(st)) {
                float[] o = Native.miniMapStageInfo(dir, d.mapStage);
                if (o != null) System.arraycopy(o, 0, i, 0, 3);
            }
            new android.os.Handler(android.os.Looper.getMainLooper()).post(() -> {
                dungeonLoading = false;
                dungeonStage = st;
                dungeon = d;
                dungeonInfo = i;
                invalidate();
            });
        }, "chart-marks").start();
    }

    private void refreshStates() {
        if (SystemClock.uptimeMillis() - statesAt <= 2000) return;
        statesAt = SystemClock.uptimeMillis();
        byte[] d = Native.progressData();
        int[] st = d != null && d.length > 0x79C ? ProgressPlaces.states(d) : null;
        if (st != null) point = ProgressPlaces.salvagePoint(d);
        if (!java.util.Arrays.equals(st, states)) {
            states = st;
            invalidate();
        }
    }

    /** the FLOR entry a height is on (-1: the stage has none) */
    private int floorIndex(float y) {
        float[] info = dungeonInfo;
        if (info == null || info.length < 5) return -1;
        int f = 0;
        for (int i = 1; 3 + 2 * i < info.length; i++)
            if (y >= info[3 + 2 * i]) f = i;
        return f;
    }

    /** the places inside the dungeon at their spots on the floors drawn, clipped to the floor map's box */
    private void drawDungeon(Canvas c, RectF pic, Context ctx, View v) {
        float[] fl = floors, info = dungeonInfo;
        if (fl == null || info == null || info[2] <= 0 || dungeon == null || states == null) return;
        float u = MiniMapDungeons.TEXEL * info[2], ox = info[0], oz = info[1];
        double t = Math.toRadians(dungeon.turn);
        float cos = (float) Math.cos(t), sin = (float) Math.sin(t);
        toScreen(pic, dungeonBox, box);
        float size = box.width() / 14f;
        c.save();
        c.clipRect(box);
        for (ProgressPlaces.Place p : ProgressPlaces.inStage(dungeonStage, ProgressPlaces.ALL_KINDS)) {
            int s = states[p.index];
            if (Float.isNaN(p.x) || !ProgressMapView.marked(marks, s)) continue;
            int fi = floorIndex(p.y + 50);
            int f = fi >= 0 ? (int) info[4 + 2 * fi] : 0;
            // the picture's own space: the stage's world turned to the pictures' stage, then by its DMAP
            float lx = cos * p.x + sin * p.z, lz = -sin * p.x + cos * p.z;
            float qx = (lx - ox) / u, qy = (oz - lz) / u;
            for (int k = 0; k + 4 < fl.length; k += 5) {
                if ((int) fl[k] != f) continue;
                float nx = fl[k + 1] + fl[k + 3] * qx / 1280f, ny = fl[k + 2] - fl[k + 4] * qy / 720f;
                float px = pic.left + ControlsView.pictureU(nx) * pic.width(), py = pic.top + ControlsView.pictureV(ny) * pic.height();
                if (!box.contains(px, py)) continue;
                ProgressMapView.drawMark(c, ctx, p, s, px, py, size, () -> v.post(v::invalidate));
            }
        }
        c.restore();
    }

    /** over a GamePad picture drawn in pic (the game's 16:9 screen in it: ControlsView.pictureU / V) */
    void draw(Canvas c, RectF pic, Context ctx, View v) {
        if (on && pictures != GAME_CHART) drawChart(c, pic, v);
        if (floors != null && pictures != GAME_CHART) drawDungeonChart(c, pic, v);
        drawFullMapButton(c, pic, ctx);
        if (marks == MiniMap.MARKS_OFF) return;
        if (floors != null) drawDungeon(c, pic, ctx, v);
        if (!on || states == null) return;
        // the whole chart's marks (not under an open square's view), then the view's own, larger
        hole.setEmpty();
        if (square > 0) {
            toScreen(pic, view, hole);
            draw(c, pic, ctx, v, area, 0, hole);
        }
        draw(c, pic, ctx, v, square > 0 ? view : area, square, null);
    }

    /** the chart anew over the game's (MiniMap.drawSeaChart): its 7 x 7 squares in the area, an open square's view in
     *  its box */
    private void drawChart(Canvas c, RectF pic, View v) {
        MainActivity act = MainActivity.instance;
        if (act == null || act.controlsView() == null) return;
        MiniMap mm = act.controlsView().miniMap;
        mm.attach(v);
        long t = SystemClock.uptimeMillis();
        if (state == null || t - stateAt > 250) {
            state = Native.miniMapState();
            stateAt = t;
            v.postInvalidateDelayed(260);  // (Link and the boat follow while it shows)
        }
        toScreen(pic, area, box);
        c.save();
        c.clipRect(box);
        mm.drawSeaChart(c, state, pictures, names, box, 0, 0, box.width() / 700000f, box.height() / 3f, 0xFF);
        c.restore();
        if (square > 0) {
            float x = ((square - 1) % 7 - 3) * 100000f, z = ((square - 1) / 7 - 3) * 100000f;
            toScreen(pic, view, box);
            c.save();
            c.clipRect(box);
            mm.drawSeaChart(c, state, pictures, names, box, x, z, box.width() / 100000f, box.width(), 0xFF);
            c.restore();
        }
    }

    /** the dungeon map anew over the game's (MiniMap.drawDungeonFloor): each floor the game draws in the floor map's box,
     *  where it places its pictures */
    private void drawDungeonChart(Canvas c, RectF pic, View v) {
        MainActivity act = MainActivity.instance;
        float[] fl = floors;
        if (act == null || act.controlsView() == null || fl == null) return;
        MiniMap mm = act.controlsView().miniMap;
        mm.attach(v);
        long t = SystemClock.uptimeMillis();
        if (state == null || t - stateAt > 250) {
            state = Native.miniMapState();
            stateAt = t;
            v.postInvalidateDelayed(260);
        }
        toScreen(pic, dungeonBox, box);
        if (!mm.dungeonPageReady(state)) return;
        // the paper, the Forsaken Fortress's outside under the rooms, the floors, the other figures over them
        mm.drawIndoorPaper(c, box);
        drawFigures(c, pic, mm, v, true);
        // (one picture texel: its floor's scale over the layout's 1280 units, as the picture shows the layout)
        float du = ControlsView.pictureU(1) - ControlsView.pictureU(0);
        for (int k = 0; k + 4 < fl.length; k += 5) {
            float sx = pic.left + ControlsView.pictureU(fl[k + 1]) * pic.width(), sy = pic.top + ControlsView.pictureV(fl[k + 2]) * pic.height();
            float ppt = fl[k + 3] / 1280f * du * pic.width();
            mm.drawDungeonFloor(c, state, pictures, (int) fl[k], sx, sy, ppt, box, false);
        }
        drawFigures(c, pic, mm, v, false);
    }

    private final android.graphics.Paint figPaint = new android.graphics.Paint(android.graphics.Paint.FILTER_BITMAP_FLAG);
    private final RectF figRect = new RectF();

    /** the game's figures on its dungeon map again, where and as it draws them (under: only the outside picture, in the
     *  Map pictures style as a room; else all the others, in their layouts' colours) */
    private void drawFigures(Canvas c, RectF pic, MiniMap mm, View v, boolean under) {
        for (Fig f : figs) {
            boolean outdoor = f.name.equals("P_Outdoor_00");
            if (outdoor != under) continue;
            android.graphics.Bitmap b = outdoor ? mm.roomPicture("MapMajyuOD_00^t", pictures) : mm.figure(f.name);
            if (b == null) continue;
            float x = pic.left + ControlsView.pictureU(f.x) * pic.width(), y = pic.top + ControlsView.pictureV(f.y) * pic.height();
            float w = (ControlsView.pictureU(f.x + f.w / 2) - ControlsView.pictureU(f.x - f.w / 2)) * pic.width();
            float h = (ControlsView.pictureV(f.y + f.h / 2) - ControlsView.pictureV(f.y - f.h / 2)) * pic.height();
            c.save();
            c.clipRect(box);
            c.rotate(-f.turn, x, y);
            figRect.set(x - w / 2, y - h / 2, x + w / 2, y + h / 2);
            figPaint.setAlpha(f.alpha);
            c.drawBitmap(b, null, figRect, figPaint);
            c.restore();
        }
    }

    private final android.graphics.Paint button = new android.graphics.Paint(android.graphics.Paint.ANTI_ALIAS_FLAG);

    /** the full map's button at the chart's top right corner, while the whole chart shows (no square's view open) */
    private void drawFullMapButton(Canvas c, RectF pic, Context ctx) {
        fullMapButton.setEmpty();
        boolean dungeonPage = !on && floors != null;
        if (!dungeonPage && (!on || square > 0)) return;
        toScreen(pic, dungeonPage ? dungeonBox : area, box);
        float b = dungeonPage ? Math.min(box.width(), box.height()) * 0.09f : box.width() / 7f * 0.62f, m = b * 0.18f;
        fullMapButton.set(box.right - m - b, box.top + m, box.right - m, box.top + m + b);
        float r = b / 2, x = fullMapButton.centerX(), y = fullMapButton.centerY();
        button.setStyle(android.graphics.Paint.Style.FILL);
        button.setColor(0x55000000);
        c.drawCircle(x + r * 0.06f, y + r * 0.1f, r, button);
        button.setColor(0xF0FFF4D8);
        c.drawCircle(x, y, r, button);
        button.setStyle(android.graphics.Paint.Style.STROKE);
        button.setColor(MiniMap.EDGE);
        button.setStrokeWidth(r * 0.13f);
        c.drawCircle(x, y, r * 0.93f, button);
        // four arrows out to the corners: "larger"
        button.setStrokeCap(android.graphics.Paint.Cap.ROUND);
        button.setStrokeWidth(r * 0.14f);
        float i = r * 0.18f, o = r * 0.52f, h = r * 0.2f;
        for (int sx = -1; sx <= 1; sx += 2)
            for (int sy = -1; sy <= 1; sy += 2) {
                float ex = x + sx * o, ey = y + sy * o;
                c.drawLine(x + sx * i, y + sy * i, ex, ey, button);
                c.drawLine(ex, ey, ex - sx * h, ey, button);
                c.drawLine(ex, ey, ex, ey - sy * h, button);
            }
        button.setStrokeCap(android.graphics.Paint.Cap.BUTT);
    }

    private static void toScreen(RectF pic, RectF b, RectF out) {
        out.set(pic.left + ControlsView.pictureU(b.left) * pic.width(), pic.top + ControlsView.pictureV(b.top) * pic.height(),
                pic.left + ControlsView.pictureU(b.right) * pic.width(), pic.top + ControlsView.pictureV(b.bottom) * pic.height());
    }

    /** the marks of the whole chart (square 0) or of one square in its box, none inside hole */
    private void draw(Canvas c, RectF pic, Context ctx, View v, RectF in, int sq, RectF hole) {
        toScreen(pic, in, box);
        float l = box.left, t = box.top, r = box.right, b = box.bottom;
        float span = sq > 0 ? 100000f : 700000f, x0, z0;
        if (sq > 0) {
            x0 = ((sq - 1) % 7 - 3.5f) * 100000f;
            z0 = ((sq - 1) / 7 - 3.5f) * 100000f;
        } else {
            x0 = z0 = -350000f;
        }
        float size = (r - l) / (sq > 0 ? 7f : 26f);
        c.save();
        c.clipRect(l, t, r, b);
        for (ProgressPlaces.Place p : ProgressPlaces.ALL) {
            int s = states[p.index];
            if (!ProgressMapView.marked(marks, s)) continue;
            float[] spot = ProgressMapView.seaSpot(p, point);
            if (spot == null) continue;
            float px = l + (spot[0] - x0) / span * (r - l), py = t + (spot[1] - z0) / span * (b - t);
            if (px < l || px > r || py < t || py > b) continue;
            if (hole != null && hole.contains(px, py)) continue;
            ProgressMapView.drawMark(c, ctx, p, s, px, py, size, () -> v.post(v::invalidate));
        }
        c.restore();
    }
}
