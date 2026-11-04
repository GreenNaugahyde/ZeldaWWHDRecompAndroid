package org.wwhdrecomp.app;

import android.graphics.Bitmap;
import android.graphics.BitmapShader;
import android.graphics.Canvas;
import android.graphics.DashPathEffect;
import android.graphics.Matrix;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;
import android.graphics.Shader;
import android.os.SystemClock;

import java.util.HashMap;
import java.util.Map;

/**
 * The mini map on the TV picture (Controls > Screen (HUD) > Mini map), in a corner as on the GameCube, drawn
 * with the GamePad map's own artwork from the player's game files (GameIcons; BtnMapBalloon_00 and LinkPos_00
 * of the 2D pack) in that layout's colours: its paper (MapArea_00, black #E0C15F / white #FFFFE7), a chart
 * square's island (r??_b, #281E05 / #B88431), Link's face with his red direction arrow, the King of Red Lions.
 * North up (or turned with Link: his way always up, a north mark on the frame; Controls > Screen (HUD) > Map
 * facing), centred on Link. On the Great Sea one chart square (100,000 units) across; a square the fishman hasn't
 * filled in shows no island but a question mark, as on the GamePad. In the dungeons the GamePad's own room pictures
 * of the floor Link is on (MiniMapDungeons; DungeonMapFloor_0x, its P_Room materials: visited #FFDD55 / #A04B00,
 * seen only on the map #323232 / #000000), a room shown once visited or with the dungeon's map; everywhere else
 * (houses, caves, boss rooms) a floor plan of the stage's collision around Link's height, in the same colours
 * (Native.miniMapPlan, map_plan.cpp). Map pictures reach indoors too: Redrawn draws the rooms' pictures and the plans
 * anew (redraw), Terrain shows the floors coloured by their ground (in a dungeon its rooms visited, those only on
 * its map grey, instead of the pictures). Marks as on the GamePad (DungeonMapTreasure_00 / Door_00 / Up_00): chests (in a
 * dungeon only with its compass; opened ones faint), doors (once a room beside them shows), stairs (the dungeon layouts'
 * own); only those on Link's floor. Hidden in events, while Link aims or looks (first person, telescope, Picto
 * Box), in fades, on the GamePad picture, in the pause menu (Native.miniMapState, mods/minimap.cpp). A tap on it (or a
 * short − on a controller) makes it the next size; in the layout editor it is dragged to a place of its own (Corner: Own place) and sized by the
 * editor's smaller / bigger buttons.
 */
final class MiniMap {
    static final int CORNER_BOTTOM_LEFT = 0, CORNER_TOP_LEFT = 1, CORNER_TOP_RIGHT = 2, CORNER_BOTTOM_RIGHT = 3,
            CORNER_OWN = 4;  // placed in the layout editor (ownX / ownY)
    /** its side by size setting (small, medium, large), as a fraction of the TV picture's height */
    static final float[] SIZES = {0.24f, 0.30f, 0.38f};
    private static final float SQUARE = 100000f;  // a chart square, in world units
    private static final long FADE_MS = 200, POLL_MS = 33;
    // the layout's colours (BtnMapBalloon_00 materials P_MapBase_00, P_DummyMapBig_00; LinkPos_00 P_Direction_00)
    static final int PAPER_BLACK = 0xE0C15F, PAPER_WHITE = 0xFFFFE7, ISLAND_BLACK = 0x281E05, ISLAND_WHITE = 0xB88431,
            ARROW_BLACK = 0xFF0000, ARROW_WHITE = 0xFFFFFF;
    // indoors: DungeonMapFloor_0x's P_Room materials (visited; seen only on the dungeon's map), a brown paper
    static final int ROOM_BLACK = 0xFFDD55, ROOM_WHITE = 0xA04B00, ROOM_ALPHA = 0xB4, DIM_BLACK = 0x323232,
            DIM_WHITE = 0x000000, DIM_ALPHA = 0x64, INDOOR_PAPER_BLACK = 0x8E7B55, INDOOR_PAPER_WHITE = 0xD2C29A;
    private static final float DUNGEON_SPAN = 280f;  // a dungeon map's texels across the mini map
    private static final float PLAN_BELOW = 700f, PLAN_ABOVE = 350f, PLAN_MOVE = 300f;  // the plan's heights around Link
    static final int EDGE = 0xFF5A3C14, GRID = 0x9970440E, UNKNOWN = 0x6670440E;
    // the round style: CompassClock_00's P_CompassBase_00 (bezel, black #381302 / white #D78C4B, 92 texels; its dark
    // rim from texel radius 38 to 45, a bronze disk inside), P_CompassBaseSh_00 (its shadow: black at 122, 4 right /
    // 4 down), the face's letters' cream (P_CompassBase_01 white #ECDDC3), the needle's red (P_Compass_00 #EB0000), the
    // face's gleam (CompassSpec_01, white)
    private static final int BEZEL_BLACK = 0x381302, BEZEL_WHITE = 0xD78C4B, FACE_CREAM = 0xFFECDDC3, NEEDLE_RED = 0xFFEB0000;
    // see-through: the lines (the paper's light colour; rooms seen only on the dungeon's map grey) with a dark edge
    // outside them so they read over any picture; the grid and question marks in the line colour
    private static final int LINE = 0xFFFFFFE7, LINE_DIM = 0xB4A0A0A0, LINE_EDGE = 0x8C000000, LINE_GRID = 0x99FFFFE7,
            LINE_UNKNOWN = 0x99FFFFE7;
    private static final float BEZEL_INNER = 37.5f / 46f, BEZEL_RIM = 41.5f / 46f;  // of the bezel's radius: the map's edge, the letters

    private final ControlsView view;
    private final Map<String, Bitmap> tinted = new HashMap<>();
    private boolean enabled;
    private int corner = CORNER_BOTTOM_LEFT, size = 1;
    private float ownX, ownY = 1;  // its own place: the top left corner as fractions of the room the picture leaves it
    private boolean turn;          // the map turns with Link (his way up) instead of north up
    private boolean roundStyle;    // round in the sailing compass's bezel, N / E / S / W on its rim (Map style)
    private boolean outlines;      // see-through: no paper, the islands / rooms / plan as lines only (Map inside)
    // Map pictures: the sea's islands drawn anew (their shapes smooth, the script on them left out), the GamePad pictures
    // enlarged with crisp edges, or as they are
    // Terrain: the islands as they are, from above, coloured by their ground (their collision; true size, so the sea map
    // zooms in, Sea zoom)
    static final int PICTURES_REDRAWN = 0, PICTURES_HD = 1, PICTURES_ORIGINAL = 2, PICTURES_TERRAIN = 3;
    private boolean hd = true;     // islands and dungeon rooms enlarged with crisp edges (HD; Redrawn for the rooms)
    private boolean redraw = true; // the sea's islands redrawn (Redrawn)
    private boolean names = true;  // on the sea: the islands' names under them (Island names)
    private boolean face = true;   // Link's face on his place (Link's face); off: only his arrow, on the place itself
    private boolean terrain;       // the sea's islands from their ground, true size, on blue water (Terrain)
    private float seaSpan = SQUARE; // world units across the map on the sea (Sea zoom)
    // the islands' terrain pictures by sea room (built on HD_WORK) and their world boxes, the last few kept
    private final Map<Integer, Bitmap> terrains = new java.util.LinkedHashMap<Integer, Bitmap>(16, 0.75f, true) {
        @Override protected boolean removeEldestEntry(Map.Entry<Integer, Bitmap> e) { return size() > 8; }
    };
    private final RectF[] terrainRect = new RectF[50];
    // small copies of the terrain and redrawn pictures (SMALL texels at most), all kept: the full map's whole sea draws
    // them all at once
    private static final int SMALL = 256;
    private final Map<Integer, Bitmap> terrainSmall = new HashMap<>(), redrawnSmall = new HashMap<>();
    private final java.util.Set<Integer> terrainPending = new java.util.HashSet<>();
    private static final int SEA_BLUE = 0xFF3F72BF;
    // their enlarged textures (sharpened, built on HD_WORK; the original is drawn until then), the last few kept
    private final Map<String, Bitmap> hdSrc = new java.util.LinkedHashMap<String, Bitmap>(16, 0.75f, true) {
        @Override protected boolean removeEldestEntry(Map.Entry<String, Bitmap> e) { return size() > 12; }
    };
    private final java.util.Set<String> hdPending = new java.util.HashSet<>();
    // the redrawn islands (built on HD_WORK; the HD picture is drawn until then), keyed by sea room * 2 + 1 for lines
    private final Map<Integer, Bitmap> redrawn = new java.util.LinkedHashMap<Integer, Bitmap>(16, 0.75f, true) {
        @Override protected boolean removeEldestEntry(Map.Entry<Integer, Bitmap> e) { return size() > 12; }
    };
    private final java.util.Set<Integer> redrawPending = new java.util.HashSet<>();
    // per sea room: the middle of its picture's land across and its lowest land down, as fractions of the square (names)
    private final float[][] islandSpot = new float[50][];
    private boolean wind = true;       // the wind's way (any stage), an arrow on a dial in a corner of the map (Wind arrow)
    private boolean fadeMoving;        // fainter (opacityMin) while Link or the boat moves (Fade while moving)
    private float opacityMax = 1, opacityMin = 0.7f, shownOpacity = 1;
    private long opacityAt;
    private float lastX, lastZ;        // Link's place at the last poll, to tell he moves
    private long lastAt, movedAt;
    private boolean movingShown;
    // Zoom while moving: the map zoomed in by moveZoom while Link or the boat moves (eased), back out when he stands
    static final float[] MOVE_ZOOMS = {1, 1.5f, 2, 3};
    private float moveZoom = 1, shownZoom = 1;
    private long zoomAt;
    // Link's mark: its size (Mark size) and the arrow's colour (Arrow colour; the texture's dark ink takes it, the light
    // edge stays white, for white a dark edge)
    static final float[] MARK_SIZES = {0.7f, 1, 1.3f, 1.6f};
    static final int[] ARROW_COLOURS = {0xFF0000, 0xFF8C00, 0xFFD700, 0x22C55E, 0x00C8FF, 0x2F6BFF, 0xFF4FB0, 0xFFFFFF};
    private float markSize = 1;
    private int arrowColour = ARROW_BLACK;
    private static final float MOVE_PER_MS = 0.15f;  // world units per ms that count as moving (a walk is ~0.5)
    private static final long MOVE_HOLD_MS = 400;
    private boolean charts = true;     // beside the map: the treasure chart Link has for the square he is in (Treasure charts)
    private byte[] chartList;          // Native.miniMapCharts (cmapdat.bin), read once
    private boolean chartListRead;
    private byte[] saveData;           // Native.progressData, read every MARKS_MS while charts or marks are on
    private boolean buttons;           // a hide button by the map, a map button while hidden (Map buttons)
    private boolean hidden;            // hidden by its button (only with the buttons on)
    private final RectF btnHide = new RectF(), btnShow = new RectF();
    static final int TAP_NONE = 0, TAP_SIZE = 1, TAP_HIDDEN = 2;
    private final RectF windHit = new RectF();  // the wind dial as drawn last (a swipe from it sets the wind)
    private int windPreview = Integer.MIN_VALUE;  // a swipe from the dial under way: the wind it sets (game angle y)
    private static final java.util.concurrent.ExecutorService HD_WORK = java.util.concurrent.Executors.newSingleThreadExecutor();
    private float shownTurn;       // degrees the map is turned now (eased towards Link's way)
    private float innerTurn;       // while a turned dungeon's pictures are drawn: their turn
    private long turnAt;
    private float[] st = new float[72];
    private float shownSide;       // the map's side in pixels, as last drawn
    private float alpha;           // 0..1, fading in while it should show, out while not
    private long alphaAt;
    private final Paint bmp = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG), line = new Paint(Paint.ANTI_ALIAS_FLAG),
            text = new Paint(Paint.ANTI_ALIAS_FLAG), paper = new Paint(Paint.ANTI_ALIAS_FLAG | Paint.FILTER_BITMAP_FLAG);
    private final Matrix m = new Matrix();
    private final RectF r = new RectF(), sq = new RectF(), ext = new RectF();
    private final Path clip = new Path();

    MiniMap(ControlsView v) {
        view = v;
        text.setTextAlign(Paint.Align.CENTER);
        text.setFakeBoldText(true);
    }

    // other views drawing its pictures (the full map, the game's chart on a second display): redrawn with it once a
    // picture is ready
    private final java.util.List<android.view.View> others = new java.util.ArrayList<>();

    void attach(android.view.View v) { if (!others.contains(v)) others.add(v); }

    void detach(android.view.View v) { others.remove(v); }

    private void repaint() {
        view.postInvalidate();
        for (android.view.View v : others) v.postInvalidate();
    }

    // the game progress's places on the map (ProgressPlaces; Mini map > Progress marks): 0 none, 1 what is missing, 2 all
    static final int MARKS_OFF = 0, MARKS_MISSING = 1, MARKS_ALL = 2;
    private static final long MARKS_MS = 2000;
    private int progressMarks;
    private int[] placeStates;   // ProgressPlaces.states of the save data, read every MARKS_MS
    private int salvagePoint;
    private long statesAt;
    private static final android.graphics.ColorMatrixColorFilter GREY;
    static {
        android.graphics.ColorMatrix cm = new android.graphics.ColorMatrix();
        cm.setSaturation(0);
        GREY = new android.graphics.ColorMatrixColorFilter(cm);
    }

    void setProgressMarks(int marks) {
        progressMarks = marks;
        placeStates = null;
        statesAt = 0;
        repaint();
    }

    private final Runnable poll = new Runnable() {
        @Override
        public void run() {
            if (!enabled) return;
            float[] s = Native.miniMapState();
            long t = SystemClock.uptimeMillis();
            if ((progressMarks != MARKS_OFF || charts) && s[0] != 0 && t - statesAt >= MARKS_MS) {
                statesAt = t;
                byte[] d = Native.progressData();
                boolean ok = d != null && d.length > 0x79C;
                if (charts && ok && !java.util.Arrays.equals(d, saveData)) {
                    saveData = d;
                    repaint();
                }
                int[] ps = ok && progressMarks != MARKS_OFF ? ProgressPlaces.states(d) : null;
                if (ps != null) salvagePoint = ProgressPlaces.salvagePoint(d);
                if (!java.util.Arrays.equals(ps, placeStates)) {
                    placeStates = ps;
                    repaint();
                }
            }
            if (s[0] != 0 && lastAt != 0 && s[0] == st[0] && Math.hypot(s[1] - lastX, s[2] - lastZ) > MOVE_PER_MS * (t - lastAt))
                movedAt = t;
            lastX = s[1];
            lastZ = s[2];
            lastAt = t;
            boolean moving = t - movedAt < MOVE_HOLD_MS;
            if ((fadeMoving || moveZoom > 1) && moving != movingShown) repaint();
            movingShown = moving;
            if (s[0] == 2) indoors(s);
            if (!java.util.Arrays.equals(s, st)) {
                st = s;
                repaint();
            }
            view.postDelayed(this, POLL_MS);
        }
    };

    void setEnabled(boolean on) {
        if (on == enabled) return;
        enabled = on;
        view.removeCallbacks(poll);
        if (on) view.post(poll);
        else {
            st = new float[72];
            alpha = 0;
        }
        repaint();
    }

    void setPlace(int corner, int size, boolean turn, float ownX, float ownY, boolean round, boolean outlines) {
        this.turn = turn;
        this.roundStyle = round;
        this.outlines = outlines;
        this.corner = corner;
        this.size = Math.max(0, Math.min(SIZES.length - 1, size));
        this.ownX = Math.max(0, Math.min(1, ownX));
        this.ownY = Math.max(0, Math.min(1, ownY));
        repaint();
    }

    /** Map pictures (pref minimap_pictures; before it there was only minimap_hd: HD or Original) */
    static int pictures(android.content.SharedPreferences p) {
        return p.getInt("minimap_pictures", p.getBoolean("minimap_hd", true) ? PICTURES_REDRAWN : PICTURES_ORIGINAL);
    }

    /** Link's face, Mark size, Arrow colour (indexes into MARK_SIZES / ARROW_COLOURS), Zoom while moving (MOVE_ZOOMS) */
    void setMark(boolean face, int size, int colour, int zoom) {
        this.face = face;
        markSize = MARK_SIZES[Math.max(0, Math.min(MARK_SIZES.length - 1, size))];
        arrowColour = ARROW_COLOURS[Math.max(0, Math.min(ARROW_COLOURS.length - 1, colour))];
        moveZoom = MOVE_ZOOMS[Math.max(0, Math.min(MOVE_ZOOMS.length - 1, zoom))];
        repaint();
    }

    /** Sea zoom (pref minimap_sea_zoom): 0 Auto (Terrain: a quarter of a square across, else one), n: 1/n square */
    static int seaZoom(android.content.SharedPreferences p) { return p.getInt("minimap_sea_zoom", 0); }

    /** Map pictures (PICTURES_*), Island names and Sea zoom */
    void setPictures(int pictures, boolean names, int zoom) {
        hd = pictures != PICTURES_ORIGINAL;
        redraw = pictures == PICTURES_REDRAWN;
        terrain = pictures == PICTURES_TERRAIN;
        this.names = names;
        seaSpan = SQUARE / (zoom > 0 ? zoom : terrain ? 4 : 1);
        repaint();
    }

    /** the wind arrow, the opacity (percent; while moving with fadeMoving), the chart beside it, its buttons */
    void setExtras(boolean wind, int opacity, boolean fadeMoving, int movingOpacity, boolean charts, boolean buttons, boolean hidden) {
        this.wind = wind;
        this.opacityMax = Math.max(0, Math.min(100, opacity)) / 100f;
        this.fadeMoving = fadeMoving;
        this.opacityMin = Math.max(0, Math.min(100, movingOpacity)) / 100f;
        this.charts = charts;
        this.buttons = buttons;
        this.hidden = hidden;
        repaint();
    }

    boolean hidden() { return hidden; }

    /** a finger down at (x, y) is on the map or one of its buttons */
    boolean hit(float x, float y) {
        return r.contains(x, y) || btnShow.contains(x, y) || btnHide.contains(x, y);
    }

    /** a tap at (x, y): a button's (hide / show) or the map's own (the next size; also on the wind dial); what changed */
    int tap(float x, float y) {
        if (!btnShow.isEmpty() && btnShow.contains(x, y)) {
            hidden = false;
            repaint();
            return TAP_HIDDEN;
        }
        if (!btnHide.isEmpty() && btnHide.contains(x, y)) {
            hidden = true;
            repaint();
            return TAP_HIDDEN;
        }
        if (!r.isEmpty() && r.contains(x, y)) {
            nextSize();
            return TAP_SIZE;
        }
        return TAP_NONE;
    }

    private boolean windSwipe = true;  // the wind dial takes fingers: a swipe sets the wind (pref minimap_wind_swipe)

    void setWindSwipe(boolean on) { windSwipe = on; }

    /** a finger down at (x, y) is on the wind dial (and the dial takes fingers) */
    boolean onWind(float x, float y) { return windSwipe && !hidden && !windHit.isEmpty() && windHit.contains(x, y); }

    /** the wind dial is on the screen now and takes fingers (its first-time hint) */
    boolean windDialShown() { return windSwipe && !hidden && !windHit.isEmpty(); }

    /** a swipe (dx, dy on the screen) from the wind dial: the game's wind angle y (s16) the way it points on the map
     *  (its turn undone), one of the compass's 8 */
    int windAngle(float dx, float dy) { return PadLink.windAngle(dx, dy, shownTurn); }

    /** the dial's turn for the wind now: the map's while it shows (its swipes and − + right stick use it), else north up */
    float windTurn() { return onScreen() ? shownTurn : 0; }

    /** the dial shows this wind (game angle y) while a swipe is under way; Integer.MIN_VALUE: the game's own again */
    void setWindPreview(int angle) {
        if (angle == windPreview) return;
        windPreview = angle;
        repaint();
    }

    boolean enabled() { return enabled; }
    int size() { return size; }
    int corner() { return corner; }
    float ownX() { return ownX; }
    float ownY() { return ownY; }

    /** on the screen now (in play, not hidden by its button) */
    boolean onScreen() { return !r.isEmpty(); }

    /** a tap on the map or a short − on a controller: the next size (small, medium, large, small ...) */
    int nextSize() {
        size = (size + 1) % SIZES.length;
        repaint();
        return size;
    }

    /** where it is in the picture tv (its corner, or its own place) */
    void place(RectF tv, RectF out) {
        float side = SIZES[size] * tv.height(), margin = 0.045f * tv.height();
        float x0, y0;
        if (corner == CORNER_OWN) {
            x0 = tv.left + margin + ownX * Math.max(0, tv.width() - 2 * margin - side);
            y0 = tv.top + margin + ownY * Math.max(0, tv.height() - 2 * margin - side);
        } else {
            boolean left = corner == CORNER_BOTTOM_LEFT || corner == CORNER_TOP_LEFT, top = corner == CORNER_TOP_LEFT || corner == CORNER_TOP_RIGHT;
            x0 = left ? tv.left + margin : tv.right - margin - side;
            y0 = top ? tv.top + margin : tv.bottom - margin - side;
        }
        out.set(x0, y0, x0 + side, y0 + side);
    }

    /** the layout editor: it moves to its own place with its top left corner at (x, y) (kept inside the picture) */
    void moveTo(RectF tv, float x, float y) {
        float side = SIZES[size] * tv.height(), margin = 0.045f * tv.height();
        float fw = tv.width() - 2 * margin - side, fh = tv.height() - 2 * margin - side;
        ownX = fw > 0 ? Math.max(0, Math.min(1, (x - tv.left - margin) / fw)) : 0;
        ownY = fh > 0 ? Math.max(0, Math.min(1, (y - tv.top - margin) / fh)) : 0;
        corner = CORNER_OWN;
        repaint();
    }

    void resize(int by) {
        size = Math.max(0, Math.min(SIZES.length - 1, size + by));
        repaint();
    }

    /** the layout editor: where the map goes, its paper, frame and Link's face (a ring when selected) */
    void drawEditor(Canvas c, RectF tv, boolean selected) {
        if (!enabled || tv.isEmpty()) return;
        place(tv, r);
        float side = r.width(), round = side * 0.08f;
        shownSide = side;
        if (roundStyle) {
            drawBezelShadow(c, side, 1f);
            c.save();
            clipInner(c, r.centerX(), r.centerY(), side);
            Bitmap tile = paperTile(true);
            if (tile != null) {
                BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
                m.reset();
                m.setScale(side / 2 / tile.getWidth(), side / 2 / tile.getWidth());
                m.postTranslate(r.left, r.top);
                sh.setLocalMatrix(m);
                paper.setShader(sh);
                paper.setAlpha(0xD0);
                c.drawRect(r, paper);
            }
            c.restore();
            drawBezel(c, side, 0xFF);
            if (selected) {
                line.setStyle(Paint.Style.STROKE);
                line.setStrokeWidth(side * 0.022f);
                line.setColor(0xFFFFC107);
                c.drawCircle(r.centerX(), r.centerY(), side / 2, line);
            }
            drawLink(c, r.centerX(), r.centerY(), side * BEZEL_INNER, (float) Math.PI, 0xFF);
            return;
        }
        c.save();
        clip.reset();
        clip.addRoundRect(r, round, round, Path.Direction.CW);
        c.clipPath(clip);
        Bitmap tile = paperTile(true);
        if (tile != null) {
            BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
            m.reset();
            m.setScale(side / 2 / tile.getWidth(), side / 2 / tile.getWidth());
            m.postTranslate(r.left, r.top);
            sh.setLocalMatrix(m);
            paper.setShader(sh);
            paper.setAlpha(0xD0);
            c.drawRect(r, paper);
        } else {
            line.setStyle(Paint.Style.FILL);
            line.setColor(0xD0000000 | PAPER_BLACK);
            c.drawRect(r, line);
        }
        c.restore();
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(side * 0.022f);
        line.setColor(selected ? 0xFFFFC107 : EDGE);
        c.drawRoundRect(r, round, round, line);
        drawLink(c, r.centerX(), r.centerY(), side, (float) Math.PI, 0xFF);
    }

    /** the map's own square in view coordinates (empty when it doesn't show) */
    RectF bounds() { return r; }

    /** shown: in play on the sea and no event, aim or fade (st), and allowed by the view (not the GamePad picture,
     *  the pause menu, the layout editor, the title) */
    void draw(Canvas c, RectF tv, boolean allowed) {
        boolean w = enabled && allowed && !tv.isEmpty() && st[0] != 0 && st[9] == 0 && st[10] == 0 && st[11] == 0;
        long now = SystemClock.uptimeMillis();
        if (alphaAt == 0) alphaAt = now;  // (a fade starts from this frame)
        float dt = (now - alphaAt) / (float) FADE_MS;
        alphaAt = now;
        alpha = w ? Math.min(1, alpha + dt) : Math.max(0, alpha - dt);
        if (w ? alpha < 1 : alpha > 0) view.postInvalidateOnAnimation();
        btnHide.setEmpty();
        btnShow.setEmpty();
        windHit.setEmpty();
        if (alpha <= 0) {
            r.setEmpty();
            if (!w) alphaAt = turnAt = zoomAt = 0;  // (shown again: turned and zoomed at once)
            return;
        }
        // the opacity: fainter while Link moves (eased), the full one else
        float target = fadeMoving && movingShown ? opacityMin : opacityMax;
        float ot = opacityAt == 0 ? 1 : 1 - (float) Math.exp(-(now - opacityAt) / 160.0);
        opacityAt = now;
        shownOpacity = Math.abs(target - shownOpacity) < 0.01f ? target : shownOpacity + (target - shownOpacity) * ot;
        if (shownOpacity != target) view.postInvalidateOnAnimation();
        float alpha = this.alpha * shownOpacity;
        // the zoom: in while Link moves (Zoom while moving), eased more slowly than the fading
        float zt = moveZoom > 1 && movingShown ? moveZoom : 1;
        float ze = zoomAt == 0 ? 1 : 1 - (float) Math.exp(-(now - zoomAt) / 350.0);
        zoomAt = now;
        shownZoom = Math.abs(zt - shownZoom) < 0.005f ? zt : shownZoom + (zt - shownZoom) * ze;
        if (shownZoom != zt) view.postInvalidateOnAnimation();
        place(tv, r);
        if (buttons && hidden) {  // hidden by its button: only the button that brings it back
            drawShowButton(c, r, Math.round(255 * this.alpha));
            r.setEmpty();
            return;
        }
        float outer = r.width();
        // the map's own width: round, the bezel's inside (its marks, Link's face and the scale go by that)
        float side = roundStyle ? outer * BEZEL_INNER : outer;
        shownSide = side;
        int a = Math.round(255 * alpha);
        boolean sea = st[0] == 1;
        float lx = st[1], lz = st[2], span = (sea ? seaSpan : indoorSpan()) / shownZoom, k = side / span, cx = r.centerX(), cy = r.centerY();
        float round = side * 0.08f;
        // turned with Link: his way up (facing 0 = south, down when north is up), eased so a quick turn doesn't jump
        float want = turn ? (float) Math.toDegrees(st[3]) - 180f : 0f;
        float diff = ((want - shownTurn) % 360f + 540f) % 360f - 180f;
        float ease = turnAt == 0 ? 1 : 1 - (float) Math.exp(-(now - turnAt) / 90.0);
        turnAt = now;
        shownTurn = Math.abs(diff) < 0.2f ? want : shownTurn + diff * ease;
        if (shownTurn != want) view.postInvalidateOnAnimation();
        // what can show inside the square at any turn (its circumcircle's square)
        ext.set(r);
        ext.inset(-side * 0.21f, -side * 0.21f);
        // a soft shadow under the paper (round: the bezel's), then the paper (the map moves on it with Link)
        if (roundStyle) drawBezelShadow(c, outer, alpha);
        else if (!outlines) {
            line.setStyle(Paint.Style.FILL);
            line.setColor(0x55000000);
            line.setAlpha(Math.round(0x55 * alpha));
            sq.set(r);
            sq.offset(side * 0.015f, side * 0.02f);
            c.drawRoundRect(sq, round, round, line);
        }
        c.save();
        if (roundStyle) clipInner(c, cx, cy, outer);
        else {
            clip.reset();
            clip.addRoundRect(r, round, round, Path.Direction.CW);
            c.clipPath(clip);
        }
        c.rotate(shownTurn, cx, cy);
        Bitmap tile = outlines ? null : paperTile(sea);
        if (outlines) {
            // see-through: no paper
        } else if (sea && terrain) {
            line.setStyle(Paint.Style.FILL);
            line.setColor(SEA_BLUE);
            line.setAlpha(a);
            c.drawRect(ext, line);
        } else if (tile != null) {
            BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
            // half a square per tile (the GamePad stretches its 64 texels over one square; this is its middle half, mirrored)
            float ts = (sea ? SQUARE / 2 : span / 3) * k / tile.getWidth();
            m.reset();
            m.setScale(ts, ts);
            m.postTranslate(cx - lx * k, cy - lz * k);
            sh.setLocalMatrix(m);
            paper.setShader(sh);
            paper.setAlpha(a);
            c.drawRect(ext, paper);
        } else {
            line.setColor(0xFF000000 | (sea ? PAPER_BLACK : INDOOR_PAPER_BLACK));
            line.setAlpha(a);
            c.drawRect(ext, line);
        }
        if (!sea) drawIndoors(c, lx, lz, k, cx, cy, a, floorIndex(st[61]), plan);
        else drawSea(c, lx, lz, k, cx, cy, side, a);
        c.restore();
        // the frame: round, the compass's bezel with the directions (turning with the map); else a brown edge and,
        // turned, the north mark
        if (roundStyle) drawBezel(c, outer, a);
        else {
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(side * 0.022f);
            line.setColor(EDGE);
            line.setAlpha(a);
            c.drawRoundRect(r, round, round, line);
            if (shownTurn != 0) drawNorth(c, cx, cy, side, a);
        }
        // Link: his face in the middle, the arrow around it the way he faces (LinkPos_00: N_Rotate_00 turns the arrow,
        // which points up at 24 units from the face's centre; the face 54 units at 0.7)
        drawLink(c, cx, cy, side, st[3], a);
        windHit.setEmpty();
        if (wind && (st[70] != 0 || st[71] != 0)) drawWind(c, cx, cy, side, a);  // (any stage: a swipe there sets the wind)
        if (sea && charts) drawChart(c, tv, outer, a);
        if (buttons) drawButtons(c, tv, outer, Math.round(255 * this.alpha));
    }

    // ---- the wind arrow: MenuMap_00's dial (a quarter, mirrored) in the map's north-east, its arrow (the left half,
    // mirrored; the tip up) turned the way the wind blows (st[70]: 0 = south, +x east), with the map's turn
    private void drawWind(Canvas c, float cx, float cy, float side, int a) {
        float d = side * (roundStyle ? 0.24f : 0.33f), rad = side * 0.12f, hr = Math.max(rad * 1.5f, side * 0.15f);
        // (a swipe under way: its wind; the game's y blows along (cos, sin) in x, z, st[70] is atan2(x, z))
        float way = windPreview != Integer.MIN_VALUE ? PadLink.windWay(windPreview) : st[70];
        drawCompass(c, cx + d, cy - d, rad, way, shownTurn, a);
        windHit.set(cx + d - hr, cy - d - hr, cx + d + hr, cy - d + hr);
    }

    /** the wind dial at (x, y), rad its radius, the arrow where the wind blows (wind: as st[70]) with the map's turn;
     *  also the full map's compass */
    void drawCompass(Canvas c, float x, float y, float rad, float wind, float turn, int a) {
        Bitmap dial = tinted("MapWindBase_00^t", 0x3C2A10, 0xFFF4D8, 0xE6), arrow = tinted("WindArrowNow_00^t", 0x2A6EB4, 0xFFFFFF, 0xFF);
        Bitmap shadow = GameIcons.get(view.getContext(), "WindArrowNow_02^q", this::repaint);
        if (dial == null || arrow == null) return;
        bmp.setAlpha(a);
        for (int q = 0; q < 4; q++) {  // the dial's quarters: top left as drawn, the others mirrored
            c.save();
            c.scale(q % 2 == 0 ? 1 : -1, q < 2 ? 1 : -1, x, y);
            sq.set(x - rad, y - rad, x, y);
            c.drawBitmap(dial, null, sq, bmp);
            c.restore();
        }
        float deg = 180f - (float) Math.toDegrees(wind) + turn, h = rad * 1.9f, hw = h * arrow.getWidth() / arrow.getHeight();
        c.save();
        c.rotate(deg, x, y);
        for (int half = 0; half < 2; half++) {
            c.save();
            if (half == 1) c.scale(-1, 1, x, y);
            if (shadow != null) {
                bmp.setAlpha(a * 3 / 5);
                sq.set(x - hw, y - h / 2, x, y + h / 2);
                sq.offset(rad * 0.06f, rad * 0.08f);
                c.drawBitmap(shadow, null, sq, bmp);
            }
            bmp.setAlpha(a);
            sq.set(x - hw, y - h / 2, x, y + h / 2);
            c.drawBitmap(arrow, null, sq, bmp);
            c.restore();
        }
        c.restore();
    }

    // ---- the treasure chart beside the map: Link has the chart of the square he is in (dSv_player_map_c set 1; a
    // Triforce chart deciphered, set 2) and its treasure isn't salvaged (set 3). Its picture (TreMap_00, the salvage
    // point set in use) covers 10,000 units around its centre (d_menu_fmap2.cpp cmapPlayerPosDispCheck: +-5000), the X
    // at the salvage point, Link's face while he is on it; the chart's name on the title ribbon above
    private static final int CHART_LEN = 0x26;
    private static final float CHART_SPAN = 10000f;

    private void drawChart(Canvas c, RectF tv, float outer, int a) {
        byte[] d = saveData;
        if (d == null) return;
        if (!chartListRead) {
            chartListRead = true;
            java.util.List<String> dirs = new java.util.ArrayList<>();
            MainActivity act = MainActivity.instance;
            if (act != null) {
                int g = Randomizer.playingGame(act);
                if (g != 0) dirs.add(Randomizer.outDir(act, g).getAbsolutePath());  // (a randomized game's own files first)
                dirs.add(act.gameDir());
            }
            chartList = dirs.isEmpty() ? null : Native.miniMapCharts(dirs.toArray(new String[0]));
        }
        byte[] l = chartList;
        if (l == null || l.length < 4) return;
        int col = (int) Math.floor((st[1] + 3.5f * SQUARE) / SQUARE), row = (int) Math.floor((st[2] + 3.5f * SQUARE) / SQUARE);
        if (col < 0 || col > 6 || row < 0 || row > 6) return;
        int n = Math.min(((l[0] & 0xFF) << 24 | (l[1] & 0xFF) << 16 | (l[2] & 0xFF) << 8 | (l[3] & 0xFF)), (l.length - 4) / CHART_LEN);
        int set = ProgressPlaces.salvagePoint(d) & 3;
        for (int i = 0; i < n; i++) {
            int e = 4 + i * CHART_LEN, grid = l[e], bit = l[e + 1], number = l[e + 2], kind = l[e + 3], gx = l[e + 4], gy = l[e + 5];
            if (grid < 1 || grid > 49 || bit < 1 || gx + 3 != col || gy + 3 != row) continue;
            if (!ProgressPlaces.chartBit(d, 1, bit) || ProgressPlaces.chartBit(d, 3, bit)) continue;
            if (kind == 0 && !ProgressPlaces.chartBit(d, 2, bit)) continue;
            int p = e + 6 + set * 8;
            float px = s16(l, p), pz = s16(l, p + 2), sx = s16(l, p + 4), sz = s16(l, p + 6);
            drawChartPanel(c, tv, outer, a, grid, set, kind, number, px, pz, sx, sz, st[1] - (col - 3) * SQUARE, st[2] - (row - 3) * SQUARE);
            return;
        }
    }

    private static float s16(byte[] b, int o) { return (short) ((b[o] & 0xFF) << 8 | (b[o + 1] & 0xFF)); }

    private void drawChartPanel(Canvas c, RectF tv, float outer, int a, int room, int set, int kind, int number,
                                float px, float pz, float sx, float sz, float lx, float lz) {
        float p = outer * 0.62f, gap = outer * 0.05f, title = p * 0.2f;
        boolean right = r.centerX() < tv.centerX(), bottom = r.centerY() > tv.centerY();
        float x0 = right ? r.right + gap : r.left - gap - p;
        float y1 = bottom ? r.bottom : r.top + title + p, y0 = y1 - p;
        RectF pic = new RectF(x0, y0, x0 + p, y1);
        float round = p * 0.08f;
        // shadow, paper, the picture, the X, Link
        line.setStyle(Paint.Style.FILL);
        line.setColor(0x55000000);
        line.setAlpha(0x55 * a / 255);
        sq.set(pic);
        sq.offset(p * 0.02f, p * 0.03f);
        c.drawRoundRect(sq, round, round, line);
        c.save();
        clip.reset();
        clip.addRoundRect(pic, round, round, Path.Direction.CW);
        c.clipPath(clip);
        Bitmap tile = paperTile(true);
        if (tile != null) {
            BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
            m.reset();
            m.setScale(p / 2 / tile.getWidth(), p / 2 / tile.getWidth());
            m.postTranslate(pic.left, pic.top);
            sh.setLocalMatrix(m);
            paper.setShader(sh);
            paper.setAlpha(0xD0 * a / 255);
            c.drawRect(pic, paper);
        }
        Bitmap b = tinted(GameIcons.chartPicture(room, set), ISLAND_BLACK, ISLAND_WHITE, 0xFF, hd);
        if (b != null) {
            bmp.setAlpha(a);
            c.drawBitmap(b, null, pic, bmp);
        }
        c.restore();
        Bitmap x = tinted("MapTreasureMark_00^t", 0x5A0000, 0xFF2A1E, 0xFF);
        if (x != null) {
            float mx = pic.left + ((sx - px) / CHART_SPAN + 0.5f) * p, my = pic.top + ((sz - pz) / CHART_SPAN + 0.5f) * p, h = p * 0.09f;
            sq.set(mx - h, my - h, mx + h, my + h);
            bmp.setAlpha(a);
            c.drawBitmap(x, null, sq, bmp);
        }
        float ox = (lx - px) / CHART_SPAN, oz = (lz - pz) / CHART_SPAN;
        if (Math.abs(ox) <= 0.5f && Math.abs(oz) <= 0.5f) {
            float keep = shownTurn;
            shownTurn = 0;  // (the chart stays north up)
            drawLink(c, pic.left + (ox + 0.5f) * p, pic.top + (oz + 0.5f) * p, p * 0.8f, st[3], a);
            shownTurn = keep;
        }
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(p * 0.025f);
        line.setColor(EDGE);
        line.setAlpha(a);
        c.drawRoundRect(pic, round, round, line);
        // the title ribbon and the chart's name
        Bitmap rib = tinted("TitleBase_00^t", 0x3C2A10, 0xFFF4D8, 0xF0);
        sq.set(x0 - p * 0.04f, y0 - title * 1.05f, x0 + p * 1.04f, y0 - title * 0.05f);
        if (rib != null) {
            bmp.setAlpha(a);
            c.drawBitmap(rib, null, sq, bmp);
        }
        String name = view.getContext().getString(kind == 0 ? R.string.minimap_chart_triforce : R.string.minimap_chart_treasure, number);
        text.setColor(0xFF3C2A10);
        text.setAlpha(a);
        text.setTextSize(title * 0.52f);
        float tw = text.measureText(name);
        if (tw > sq.width() * 0.9f) text.setTextSize(title * 0.52f * sq.width() * 0.9f / tw);
        c.drawText(name, sq.centerX(), sq.centerY() + text.getTextSize() * 0.35f, text);
    }

    // ---- the map button: hide, by the map's corner on the picture's outer side (above the map in the lower half of
    // the picture, below it in the upper half); hidden: the GamePad's map button where the map's corner was. (The size
    // goes by a tap on the map or a short − on a controller.)
    private void drawButtons(Canvas c, RectF tv, float outer, int a) {
        float b = outer * 0.16f, gap = outer * 0.04f;
        boolean bottom = r.centerY() > tv.centerY(), right = r.centerX() > tv.centerX();
        float y0 = bottom ? r.top - gap - b : r.bottom + gap;
        float x = right ? r.right - b : r.left;
        btnHide.set(x, y0, x + b, y0 + b);
        button(c, btnHide, a, true);
        line.setStrokeWidth(b * 0.1f);
        line.setStrokeCap(Paint.Cap.ROUND);
        float k = b * 0.2f;
        c.drawLine(btnHide.centerX() - k, btnHide.centerY() - k, btnHide.centerX() + k, btnHide.centerY() + k, line);
        c.drawLine(btnHide.centerX() - k, btnHide.centerY() + k, btnHide.centerX() + k, btnHide.centerY() - k, line);
        line.setStrokeCap(Paint.Cap.BUTT);
    }

    // a round button: paper inside a brown ring, a soft shadow; leaves line set up for its brown sign
    private void button(Canvas c, RectF b, int a, boolean ring) {
        float rad = b.width() / 2;
        line.setStyle(Paint.Style.FILL);
        line.setColor(0x55000000);
        line.setAlpha(0x55 * a / 255);
        c.drawCircle(b.centerX() + rad * 0.06f, b.centerY() + rad * 0.1f, rad, line);
        line.setColor(0xFFFFF4D8);
        line.setAlpha(0xF0 * a / 255);
        c.drawCircle(b.centerX(), b.centerY(), rad, line);
        line.setStyle(Paint.Style.STROKE);
        line.setColor(EDGE);
        line.setAlpha(a);
        if (ring) {
            line.setStrokeWidth(rad * 0.14f);
            c.drawCircle(b.centerX(), b.centerY(), rad * 0.93f, line);
        }
    }

    private void drawShowButton(Canvas c, RectF place, int a) {
        float b = place.width() * 0.3f;
        boolean bottom = place.centerY() > (view.getHeight() / 2f), right = place.centerX() > (view.getWidth() / 2f);
        float x = right ? place.right - b : place.left, y = bottom ? place.bottom - b : place.top;
        btnShow.set(x, y, x + b, y + b);
        button(c, btnShow, a, true);
        Bitmap icon = GameIcons.get(view.getContext(), "MapBtn_00^l", this::repaint);
        if (icon != null) {
            sq.set(btnShow);
            sq.inset(b * 0.16f, b * 0.16f);
            bmp.setAlpha(a);
            c.drawBitmap(icon, null, sq, bmp);
        }
    }

    private void drawSea(Canvas c, float lx, float lz, float k, float cx, float cy, float side, int a) {
        // the chart squares in view: their islands (filled in by the fishman) or a question mark; the grid between them
        float half = SQUARE / 2;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 7; col++) {
                float sx = (col - 3) * SQUARE, sz = (row - 3) * SQUARE;
                sq.set(cx + (sx - half - lx) * k, cy + (sz - half - lz) * k, cx + (sx + half - lx) * k, cy + (sz + half - lz) * k);
                if (!RectF.intersects(sq, ext)) continue;
                int room = row * 7 + col + 1;
                if (st[11 + room] != 0 && terrain) {
                    // (small on the screen, as on the full map's whole sea: the small copy; the full one only when large)
                    RectF tr = terrainRect[room];
                    boolean large = tr == null || tr.width() * k > SMALL * 1.25f;
                    Bitmap t = large ? terrain(room) : null;
                    if (t == null) t = terrainSmall.get(room);
                    if (t == null && !large) t = terrain(room);
                    if (t != null && tr != null) {
                        sq.set(cx + (tr.left - lx) * k, cy + (tr.top - lz) * k, cx + (tr.right - lx) * k, cy + (tr.bottom - lz) * k);
                        if (RectF.intersects(sq, ext)) {
                            bmp.setAlpha(a);
                            c.drawBitmap(t, null, sq, bmp);
                        }
                    }
                } else if (st[11 + room] != 0) {
                    boolean large = SQUARE * k > SMALL * 1.25f;
                    Bitmap isl = redraw && large ? redrawn(room) : null;
                    if (isl == null && redraw) isl = redrawnSmall.get(room * 2 + (outlines ? 1 : 0));
                    if (isl == null && redraw && !large) isl = redrawn(room);
                    if (isl == null)
                        isl = outlines ? outline(GameIcons.mapIsland(room, false), LINE, 1, hd)
                                : tinted(GameIcons.mapIsland(room, false), ISLAND_BLACK, ISLAND_WHITE, 0xFF, hd);
                    if (isl != null) {
                        bmp.setAlpha(a);
                        c.drawBitmap(isl, null, sq, bmp);
                    }
                } else {
                    int qc = outlines || terrain ? LINE_UNKNOWN : UNKNOWN;
                    text.setColor(qc);
                    text.setAlpha((qc >>> 24) * a / 255);
                    float qs = Math.min(side * 0.5f, SQUARE * k * 0.6f);
                    text.setTextSize(qs);
                    upright(c, sq.centerX(), sq.centerY());
                    c.drawText("?", sq.centerX(), sq.centerY() + qs * 0.36f, text);
                    c.restore();
                }
            }
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeWidth(Math.max(1, side * 0.008f));
        line.setPathEffect(new DashPathEffect(new float[] {side * 0.025f, side * 0.02f}, 0));
        int gc = outlines || terrain ? LINE_GRID : GRID;
        line.setColor(gc);
        line.setAlpha((gc >>> 24) * a / 255);
        for (int i = 0; i <= 7; i++) {
            float g = (i - 3.5f) * SQUARE;
            float gx = cx + (g - lx) * k, gz = cy + (g - lz) * k;
            if (gx > ext.left && gx < ext.right) c.drawLine(gx, ext.top, gx, ext.bottom, line);
            if (gz > ext.top && gz < ext.bottom) c.drawLine(ext.left, gz, ext.right, gz, line);
        }
        line.setPathEffect(null);
        if (names) drawIslandNames(c, lx, lz, k, cx, cy, side, a);
        // the King of Red Lions where Link left him (on board: Link's own mark)
        if (st[5] != 0 && st[4] == 0) {
            Bitmap ship = GameIcons.get(view.getContext(), "Ship_00^q", this::repaint);
            if (ship != null) {
                float h = side * 0.2f, wd = h * ship.getWidth() / ship.getHeight();
                float px = cx + (st[6] - lx) * k, py = cy + (st[7] - lz) * k;
                sq.set(px - wd / 2, py - h / 2, px + wd / 2, py + h / 2);
                bmp.setAlpha(a);
                if (RectF.intersects(sq, ext)) {
                    upright(c, px, py);
                    c.drawBitmap(ship, null, sq, bmp);
                    c.restore();
                }
            }
        }
        drawProgress(c, null, lx, lz, k, cx, cy, a, -1e9f, 1e9f);
    }

    /** the sea chart, or a part of it, for the full map and the game's chart in the + menu (FullMap, ChartMarks): world
     *  (lx, lz) at box's middle, k pixels per world unit, filling box (the caller clips); side: the size Link's mark, the
     *  boat and the names go by (as the mini map's side); state: Native.miniMapState (the squares filled in, Link, the
     *  boat; Link only on the sea); pictures: PICTURES_*, filled (never see-through), north up; the progress marks as on
     *  the mini map */
    /** the full map's Map decorations: the chart paper's grain over the Terrain sea (drawSeaChart) */
    boolean seaGrain;

    // the full map's Terrain sea: a few of the game chart's wave marks (MenuMap_00 MapDecoLine_00) in each square, away
    // from its middle (where most islands lie), the same places each time; a mark's size grows with the zoom up to a
    // limit (ox, oz: the sea's middle on the screen)
    private static final float[][] WAVES = {{-0.34f, -0.30f}, {0.28f, -0.36f}, {0.36f, 0.12f}, {-0.30f, 0.30f}, {0.10f, 0.38f}};

    private void drawWaves(Canvas c, float ox, float oz, float k, int a) {
        Bitmap w = tinted("MapDecoLine_00^t", 0x2C5A9A, 0xD8E8FF, 0x80);
        if (w == null) return;
        float dp = view.getResources().getDisplayMetrics().density, s = SQUARE * k;
        float ww = Math.max(10 * dp, Math.min(40 * dp, s * 0.11f)), wh = ww * w.getHeight() / w.getWidth();
        bmp.setAlpha(a);
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 7; col++) {
                float mx = ox + (col - 3) * s, mz = oz + (row - 3) * s;
                if (mx + s / 2 < ext.left || mx - s / 2 > ext.right || mz + s / 2 < ext.top || mz - s / 2 > ext.bottom) continue;
                int seed = row * 7 + col;
                for (int i = 0; i < WAVES.length; i++) {
                    if ((seed + i) % 5 == 0) continue;  // (4 of the 5, not the same in each square)
                    float j = ((seed * 37 + i * 11) % 9 - 4) * 0.012f;
                    float x = mx + (WAVES[i][0] + j) * s, y = mz + (WAVES[i][1] - j) * s;
                    sq.set(x - ww / 2, y - wh / 2, x + ww / 2, y + wh / 2);
                    if (!RectF.intersects(sq, ext)) continue;
                    c.drawBitmap(w, null, sq, bmp);
                }
            }
    }

    void drawSeaChart(Canvas c, float[] state, int pictures, boolean names, RectF box, float lx, float lz, float k, float side, int a) {
        float[] keepSt = st;
        boolean keepHd = hd, keepRedraw = redraw, keepTerrain = terrain, keepOutlines = outlines, keepNames = this.names;
        float keepTurn = shownTurn, keepSide = shownSide;
        st = state.clone();
        if (st[0] != 1) st[5] = 0;  // (no boat off the sea)
        hd = pictures != PICTURES_ORIGINAL;
        redraw = pictures == PICTURES_REDRAWN;
        terrain = pictures == PICTURES_TERRAIN;
        outlines = false;
        this.names = names;
        shownTurn = 0;
        shownSide = side;
        try {
            refreshPlaces();
            ext.set(box);
            float cx = box.centerX(), cy = box.centerY(), mx = lx, mz = lz;
            if (terrain) {
                line.setStyle(Paint.Style.FILL);
                line.setColor(SEA_BLUE);
                line.setAlpha(a);
                c.drawRect(box, line);
                Bitmap tile = seaGrain ? paperTile(true) : null;
                if (tile != null) {  // (the full map: the chart paper's grain over the blue, faint)
                    BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
                    float ts = SQUARE / 2 * k / tile.getWidth();
                    m.reset();
                    m.setScale(ts, ts);
                    m.postTranslate(cx - mx * k, cy - mz * k);
                    sh.setLocalMatrix(m);
                    paper.setShader(sh);
                    paper.setAlpha(a * 0x30 / 255);
                    c.drawRect(box, paper);
                }
                if (seaGrain) drawWaves(c, cx - mx * k, cy - mz * k, k, a);
            } else {
                Bitmap tile = paperTile(true);
                if (tile != null) {
                    BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
                    float ts = SQUARE / 2 * k / tile.getWidth();
                    m.reset();
                    m.setScale(ts, ts);
                    m.postTranslate(cx - mx * k, cy - mz * k);
                    sh.setLocalMatrix(m);
                    paper.setShader(sh);
                    paper.setAlpha(a);
                    c.drawRect(box, paper);
                } else {
                    line.setStyle(Paint.Style.FILL);
                    line.setColor(0xFF000000 | PAPER_BLACK);
                    line.setAlpha(a);
                    c.drawRect(box, line);
                }
            }
            drawSea(c, mx, mz, k, cx, cy, side, a);
            if (st[0] == 1) {
                float px = cx + (st[1] - mx) * k, py = cy + (st[2] - mz) * k;
                if (box.contains(px, py)) drawLink(c, px, py, side, st[3], a);
            }
        } finally {
            st = keepSt;
            hd = keepHd;
            redraw = keepRedraw;
            terrain = keepTerrain;
            outlines = keepOutlines;
            this.names = keepNames;
            shownTurn = keepTurn;
            shownSide = keepSide;
        }
    }

    /** the full map indoors: the stage Link is in, floor fi (FLOR index, as indoorFloors; -1: the stage has none), in a
     *  Map pictures style (pictures) on the indoor paper, at (lx, lz) in the middle of box, k pixels per world unit;
     *  Link on it when he is on that floor; false while the stage's data is being read */
    boolean drawIndoorChart(Canvas c, float[] state, int pictures, int fi, RectF box, float lx, float lz, float k, float side, int a) {
        float[] keepSt = st;
        boolean keepHd = hd, keepRedraw = redraw, keepTerrain = terrain, keepOutlines = outlines;
        float keepTurn = shownTurn, keepSide = shownSide;
        st = state.clone();
        hd = pictures != PICTURES_ORIGINAL;
        redraw = pictures == PICTURES_REDRAWN;
        terrain = pictures == PICTURES_TERRAIN;
        outlines = false;
        shownTurn = 0;
        shownSide = side;
        try {
            refreshPlaces();
            ext.set(box);
            float cx = box.centerX(), cy = box.centerY();
            Bitmap tile = paperTile(false);
            if (tile != null) {
                BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
                float ts = 1500f * k / tile.getWidth();
                m.reset();
                m.setScale(ts, ts);
                m.postTranslate(cx - lx * k, cy - lz * k);
                sh.setLocalMatrix(m);
                paper.setShader(sh);
                paper.setAlpha(a);
                c.drawRect(box, paper);
            } else {
                line.setStyle(Paint.Style.FILL);
                line.setColor(0xFF000000 | INDOOR_PAPER_BLACK);
                line.setAlpha(a);
                c.drawRect(box, line);
            }
            if (st[0] != 2 || !stageReady()) return false;
            Plan pl = floorPlan(fi, null, 0);
            if (pl != null) chartPlan = pl;
            else if (chartPlan != null && chartPlan.style == planStyle()) pl = chartPlan;  // (the last one while this one is made)
            drawIndoors(c, lx, lz, k, cx, cy, a, fi, pl, finePlan(pl, box, lx, lz, k, fi));
            if (fi == floorIndex(st[61])) {
                float px = cx + (st[1] - lx) * k, py = cy + (st[2] - lz) * k;
                if (box.contains(px, py)) drawLink(c, px, py, side, st[3], a);
            }
            return true;
        } finally {
            st = keepSt;
            hd = keepHd;
            redraw = keepRedraw;
            terrain = keepTerrain;
            outlines = keepOutlines;
            shownTurn = keepTurn;
            shownSide = keepSide;
        }
    }

    /** the full map: a finer plan around what box shows (asked for when pl's texels are larger than FINE_AT pixels and
     *  the last one doesn't cover the screen at about this scale; one at a time), or null */
    private Plan finePlan(Plan pl, RectF box, float lx, float lz, float k, int fi) {
        if (pl == null || pl.img == null) return null;
        float upp = pl.rect.width() / pl.img.getWidth();
        if (upp * k <= FINE_AT) return null;
        // the one asked for last, once made
        if (finePending != null && !plansPending.contains(finePending)) {
            Plan done = plans.get(finePending);
            if (done != null) {
                fine = done;
                System.arraycopy(pendingWant, 0, fineWant, 0, 5);
            }
            finePending = null;
        }
        // the screen's world box (no further than the floor's)
        float x0 = Math.max(pl.rect.left, lx - box.width() / 2 / k), x1 = Math.min(pl.rect.right, lx + box.width() / 2 / k);
        float z0 = Math.max(pl.rect.top, lz - box.height() / 2 / k), z1 = Math.min(pl.rect.bottom, lz + box.height() / 2 / k);
        float want = Math.max(1.5f, FINE_AT / k);
        Plan f = fine;
        boolean kind = f != null && sameKind(f, pl);
        // the last one while it covers the screen at a scale no coarser than 1.5 times this one, not over 3 times finer
        if (kind && fineWant[4] <= want * 1.5f && fineWant[4] >= want / 3 && fineWant[0] <= x0 && fineWant[2] >= x1
                && fineWant[1] <= z0 && fineWant[3] >= z1)
            return f;
        if (finePending == null && x1 > x0 && z1 > z0) {
            // a new one: the screen and half of it round, at the scale wanted (at most ~2300 texels across)
            float mx = (x1 - x0) * 0.5f, mz = (z1 - z0) * 0.5f;
            float[] w = {x0 - mx, z0 - mz, x1 + mx, z1 + mz};
            float fu = Math.max(want, Math.max(w[2] - w[0], w[3] - w[1]) / 2300f);
            Plan made = floorPlan(fi, w, fu);
            if (made != null) {
                fine = made;
                System.arraycopy(w, 0, fineWant, 0, 4);
                fineWant[4] = fu;
                return made;
            }
            finePending = lastPlanKey;
            System.arraycopy(w, 0, pendingWant, 0, 4);
            pendingWant[4] = fu;
        }
        // (meanwhile the last one, when it is of this floor)
        return kind ? f : null;
    }

    private boolean noProgress;  // the + menu's dungeon map: its progress marks are ChartMarks' own

    /** the + menu's dungeon map drawn anew (ChartMarks): the indoor paper over box, then the dungeon's floor `floor` (0 =
     *  1F, -1 = B1, ...) in a Map pictures style, the dungeon map's origin (DMAP) at (sx, sy), ppt pixels a picture
     *  texel (the game's own placing of its floor pictures); doors, chests, stairs, Link on his floor; false (nothing
     *  drawn) while the stage's data isn't read or it has no dungeon map */
    boolean drawDungeonFloor(Canvas c, float[] state, int pictures, int floor, float sx, float sy, float ppt, RectF box, boolean paperToo) {
        if (state == null || state.length < 72 || state[0] != 2 || !stageReady() || !hasDungeonMap()) return false;
        int fi = -1;
        for (int i = 0; 4 + 2 * i < info.length; i++)
            if ((int) info[4 + 2 * i] == floor) fi = i;
        float[] keepSt = st;
        boolean keepHd = hd, keepRedraw = redraw, keepTerrain = terrain, keepOutlines = outlines;
        float keepTurn = shownTurn, keepSide = shownSide;
        st = state.clone();
        hd = pictures != PICTURES_ORIGINAL;
        redraw = pictures == PICTURES_REDRAWN;
        terrain = pictures == PICTURES_TERRAIN;
        outlines = false;
        shownSide = Math.min(box.width(), box.height()) * 1.3f;
        noProgress = true;
        c.save();
        try {
            c.clipRect(box);
            if (paperToo) {
                Bitmap tile = paperTile(false);
                if (tile != null) {
                    BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
                    m.reset();
                    float ts = box.width() / 3 / tile.getWidth();
                    m.setScale(ts, ts);
                    m.postTranslate(box.left, box.top);
                    sh.setLocalMatrix(m);
                    paper.setShader(sh);
                    paper.setAlpha(0xFF);
                    c.drawRect(box, paper);
                } else {
                    line.setStyle(Paint.Style.FILL);
                    line.setColor(0xFF000000 | INDOOR_PAPER_BLACK);
                    c.drawRect(box, line);
                }
            }
            // the screen is the pictures' frame: this stage's world turned (dungeon.turn) and scaled by the DMAP; the
            // world point under the origin
            float u = MiniMapDungeons.TEXEL * info[2], k = ppt / u, ox = info[0], oz = info[1];
            double t = Math.toRadians(dungeon.turn);
            float cos = (float) Math.cos(t), sin = (float) Math.sin(t);
            float wx = cos * ox - sin * oz, wz = sin * ox + cos * oz;
            shownTurn = -dungeon.turn;
            c.rotate(shownTurn, sx, sy);
            float r = Math.max(box.width(), box.height());
            ext.set(sx - 2 * r, sy - 2 * r, sx + 2 * r, sy + 2 * r);
            drawIndoors(c, wx, wz, k, sx, sy, 0xFF, fi, terrain ? floorPlan(fi, null, 0) : null);
            if (fi >= 0 && fi == floorIndex(st[61])) {
                float px = sx + (st[1] - wx) * k, py = sy + (st[2] - wz) * k;
                upright(c, px, py);
                drawLink(c, px, py, shownSide, st[3], 0xFF);
                c.restore();
            }
            return true;
        } finally {
            c.restore();
            noProgress = false;
            st = keepSt;
            hd = keepHd;
            redraw = keepRedraw;
            terrain = keepTerrain;
            outlines = keepOutlines;
            shownTurn = keepTurn;
            shownSide = keepSide;
        }
    }

    /** the + menu's dungeon map: the stage's data read and a dungeon map for it (a GamePad layout of its rooms) */
    boolean dungeonPageReady(float[] state) {
        return state != null && state.length >= 72 && state[0] == 2 && stageReady() && hasDungeonMap();
    }

    /** the indoor paper over box (the + menu's dungeon map drawn anew) */
    void drawIndoorPaper(Canvas c, RectF box) {
        Bitmap tile = paperTile(false);
        if (tile != null) {
            BitmapShader sh = new BitmapShader(tile, Shader.TileMode.MIRROR, Shader.TileMode.MIRROR);
            m.reset();
            float ts = box.width() / 3 / tile.getWidth();
            m.setScale(ts, ts);
            m.postTranslate(box.left, box.top);
            sh.setLocalMatrix(m);
            paper.setShader(sh);
            paper.setAlpha(0xFF);
            c.drawRect(box, paper);
        } else {
            line.setStyle(Paint.Style.FILL);
            line.setColor(0xFF000000 | INDOOR_PAPER_BLACK);
            c.drawRect(box, line);
        }
    }

    /** a dungeon picture of no room (the Forsaken Fortress's outside) in a Map pictures style: visited colours; Redrawn
     *  drawn anew (the HD one until then); Terrain as HD (it has no ground of its own); null while not loaded */
    Bitmap roomPicture(String texture, int pictures) {
        boolean keep = outlines;
        outlines = false;
        try {
            Bitmap b = pictures == PICTURES_REDRAWN ? roomRedrawn(texture, true) : null;
            return b != null ? b : tinted(texture, ROOM_BLACK, ROOM_WHITE, ROOM_ALPHA, pictures != PICTURES_ORIGINAL);
        } finally {
            outlines = keep;
        }
    }

    /** a dungeon map figure's picture (ChartMarks.Fig) in its layout's colours (DungeonMapStatue_00, Ship_00,
     *  Searchlight_00, DungeonMapFloor_00's P_Destination_00 with its red vertex colour), or null */
    Bitmap figure(String pane) {
        switch (pane) {
            case "P_Statue_00": return GameIcons.get(view.getContext(), "MapStatueS_00^q", this::repaint);
            case "P_Statue_01": return tinted("MapStatueS_01^t", 0x0000FF, 0x008CFF);
            case "P_Direction_00": return tinted("MapDirectionArrow_00^t", 0xFF0000, 0xFFFFFF);
            case "P_Ship_00": return GameIcons.get(view.getContext(), "Ship_00^q", this::repaint);
            case "P_Searchlight_00": return GameIcons.get(view.getContext(), "MapSearchlight_00^q", this::repaint);
            case "P_Searchlight_01": return GameIcons.get(view.getContext(), "MapSearchlight_01^q", this::repaint);
            case "P_Destination_00": return tinted("DMDestination_00^t", 0xFF0000, 0xFF0000);
            default: return null;
        }
    }

    /** indoors: the stage's floors (FLOR: 0 = 1F, -1 = B1, 1 = 2F ..., lowest first), or null without any (or not read yet) */
    int[] indoorFloors() {
        if (info == null || info.length < 5) return null;
        int[] f = new int[(info.length - 3) / 2];
        for (int i = 0; i < f.length; i++) f[i] = (int) info[4 + 2 * i];
        return f;
    }

    /** indoors: the stage Link is in read (it is read when it isn't) */
    boolean indoorReady() { return stageReady(); }

    /** indoors: the FLOR index of a height (-1: the stage has none) */
    int indoorFloorAt(float y) { return floorIndex(y); }

    /** indoors: the world box the full map shows: a dungeon map's pictures on all its floors, else the full map's plan
     *  (null while not known) */
    RectF indoorBounds() {
        if (hasDungeonMap()) {
            float u = MiniMapDungeons.TEXEL * info[2], ox = info[0], oz = info[1];
            double t = Math.toRadians(dungeon.turn);
            float cos = (float) Math.cos(t), sin = (float) Math.sin(t);
            RectF r = null;
            for (MiniMapDungeons.Pic p : dungeon.pics) {
                float x0 = ox + (p.x - p.w / 2) * u, z0 = oz - (p.y + p.h / 2) * u;
                for (int i = 0; i < 4; i++) {
                    float px = x0 + (i % 2) * p.w * u, pz = z0 + (i / 2) * p.h * u;
                    // (the pictures' frame turned back into this stage's world)
                    float wx = cos * px - sin * pz, wz = sin * px + cos * pz;
                    if (r == null) r = new RectF(wx, wz, wx, wz);
                    else r.union(wx, wz);
                }
            }
            if (r != null) return r;
        }
        Plan pl = chartPlan;
        return pl != null && pl.img != null ? new RectF(pl.rect) : null;
    }

    /** the progress marks' states read again when they are older than MARKS_MS (the poll reads them only on the sea
     *  while the mini map is on) */
    private void refreshPlaces() {
        long t = SystemClock.uptimeMillis();
        if (progressMarks == MARKS_OFF || t - statesAt < MARKS_MS) return;
        statesAt = t;
        byte[] d = Native.progressData();
        int[] ps = d != null && d.length > 0x79C ? ProgressPlaces.states(d) : null;
        if (ps != null) salvagePoint = ProgressPlaces.salvagePoint(d);
        placeStates = ps;
    }

    /** the names of the charted islands in view (the game's own, GameText.islandName) under their land, upright, dark on
     *  a paper edge (see-through: light on a dark one) */
    private void drawIslandNames(Canvas c, float lx, float lz, float k, float cx, float cy, float side, int a) {
        float half = SQUARE / 2, size = side * 0.065f;
        for (int row = 0; row < 7; row++)
            for (int col = 0; col < 7; col++) {
                int room = row * 7 + col + 1;
                if (st[11 + room] == 0) continue;
                String name = GameText.islandName(room);
                if (name == null) GameText.prepare(view.getContext(), this::repaint);  // (read once, in the background)
                float[] spot = islandSpot(room);
                RectF tr = terrain ? terrainRect[room] : null;
                if (name == null || spot == null || terrain && tr == null) continue;
                float sx = (col - 3) * SQUARE - half, sz = (row - 3) * SQUARE - half;
                // (terrain: under its land, the box less the shallows round it)
                float wx = tr != null ? tr.centerX() : sx + spot[0] * SQUARE, wz = tr != null ? tr.bottom - 1200 : sz + Math.min(spot[1] + 0.02f, 0.97f) * SQUARE;
                float px = cx + (wx - lx) * k, py = cy + (wz - lz) * k;
                if (!ext.contains(px, py)) continue;
                text.setTextSize(size);
                float tw = text.measureText(name), limit = SQUARE * k * 0.95f;
                if (tw > limit) text.setTextSize(size * limit / tw);
                float base = py + text.getTextSize() * 0.85f, tx = px;
                if (shownTurn + innerTurn == 0) {
                    // north up (the full map): a name by the sea's edge kept inside it (it was cut off there)
                    float edgeIn = text.getTextSize() * 0.3f, hw = text.measureText(name) / 2 + edgeIn;
                    float seaL = cx + (-3.5f * SQUARE - lx) * k, seaR = cx + (3.5f * SQUARE - lx) * k;
                    float seaB = cy + (3.5f * SQUARE - lz) * k;
                    if (tx - hw < seaL) tx = seaL + hw;
                    else if (tx + hw > seaR) tx = seaR - hw;
                    base = Math.min(base, seaB - text.getTextSize() * 0.25f - edgeIn);
                }
                upright(c, px, py);
                text.setStyle(Paint.Style.STROKE);
                text.setStrokeWidth(text.getTextSize() * 0.22f);
                text.setStrokeJoin(Paint.Join.ROUND);
                int edge = outlines || terrain ? LINE_EDGE : 0xD0000000 | PAPER_WHITE;
                text.setColor(edge);
                text.setAlpha((edge >>> 24) * a / 255);
                c.drawText(name, tx, base, text);
                text.setStyle(Paint.Style.FILL);
                int fill = outlines || terrain ? LINE : 0xFF000000 | ISLAND_BLACK;
                text.setColor(fill);
                text.setAlpha((fill >>> 24) * a / 255);
                c.drawText(name, tx, base, text);
                c.restore();
            }
    }

    /** the middle of a sea room's picture's land across and its lowest land down (fractions of the square), from the
     *  GamePad picture's alpha; null while it isn't there */
    private float[] islandSpot(int room) {
        if (islandSpot[room] != null) return islandSpot[room];
        Bitmap b = GameIcons.get(view.getContext(), GameIcons.mapIsland(room, false), this::repaint);
        if (b == null) return null;
        int w = b.getWidth(), h = b.getHeight(), x0 = w, x1 = -1, y1 = -1;
        int[] px = new int[w * h];
        b.getPixels(px, 0, w, 0, 0, w, h);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                int p = px[y * w + x];
                if ((p >>> 24) * (p >> 16 & 0xFF) < 128 * 255) continue;  // land: light ink, opaque
                x0 = Math.min(x0, x);
                x1 = Math.max(x1, x);
                y1 = y;
            }
        islandSpot[room] = x1 < 0 ? new float[] {0.5f, 0.6f} : new float[] {(x0 + x1 + 1) / 2f / w, (y1 + 1f) / h};
        return islandSpot[room];
    }

    /** a sea room's island from above (Terrain; its world box in terrainRect), or null while that is being built in
     *  the background (or the game has none) */
    private Bitmap terrain(int room) {
        Bitmap b = terrains.get(room);
        if (b != null) return b;
        MainActivity act = MainActivity.instance;
        if (act == null || !terrainPending.add(room)) return null;
        String dir = act.gameDir();
        HD_WORK.execute(() -> {
            int[] p = Native.miniMapTerrain(dir, room);
            Bitmap t = p == null ? null : Bitmap.createBitmap(p, 5, p[0], p[0], p[1], Bitmap.Config.ARGB_8888);
            Bitmap small = t == null ? null : small(t);
            view.post(() -> {
                if (t == null) return;  // (stays pending: not tried again)
                terrainPending.remove(room);
                terrainSmall.put(room, small);
                float x0 = Float.intBitsToFloat(p[2]), z0 = Float.intBitsToFloat(p[3]), upp = Float.intBitsToFloat(p[4]);
                terrainRect[room] = new RectF(x0, z0, x0 + p[0] * upp, z0 + p[1] * upp);
                terrains.put(room, t);
                repaint();
            });
        });
        return null;
    }

    /** b at SMALL texels at most (itself when it is no larger) */
    private static Bitmap small(Bitmap b) {
        int m = Math.max(b.getWidth(), b.getHeight());
        if (m <= SMALL) return b;
        float f = SMALL / (float) m;
        return Bitmap.createScaledBitmap(b, Math.max(1, Math.round(b.getWidth() * f)), Math.max(1, Math.round(b.getHeight() * f)), true);
    }

    /** a sea room's island redrawn (redraw), or null while that is being built in the background */
    private Bitmap redrawn(int room) {
        int key = room * 2 + (outlines ? 1 : 0);
        Bitmap b = redrawn.get(key);
        if (b != null) return b;
        Bitmap orig = GameIcons.get(view.getContext(), GameIcons.mapIsland(room, false), this::repaint);
        if (orig == null || !redrawPending.add(key)) return null;
        boolean lines = outlines;
        HD_WORK.execute(() -> {
            Bitmap s = redraw(orig, lines), small = small(s);
            view.post(() -> {
                redrawPending.remove(key);
                redrawn.put(key, s);
                redrawnSmall.put(key, small);
                repaint();
            });
        });
        return null;
    }

    /** an island picture (grey + alpha: light land, a dark ink edge, the island's name in the game's script on it) drawn
     *  anew about 500 texels across: its land (light ink over half opaque) with the script's small enclosed gaps filled,
     *  smoothed a little and enlarged (Catmull-Rom), then an edge from the distance to the land's border: the land in the
     *  island colour, a crisp dark edge line and a soft dark shadow outside, as the GamePad's ink; lines (see-through):
     *  the edge line in the line colour with a dark edge outside */
    static Bitmap redraw(Bitmap src, boolean lines) {
        return redraw(src, lines, 0xFF000000 | ISLAND_WHITE, 0xFF000000 | ISLAND_BLACK, LINE, src.getWidth() / 300f, true, false, 500);
    }

    /** a shape drawn anew as redraw does it: fill (ARGB) the land, ink (ARGB) its edge line and shadow, line (ARGB) the
     *  see-through line; unit: the edge's unit in source texels; gaps: the script's small enclosed gaps filled; byAlpha:
     *  the land is the opaque part (a room picture, a floor mask), not the light ink; about `size` texels across */
    static Bitmap redraw(Bitmap src, boolean lines, int fill, int ink, int line, float unit, boolean gaps, boolean byAlpha, int size) {
        int w = src.getWidth(), h = src.getHeight(), n = w * h;
        int[] in = new int[n];
        src.getPixels(in, 0, w, 0, 0, w, h);
        float[] g = new float[n];
        boolean[] land = new boolean[n];
        for (int i = 0; i < n; i++) {
            g[i] = byAlpha ? (in[i] >>> 24) / 255f : (in[i] >>> 24) * (in[i] >> 16 & 0xFF) / (255f * 255f);
            land[i] = g[i] > 0.5f;
        }
        // the script: gaps in the land not reaching the picture's edge, smaller than 60 texels of a 100-texel picture
        int maxGap = gaps ? Math.max(4, 60 * n / 10000) : 0;
        boolean[] seen = new boolean[n];
        int[] queue = new int[n];
        for (int s = 0; s < n; s++) {
            if (land[s] || seen[s]) continue;
            int head = 0, tail = 0;
            boolean open = false;
            queue[tail++] = s;
            seen[s] = true;
            while (head < tail) {
                int i = queue[head++], x = i % w, y = i / w;
                if (x == 0 || y == 0 || x == w - 1 || y == h - 1) open = true;
                if (x > 0 && !land[i - 1] && !seen[i - 1]) { seen[i - 1] = true; queue[tail++] = i - 1; }
                if (x < w - 1 && !land[i + 1] && !seen[i + 1]) { seen[i + 1] = true; queue[tail++] = i + 1; }
                if (y > 0 && !land[i - w] && !seen[i - w]) { seen[i - w] = true; queue[tail++] = i - w; }
                if (y < h - 1 && !land[i + w] && !seen[i + w]) { seen[i + w] = true; queue[tail++] = i + w; }
            }
            if (!open && tail < maxGap)
                for (int j = 0; j < tail; j++) g[queue[j]] = 1;
        }
        // smoothed (1 2 1 both ways), then enlarged
        float[] t = new float[n];
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                t[y * w + x] = (g[y * w + Math.max(0, x - 1)] + 2 * g[y * w + x] + g[y * w + Math.min(w - 1, x + 1)]) / 4;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                g[y * w + x] = (t[Math.max(0, y - 1) * w + x] + 2 * t[y * w + x] + t[Math.min(h - 1, y + 1) * w + x]) / 4;
        int f = Math.max(1, Math.min(8, Math.round((float) size / Math.max(w, h))));
        int W = w * f, H = h * f;
        float[] wx = new float[W * 4], wy = new float[H * 4];
        int[] ix = new int[W], iy = new int[H];
        catmullRom(W, f, wx, ix);
        catmullRom(H, f, wy, iy);
        float[] v = new float[W * H];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                float s = 0;
                for (int j = 0; j < 4; j++) {
                    int sy = Math.max(0, Math.min(h - 1, iy[y] + j - 1)) * w;
                    for (int i = 0; i < 4; i++) s += g[sy + Math.max(0, Math.min(w - 1, ix[x] + i - 1))] * wy[y * 4 + j] * wx[x * 4 + i];
                }
                v[y * W + x] = s;
            }
        // the distance to the land's border in texels (+ inside), from the field's slope near it
        float u = unit * f;  // the edge's unit
        int[] out = new int[W * H];
        int lineRgb = (lines ? line : ink) & 0xFFFFFF, lineA = (lines ? line : ink) >>> 24;
        float inkA = (ink >>> 24) / 255f, fillA = (fill >>> 24) / 255f;
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                int i = y * W + x;
                float gx = (v[y * W + Math.min(W - 1, x + 1)] - v[y * W + Math.max(0, x - 1)]) / 2;
                float gy = (v[Math.min(H - 1, y + 1) * W + x] - v[Math.max(0, y - 1) * W + x]) / 2;
                float d = (v[i] - 0.5f) / Math.max((float) Math.hypot(gx, gy), 1e-3f);
                d = Math.max(-40, Math.min(40, d));
                float edge = clamp01(1.5f * u - Math.abs(d - 1.2f * u));
                // colour over colour, premultiplied: shadow (or the lines' dark edge), land, edge line
                float r = 0, gg = 0, b = 0, al = 0;
                float shadow = lines ? (d < 0 ? (LINE_EDGE >>> 24) / 255f * clamp01(1 + d / (1.5f * u)) : 0)
                        : (d < 0 ? 0.4f * inkA * clamp01(1 + d / (3 * u)) : 0);
                al = shadow;
                if (!lines) {
                    r = shadow * (ink >> 16 & 0xFF);
                    gg = shadow * (ink >> 8 & 0xFF);
                    b = shadow * (ink & 0xFF);
                    float l = clamp01(d + 0.5f);
                    r = r * (1 - l) + (fill >> 16 & 0xFF) * fillA * l;
                    gg = gg * (1 - l) + (fill >> 8 & 0xFF) * fillA * l;
                    b = b * (1 - l) + (fill & 0xFF) * fillA * l;
                    al = al * (1 - l) + fillA * l;
                }
                float e = edge * lineA / 255f;
                r = r * (1 - e) + (lineRgb >> 16 & 0xFF) * e;
                gg = gg * (1 - e) + (lineRgb >> 8 & 0xFF) * e;
                b = b * (1 - e) + (lineRgb & 0xFF) * e;
                al = al * (1 - e) + e;
                if (al <= 0) continue;
                out[i] = Math.round(al * 255) << 24 | Math.min(255, Math.round(r / al)) << 16
                        | Math.min(255, Math.round(gg / al)) << 8 | Math.min(255, Math.round(b / al));
            }
        return Bitmap.createBitmap(out, W, H, Bitmap.Config.ARGB_8888);
    }

    private static float clamp01(float v) { return Math.max(0, Math.min(1, v)); }

    /** the game progress's places of the stage (null: the sea) between the heights y0..y1 at their spots: their icon on a
     *  small light disc, grey and faint while missing (Progress marks: what is missing, or everything) */
    private void drawProgress(Canvas c, String stage, float lx, float lz, float k, float cx, float cy, int a, float y0, float y1) {
        int[] ps = placeStates;
        if (progressMarks == MARKS_OFF || ps == null) return;
        float size = Math.min(shownSide * 0.075f, SQUARE * k * 0.09f);  // (the full map's whole sea: smaller)
        for (ProgressPlaces.Place p : ProgressPlaces.ALL) {
            if (p.kind == ProgressPlaces.SQUARE || (stage == null ? p.stage != null : !stage.equals(p.stage))) continue;
            int s = ps[p.index];
            if (progressMarks == MARKS_MISSING && s == ProgressPlaces.FOUND) continue;
            float x = p.x, z = p.z;
            int chart = p.sunkenChart();
            if (chart > 0) {
                float[] spot = ProgressPlaces.sunkenSpot(chart, salvagePoint);
                if (spot == null) continue;
                x = spot[0];
                z = spot[1];
            } else if (stage != null && (p.y < y0 - 50 || p.y >= y1)) continue;
            if (Float.isNaN(x)) continue;
            float px = cx + (x - lx) * k, py = cy + (z - lz) * k;
            if (!ext.contains(px, py)) continue;
            String tex = ProgressPlaces.icon(p);
            Bitmap b = tex != null ? GameIcons.get(view.getContext(), tex, this::repaint) : null;
            boolean missing = s == ProgressPlaces.MISSING;
            upright(c, px, py);
            line.setStyle(Paint.Style.FILL);
            line.setColor(missing ? 0x88302010 : 0xC0FFFFE7);
            line.setAlpha((missing ? 0x88 : 0xC0) * a / 255);
            c.drawCircle(px, py, size * 0.6f, line);
            if (b != null) {
                float f = size / Math.max(b.getWidth(), b.getHeight()), w = b.getWidth() * f, h = b.getHeight() * f;
                sq.set(px - w / 2, py - h / 2, px + w / 2, py + h / 2);
                bmp.setColorFilter(missing ? GREY : null);
                bmp.setAlpha(missing ? 150 * a / 255 : a);
                c.drawBitmap(b, null, sq, bmp);
                bmp.setColorFilter(null);
            }
            c.restore();
        }
    }

    /** Link's partner in his temple, as on the GamePad's dungeon map (DungeonMapMedley_00 / Makore_00: the face, the red
     *  direction arrow), when it is between the heights y0..y1 */
    private void drawPartner(Canvas c, float lx, float lz, float k, float cx, float cy, int a, float y0, float y1) {
        if (st.length <= 76 || st[72] == 0 || st[76] < y0 - 50 || st[76] >= y1) return;
        String tex = "M_Dai".equals(stage) ? "MapMedleyS_00^q" : "kaze".equals(stage) ? "MapMakoreS_00^q" : null;
        if (tex == null) return;
        float px = cx + (st[73] - lx) * k, py = cy + (st[74] - lz) * k;
        if (!ext.contains(px, py)) return;
        upright(c, px, py);  // (as Link's: on the map unturned, drawFigure turns its arrow by the map's turn)
        drawFigure(c, px, py, shownSide * 0.85f, st[75], a, tex, true, ARROW_BLACK);
        c.restore();
    }

    private void drawLink(Canvas c, float cx, float cy, float side, float facing, int a) {
        drawFigure(c, cx, cy, side, facing, a, "MapLinkS_00^l", this.face, arrowColour);
    }

    /** a figure's mark: its face (withFace) and its direction arrow in arrowCol (Link's: Mark size, Arrow colour) */
    private void drawFigure(Canvas c, float cx, float cy, float side, float facing, int a, String faceTex, boolean withFace, int arrowCol) {
        float face = side * 0.2f * markSize;
        Bitmap arrow = tinted("MapDirectionArrow_00^t", arrowCol, arrowCol == 0xFFFFFF ? 0x202020 : ARROW_WHITE);
        if (arrow != null) {
            // facing 0 = +z = south = down on the map; the arrow points up unturned: turned by 180 degrees - facing
            // (without the face: the arrow alone, a little smaller, its middle on Link's place)
            float aw = face * (withFace ? 0.62f : 0.5f), ah = aw * arrow.getHeight() / arrow.getWidth(), up = withFace ? face * 0.62f : 0;
            c.save();
            c.rotate(180f - (float) Math.toDegrees(facing) + shownTurn, cx, cy);
            sq.set(cx - aw / 2, cy - up - ah / 2, cx + aw / 2, cy - up + ah / 2);
            bmp.setAlpha(a);
            c.drawBitmap(arrow, null, sq, bmp);
            c.restore();
        }
        Bitmap f = withFace ? GameIcons.get(view.getContext(), faceTex, this::repaint) : null;
        if (f != null) {
            sq.set(cx - face / 2, cy - face / 2, cx + face / 2, cy + face / 2);
            bmp.setAlpha(a);
            c.drawBitmap(f, null, sq, bmp);
        }
    }

    /** a mark drawn at (x, y) on the turned map stays upright (c.restore() after it) */
    private void upright(Canvas c, float x, float y) {
        c.save();
        c.rotate(-shownTurn - innerTurn, x, y);
    }

    /** round: the map inside the bezel (a little under its rim, which hides the clip's edge) */
    private void clipInner(Canvas c, float cx, float cy, float outer) {
        clip.reset();
        clip.addCircle(cx, cy, outer / 2 * (BEZEL_INNER + 0.01f), Path.Direction.CW);
        c.clipPath(clip);
    }

    /** the bezel's shadow (P_CompassBaseSh_00: its shape in black at 122, 4 of 92 right and down) */
    private void drawBezelShadow(Canvas c, float outer, float alpha) {
        Bitmap sh = outlines ? bezelRingShadow() : tinted("CompassBase_00^t", 0x000000, 0x000000, 0x7A);
        if (sh == null) return;
        sq.set(r);
        sq.offset(outer * 4 / 92, outer * 4 / 92);
        bmp.setAlpha(Math.round(255 * alpha));
        c.drawBitmap(sh, null, sq, bmp);
    }

    /** the round frame: the compass's bronze bezel (its inside cut out), the face's gleam over the map, and on the dark
     *  rim N (in the needle's red), E, S, W in the face's cream with dots between, turned with the map */
    private void drawBezel(Canvas c, float outer, int a) {
        float cx = r.centerX(), cy = r.centerY(), rad = outer / 2;
        Bitmap gleam = tinted("CompassSpec_01^t", 0xFFFFFF, 0xFFFFFF, 0xFF);
        if (gleam != null) {
            float g = rad * BEZEL_INNER;
            sq.set(cx - g, cy - g, cx + g, cy + g);
            bmp.setAlpha(a);
            c.drawBitmap(gleam, null, sq, bmp);
        }
        Bitmap ring = bezelRing();
        if (ring != null) {
            bmp.setAlpha(a);
            c.drawBitmap(ring, null, r, bmp);
        } else {
            line.setStyle(Paint.Style.STROKE);
            line.setStrokeWidth(rad * (1 - BEZEL_INNER));
            line.setColor(0xFF000000 | BEZEL_BLACK);
            line.setAlpha(a);
            c.drawCircle(cx, cy, rad * (1 + BEZEL_INNER) / 2, line);
        }
        String[] dirs = {"N", "E", "S", "W"};
        text.setTextSize(rad * 0.15f);
        float at = rad * BEZEL_RIM;
        for (int i = 0; i < 8; i++) {
            double t = Math.toRadians(shownTurn + 45 * i);
            float x = cx + (float) Math.sin(t) * at, y = cy - (float) Math.cos(t) * at;
            if ((i & 1) != 0) {
                line.setStyle(Paint.Style.FILL);
                line.setColor(FACE_CREAM);
                line.setAlpha(a * 3 / 4);
                c.drawCircle(x, y, rad * 0.022f, line);
                continue;
            }
            text.setColor(i == 0 ? NEEDLE_RED : FACE_CREAM);
            text.setAlpha(a);
            c.drawText(dirs[i / 2], x, y + text.getTextSize() * 0.36f, text);
        }
    }

    /** see-through: the bezel's shadow only under the ring (the whole disk would darken the inside) */
    private Bitmap bezelRingShadow() {
        Bitmap b = tinted.get("#bezelsh");
        if (b != null) return b;
        Bitmap ring = bezelRing();
        if (ring == null) return null;
        int w = ring.getWidth(), h = ring.getHeight();
        int[] px = new int[w * h];
        ring.getPixels(px, 0, w, 0, 0, w, h);
        for (int i = 0; i < px.length; i++) px[i] = ((px[i] >>> 24) * 0x7A / 255) << 24;
        b = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
        tinted.put("#bezelsh", b);
        return b;
    }

    /** see-through: a texture's outline (its alpha's edge, `t` texels wide inside, in `color`, a dark edge one texel
     *  further out), or null while not built */
    private Bitmap outline(String texture, int color, int t, boolean sharp) {
        String key = texture + "#line" + Integer.toHexString(color) + "/" + t;
        Bitmap b = tinted.get(key + (sharp ? "@hd" : ""));
        if (b != null) return b;
        Bitmap orig = GameIcons.get(view.getContext(), texture, this::repaint);
        if (orig == null) return null;
        Bitmap src = sharp ? sharpened(texture, orig) : orig;
        if (src != orig) key += "@hd";
        else if (sharp && (b = tinted.get(key)) != null) return b;  // (the HD one still being built)
        b = outline(src, color, t * src.getWidth() / orig.getWidth());
        tinted.put(key, b);
        return b;
    }

    private static Bitmap outline(Bitmap src, int color, int t) {
        int w = src.getWidth(), h = src.getHeight();
        int[] px = new int[w * h];
        src.getPixels(px, 0, w, 0, 0, w, h);
        // distance (in 8-neighbour steps, up to t + 2) to the other side of the shape's edge, inside and outside
        boolean[] in = new boolean[w * h];
        for (int i = 0; i < px.length; i++) in[i] = (px[i] >>> 24) > 90;
        int[] din = new int[w * h], dout = new int[w * h];
        int far = t + 2;
        for (int i = 0; i < px.length; i++) {
            din[i] = in[i] ? far : 0;
            dout[i] = in[i] ? 0 : far;
        }
        for (int step = 1; step < far; step++)
            for (int y = 0; y < h; y++)
                for (int x = 0; x < w; x++) {
                    int i = y * w + x;
                    if (in[i] ? din[i] < far : dout[i] < far) continue;
                    boolean near = false;
                    for (int dy = -1; dy <= 1 && !near; dy++)
                        for (int dx = -1; dx <= 1; dx++) {
                            int xx = x + dx, yy = y + dy;
                            int d = xx < 0 || yy < 0 || xx >= w || yy >= h ? (in[i] ? 0 : far) : in[i] ? (in[yy * w + xx] ? din[yy * w + xx] : 0)
                                    : (in[yy * w + xx] ? 0 : dout[yy * w + xx]);
                            if (d == step - 1) {
                                near = true;
                                break;
                            }
                        }
                    if (near) {
                        if (in[i]) din[i] = step;
                        else dout[i] = step;
                    }
                }
        int ca = color >>> 24, rgb = color & 0xFFFFFF, ea = LINE_EDGE >>> 24;
        for (int i = 0; i < px.length; i++) {
            if (in[i]) px[i] = din[i] <= t ? ca << 24 | rgb : 0;
            else px[i] = dout[i] <= 2 ? (dout[i] == 1 ? ea : ea / 2) << 24 : 0;
        }
        return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
    }

    /** CompassBase_00 tinted as its material, the inside (the bronze disk under the map) cut out softly */
    private Bitmap bezelRing() {
        Bitmap b = tinted.get("#bezel");
        if (b != null) return b;
        Bitmap t = tinted("CompassBase_00^t", BEZEL_BLACK, BEZEL_WHITE, 0xFF);
        if (t == null) return null;
        int w = t.getWidth(), h = t.getHeight();
        int[] px = new int[w * h];
        t.getPixels(px, 0, w, 0, 0, w, h);
        float c0 = (w - 1) / 2f, cut = w / 2f * BEZEL_INNER;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                float d = (float) Math.hypot(x - c0, y - (h - 1) / 2f), f = Math.max(0, Math.min(1, d - cut + 0.5f));
                int i = y * w + x;
                px[i] = Math.round((px[i] >>> 24) * f) << 24 | px[i] & 0xFFFFFF;
            }
        b = Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
        tinted.put("#bezel", b);
        return b;
    }

    /** the map turned: a dark disc with an N on the frame where north is */
    private void drawNorth(Canvas c, float cx, float cy, float side, int a) {
        double t = Math.toRadians(shownTurn);
        float dx = (float) Math.sin(t), dy = (float) -Math.cos(t);
        float rad = side * 0.07f, reach = side / 2 / Math.max(Math.abs(dx), Math.abs(dy)) - rad * 0.6f;
        float x = cx + dx * reach, y = cy + dy * reach;
        line.setStyle(Paint.Style.FILL);
        line.setColor(EDGE);
        line.setAlpha(a);
        c.drawCircle(x, y, rad, line);
        text.setColor(0xFF000000 | PAPER_WHITE);
        text.setAlpha(a);
        text.setTextSize(rad * 1.3f);
        c.drawText("N", x, y + rad * 0.46f, text);
    }

    /** the paper: the middle half of MapArea_00 (its edges fade out on the GamePad's single square), tinted; indoors
     *  brown */
    /** a layout texture tinted as tinted() does, for the full map's frame (null while not built) */
    Bitmap texture(String name, int black, int white, int whiteAlpha) { return tinted(name, black, white, whiteAlpha); }

    /** the sea chart's paper (a tile of MapArea_00), for the full map's frame */
    Bitmap seaPaper() { return paperTile(true); }

    private Bitmap paperTile(boolean sea) {
        String key = sea ? "#paper" : "#paper_in";
        Bitmap b = tinted.get(key);
        if (b != null) return b;
        Bitmap t = sea ? tinted("MapArea_00^t", PAPER_BLACK, PAPER_WHITE, 0xFF) : tinted("MapArea_00^t", INDOOR_PAPER_BLACK, INDOOR_PAPER_WHITE, 0xFF);
        if (t == null) return null;
        int w = t.getWidth(), h = t.getHeight();
        b = Bitmap.createBitmap(t, w / 4, h / 4, w / 2, h / 2);
        tinted.put(key, b);
        return b;
    }

    // ---- indoors (st[0] == 2): the stage's data and, where it has no dungeon map, the collision plan, made on a
    // background thread (the files are read each time the stage or Link's height band changes); the full map asks for
    // plans of its own (a floor it shows, in its pictures)
    private String stage = "";
    private MiniMapDungeons.Dungeon dungeon;
    private float[] info;             // Native.miniMapStageInfo: DMAP origin x, z, scale, then FLOR height, floor pairs
    private float[] marks;            // Native.miniMapMarks: chests and doors
    // the plan's look (Map pictures): PLAN_CLASSIC the dungeon map's colours (Original, HD), PLAN_TERRAIN coloured by its
    // ground (Terrain; in a dungeon too, its rooms as visited / seen on the map), PLAN_MASK the floor redrawn (Redrawn)
    static final int PLAN_CLASSIC = 0, PLAN_TERRAIN = 1, PLAN_MASK = 2;

    /** a collision plan as made (img null: the stage has no floor there) and what for */
    private static final class Plan {
        Bitmap img, lines;            // the plan; its outline (see-through; Redrawn: made with it)
        final RectF rect = new RectF();  // where it lies
        int style;
        float from, to;               // its heights
        float y;                      // the height it was made around (a floor's band: its floor's start)
        long lit, dim;                // the rooms it shows (a bit per room) and those of them only seen on the map
    }
    private Plan plan;                // the mini map's (Link's heights; null: none tried yet)
    private boolean planLoading;
    // the full map's and the + menu's (a floor in their pictures; zoomed in: a window, finer), the last few kept
    private final Map<String, Plan> plans = new java.util.LinkedHashMap<String, Plan>(16, 0.75f, true) {
        @Override protected boolean removeEldestEntry(Map.Entry<String, Plan> e) { return size() > 8; }
    };
    private final java.util.Set<String> plansPending = new java.util.HashSet<>();
    private Plan chartPlan;           // the full map's last whole floor (its box)
    // the dungeon rooms' pictures redrawn (Redrawn; built on HD_WORK, the HD one drawn until then), the last few kept
    private final Map<String, Bitmap> roomsRedrawn = new java.util.LinkedHashMap<String, Bitmap>(16, 0.75f, true) {
        @Override protected boolean removeEldestEntry(Map.Entry<String, Bitmap> e) { return size() > 48; }
    };
    private final java.util.Set<String> roomsPending = new java.util.HashSet<>();
    private boolean loading;          // the stage's data being read
    private int gen;                  // a new stage drops the work for the old one
    private final java.util.concurrent.ExecutorService worker = java.util.concurrent.Executors.newSingleThreadExecutor();

    /** the stage Link is in, its data read (again once it changed): false while not read */
    private boolean stageReady() {
        String now = Native.miniMapStage();
        if (now.isEmpty()) return false;
        String dir = MainActivity.instance != null ? MainActivity.instance.gameDir() : null;
        if (dir == null) return false;
        if (!now.equals(stage)) {
            stage = now;
            dungeon = MiniMapDungeons.of(now);
            info = null;
            marks = null;
            plan = chartPlan = null;
            plans.clear();
            plansPending.clear();
            fine = null;
            finePending = null;
            planLoading = false;
            loading = true;
            int g = ++gen;
            worker.execute(() -> {
                float[] i = Native.miniMapStageInfo(dir, now), mk = Native.miniMapMarks(dir, now);
                MiniMapDungeons.Dungeon d = MiniMapDungeons.of(now);
                if (i != null && d != null && !d.mapStage.equals(now)) {
                    // the pictures placed by the other stage's DMAP (this stage's floors stay)
                    float[] o = Native.miniMapStageInfo(dir, d.mapStage);
                    if (o != null) System.arraycopy(o, 0, i, 0, 3);
                }
                view.post(() -> {
                    if (g != gen) return;
                    info = i != null ? i : new float[] {0, 0, 0};
                    marks = mk;
                    loading = false;
                    repaint();
                });
            });
            return false;
        }
        return info != null && !loading;
    }

    /** the plan's look for the pictures now set (terrain, redraw) */
    private int planStyle() { return terrain ? PLAN_TERRAIN : redraw ? PLAN_MASK : PLAN_CLASSIC; }

    /** the rooms visited (a bit per room) */
    private static long visitedRooms(float[] s) {
        long lit = 0;
        for (int r = 0; r < 64; r++)
            if (((int) s[64 + r / 16] >> (r % 16) & 1) != 0) lit |= 1L << r;
        return lit;
    }

    /** poll: the stage changed, or Link left the plan's heights: (re)load */
    private void indoors(float[] s) {
        if (!stageReady() || planLoading) return;
        // a dungeon with its map: its pictures, with Terrain its ground (the rooms visited, those only on the map grey)
        int style = planStyle();
        long lit = -1L, dim = 0;
        if (hasDungeonMap()) {
            if (style != PLAN_TERRAIN) return;
            lit = visitedRooms(s);
            dim = ((int) s[63] & 1) != 0 ? ~lit : 0;
        }
        boolean same = plan != null && style == plan.style && lit == plan.lit && dim == plan.dim;
        // the plan's heights: the floor Link is on (FLOR), else a band around him, made again once he leaves it
        float y = s[61], from, to;
        int f = floorIndex(y);
        if (f >= 0) {
            from = f == 0 ? -1e9f : info[3 + 2 * f];
            to = 3 + 2 * (f + 1) < info.length ? info[3 + 2 * (f + 1)] : 1e9f;
            if (same && plan.y == from) return;
        } else {
            if (same && Math.abs(y - plan.y) < PLAN_MOVE) return;
            from = y - PLAN_BELOW;
            to = y + PLAN_ABOVE;
        }
        planLoading = true;
        int g = gen;
        loadPlan(style, from, to, f >= 0 ? from : y, lit, dim, null, 0, true, pl -> {
            if (g != gen) return;
            planLoading = false;
            plan = pl;
            repaint();
        });
    }

    /** the stage's plan made on the worker, handed to done on the main thread (window {x0, z0, x1, z1}: only that part, at
     *  fineUpp units a pixel; lines: its see-through outline made too) */
    private void loadPlan(int style, float from, float to, float y, long lit, long dim, float[] window, float fineUpp,
                          boolean withLines, java.util.function.Consumer<Plan> done) {
        String dir = MainActivity.instance != null ? MainActivity.instance.gameDir() : null, now = stage;
        if (dir == null) return;
        worker.execute(() -> {
            Plan pl = new Plan();
            pl.style = style;
            pl.from = from;
            pl.to = to;
            pl.y = y;
            pl.lit = lit;
            pl.dim = dim;
            int[] p = Native.miniMapPlan(dir, now, from, to, style, lit, dim, window, fineUpp);
            Bitmap b = p == null ? null : Bitmap.createBitmap(p, 5, p[0], p[0], p[1], Bitmap.Config.ARGB_8888);
            if (b != null && style == PLAN_MASK) {
                // drawn anew: the room colours, an edge about 18 units wide
                float unit = 18f / Float.intBitsToFloat(p[4]);
                if (withLines) pl.lines = redraw(b, true, 0xD0000000 | ROOM_BLACK, 0xFF000000 | ROOM_WHITE, LINE, unit, false, true, 600);
                b = redraw(b, false, 0xD0000000 | ROOM_BLACK, 0xFF000000 | ROOM_WHITE, LINE, unit, false, true, 600);
            }
            pl.img = b;
            if (p != null) {
                float x0 = Float.intBitsToFloat(p[2]), z0 = Float.intBitsToFloat(p[3]), upp = Float.intBitsToFloat(p[4]);
                pl.rect.set(x0, z0, x0 + p[0] * upp, z0 + p[1] * upp);
            }
            view.post(() -> done.accept(pl));
        });
    }

    /** the plan of floor fi (FLOR index; -1: a band around Link) in the pictures set now (window: only that part, at
     *  fineUpp units a pixel), from the cache, else made on the worker (null meanwhile; also in a dungeon with its map
     *  without Terrain: it has none) */
    private Plan floorPlan(int fi, float[] window, float fineUpp) {
        int style = planStyle();
        long lit = -1L, dim = 0;
        if (hasDungeonMap()) {
            if (style != PLAN_TERRAIN) return null;
            lit = visitedRooms(st);
            dim = ((int) st[63] & 1) != 0 ? ~lit : 0;
        }
        float from, to;
        if (fi >= 0) {
            from = fi == 0 ? -1e9f : info[3 + 2 * fi];
            to = 3 + 2 * (fi + 1) < info.length ? info[3 + 2 * (fi + 1)] : 1e9f;
        } else {
            // (a band around Link, steady while he stands: the game is paused)
            from = st[61] - PLAN_BELOW;
            to = st[61] + PLAN_ABOVE;
        }
        // (the mini map's own when it is that one)
        Plan mp = plan;
        if (window == null && mp != null && mp.style == style && mp.from == from && mp.to == to && mp.lit == lit && mp.dim == dim)
            return mp;
        String key = style + "/" + from + "/" + to + "/" + lit + "/" + dim
                + (window == null ? "" : "/" + window[0] + "," + window[1] + "," + window[2] + "," + window[3] + "@" + fineUpp);
        lastPlanKey = key;
        Plan pl = plans.get(key);
        if (pl != null || !plansPending.add(key)) return pl;
        int g = gen;
        loadPlan(style, from, to, fi >= 0 ? from : st[61], lit, dim, window, fineUpp, false, made -> {
            if (g != gen) return;
            plansPending.remove(key);
            plans.put(key, made);
            repaint();
        });
        return null;
    }

    /** two plans of the same floor, rooms and look */
    private static boolean sameKind(Plan a, Plan b) {
        return a.style == b.style && a.from == b.from && a.to == b.to && a.lit == b.lit && a.dim == b.dim;
    }

    // the full map zoomed in: a finer plan of the part around the screen (fine), asked for once the whole floor's texels
    // grow past FINE_AT screen pixels
    private static final float FINE_AT = 1.6f;
    private Plan fine;
    private final float[] fineWant = new float[5];  // its window and scale
    private String finePending;                      // the one being made (its key in plans), its window and scale
    private final float[] pendingWant = new float[5];
    private String lastPlanKey;                      // floorPlan's last key

    /** the FLOR entry Link's height is on (-1: the stage has none) */
    private int floorIndex(float y) {
        if (info == null || info.length < 5) return -1;
        int f = 0;
        for (int i = 1; 3 + 2 * i < info.length; i++)
            if (y >= info[3 + 2 * i]) f = i;
        return f;
    }

    private boolean hasDungeonMap() { return dungeon != null && info != null && info[2] > 0; }

    /** world units across the mini map indoors: a dungeon map's DUNGEON_SPAN texels, a plan's room (2000..6000) */
    private float indoorSpan() {
        if (hasDungeonMap()) return DUNGEON_SPAN * MiniMapDungeons.TEXEL * info[2];
        if (plan != null && plan.img != null) return Math.max(2000f, Math.min(6000f, 0.8f * Math.max(plan.rect.width(), plan.rect.height())));
        return 3000f;
    }

    private boolean opened(int chest) {
        if (chest < 0 || chest > 31) return false;
        return ((int) st[68 + chest / 16] >> (chest % 16) & 1) != 0;
    }

    private boolean visited(int room) {
        if (room < 0 || room > 63) return false;
        int part = (int) st[64 + room / 16];
        return (part >> (room % 16) & 1) != 0;
    }

    /** the stage's floor fi (FLOR index; -1 none) with the plan pl (null: none) */
    private void drawIndoors(Canvas c, float lx, float lz, float k, float cx, float cy, int a, int fi, Plan pl) {
        drawIndoors(c, lx, lz, k, cx, cy, a, fi, pl, null);
    }

    /** ... with a finer plan of a part of it over it (fine; null: none) */
    private void drawIndoors(Canvas c, float lx, float lz, float k, float cx, float cy, int a, int fi, Plan pl, Plan fp) {
        if (hasDungeonMap()) {
            // the floor: its rooms visited, and with the dungeon's map all of them (a room not visited dim)
            int floor = fi >= 0 ? (int) info[4 + 2 * fi] : 0;
            boolean map = ((int) st[63] & 1) != 0;
            float u = MiniMapDungeons.TEXEL * info[2], ox = info[0], oz = info[1];
            // Terrain: the floor's ground in this stage's own world (no turn), instead of the pictures
            boolean ground = terrain && pl != null && pl.img != null && pl.style == PLAN_TERRAIN
                    && pl.from == (fi <= 0 ? -1e9f : info[3 + 2 * fi]);
            if (ground) drawPlans(c, pl, fp, lx, lz, k, cx, cy, a);
            // a dungeon placed by another stage's world turned: Link there, the map turned back (marks are this stage's)
            float wlx = lx, wlz = lz;
            if (dungeon.turn != 0) {
                double t = Math.toRadians(dungeon.turn);
                float cos = (float) Math.cos(t), sin = (float) Math.sin(t);
                lx = cos * wlx + sin * wlz;
                lz = -sin * wlx + cos * wlz;
                c.save();
                c.rotate(dungeon.turn, cx, cy);
                innerTurn = dungeon.turn;
            }
            for (MiniMapDungeons.Pic p : dungeon.pics) {
                if (ground || p.floor != floor) continue;
                boolean seen = p.room >= 0 && visited(p.room);
                if (!seen && !map) continue;
                boolean lit = seen || p.room < 0;
                Bitmap b = redraw ? roomRedrawn(p.texture, lit) : null;
                if (b == null)
                    b = outlines ? outline(p.texture, lit ? LINE : LINE_DIM, 2, hd)
                            : lit ? tinted(p.texture, ROOM_BLACK, ROOM_WHITE, ROOM_ALPHA, hd) : tinted(p.texture, DIM_BLACK, DIM_WHITE, DIM_ALPHA, hd);
                if (b == null) continue;
                float wx = ox + (p.x - p.w / 2) * u, wz = oz - (p.y + p.h / 2) * u;
                sq.set(cx + (wx - lx) * k, cy + (wz - lz) * k, cx + (wx + p.w * u - lx) * k, cy + (wz + p.h * u - lz) * k);
                if (!RectF.intersects(sq, ext)) continue;
                bmp.setAlpha(a);
                c.drawBitmap(b, null, sq, bmp);
            }
            // stairs in the rooms shown
            Bitmap stair = tinted("MapFloorUpDown_00^t", 0x000000, 0xFFFFFF, 0xFF);
            if (stair != null)
                for (MiniMapDungeons.Stair s : dungeon.stairs) {
                    if (s.floor != floor || !(map || s.room >= 0 && visited(s.room))) continue;
                    float px = cx + (ox + s.x * u - lx) * k, py = cy + (oz - s.y * u - lz) * k;
                    mark(c, stair, px, py, 0.1f, s.up ? 0 : 180, 0.85f * a);
                }
            if (dungeon.turn != 0) {
                c.restore();
                innerTurn = 0;
                lx = wlx;
                lz = wlz;
            }
            float y0 = fi >= 0 ? info[3 + 2 * fi] : -1e9f, y1 = fi >= 0 && 3 + 2 * (fi + 1) < info.length ? info[3 + 2 * (fi + 1)] : 1e9f;
            if (fi == 0) y0 = -1e9f;
            drawMarks(c, lx, lz, k, cx, cy, a, y0, y1, map, ((int) st[63] & 2) != 0);
            drawPartner(c, lx, lz, k, cx, cy, a, y0, y1);
            if (!noProgress) drawProgress(c, stage, lx, lz, k, cx, cy, a, y0, y1);
            return;
        }
        if (pl == null || pl.img == null) return;
        drawPlans(c, pl, fp, lx, lz, k, cx, cy, a);
        drawMarks(c, lx, lz, k, cx, cy, a, pl.from, pl.to, true, true);
        drawPartner(c, lx, lz, k, cx, cy, a, pl.from, pl.to);
        if (!noProgress) drawProgress(c, stage, lx, lz, k, cx, cy, a, pl.from, pl.to);
    }

    /** a plan and a finer one of a part of it (when it is of the same kind) over it, the plan left out under it */
    private void drawPlans(Canvas c, Plan pl, Plan fp, float lx, float lz, float k, float cx, float cy, int a) {
        if (fp == null || fp.img == null || !sameKind(fp, pl)) {
            drawPlan(c, pl, lx, lz, k, cx, cy, a);
            return;
        }
        sq.set(cx + (fp.rect.left - lx) * k, cy + (fp.rect.top - lz) * k, cx + (fp.rect.right - lx) * k, cy + (fp.rect.bottom - lz) * k);
        c.save();
        c.clipOutRect(sq);
        drawPlan(c, pl, lx, lz, k, cx, cy, a);
        c.restore();
        drawPlan(c, fp, lx, lz, k, cx, cy, a);
    }

    /** the collision plan at its place (see-through: its outline; Terrain's as it is) */
    private void drawPlan(Canvas c, Plan pl, float lx, float lz, float k, float cx, float cy, int a) {
        sq.set(cx + (pl.rect.left - lx) * k, cy + (pl.rect.top - lz) * k, cx + (pl.rect.right - lx) * k, cy + (pl.rect.bottom - lz) * k);
        bmp.setAlpha(a);
        boolean lines = outlines && pl.style != PLAN_TERRAIN;
        if (lines && pl.lines == null) pl.lines = outline(pl.img, LINE, 2);
        c.drawBitmap(lines ? pl.lines : pl.img, null, sq, bmp);
    }

    /** a dungeon room's picture drawn anew (redraw: its shape in the room colours, visited or only on the map; see-through
     *  its line), or null while that is being built in the background */
    private Bitmap roomRedrawn(String texture, boolean lit) {
        String key = texture + (lit ? "#lit" : "#dim") + (outlines ? "#lines" : "");
        Bitmap b = roomsRedrawn.get(key);
        if (b != null) return b;
        Bitmap orig = GameIcons.get(view.getContext(), texture, this::repaint);
        if (orig == null || !roomsPending.add(key)) return null;
        boolean lines = outlines;
        HD_WORK.execute(() -> {
            Bitmap s = lit ? redraw(orig, lines, ROOM_ALPHA << 24 | ROOM_BLACK, 0xFF000000 | ROOM_WHITE, LINE, 0.6f, false, true, 256)
                    : redraw(orig, lines, DIM_ALPHA << 24 | DIM_BLACK, 0x80000000 | DIM_WHITE, LINE_DIM, 0.6f, false, true, 256);
            view.post(() -> {
                roomsPending.remove(key);
                roomsRedrawn.put(key, s);
                repaint();
            });
        });
        return null;
    }

    /** chests and doors between the heights y0..y1 (a little slack below: a chest stands on the floor); in a dungeon a
     *  door once a room beside it shows (all with the map), chests with the compass */
    private void drawMarks(Canvas c, float lx, float lz, float k, float cx, float cy, int a, float y0, float y1, boolean map, boolean compass) {
        float[] mk = marks;
        if (mk == null) return;
        Bitmap door = tinted("MapDoor_00^t", 0x333333, 0x808080, 0xFF), chest = tinted("MapTreasure_00^t", 0x211400, 0xFFFF00, 0xFF);
        for (int i = 0; i + 6 < mk.length; i += 7) {
            float y = mk[i + 2];
            if (y < y0 - 50 || y >= y1) continue;
            float px = cx + (mk[i + 1] - lx) * k, py = cy + (mk[i + 3] - lz) * k;
            if (!ext.contains(px, py)) continue;
            if (mk[i] == 1) {
                int r1 = (int) mk[i + 5], r2 = (int) mk[i + 6];
                if (door == null || !(map || visited(r1) || visited(r2))) continue;
                // the bar lies across the doorway: a door turned by ay spans world (cos ay, -sin ay) on x / z
                c.save();
                c.rotate(-(float) Math.toDegrees(mk[i + 4]), px, py);
                float w = shownSide * 0.11f, h = w * door.getHeight() / door.getWidth();
                sq.set(px - w / 2, py - h / 2, px + w / 2, py + h / 2);
                bmp.setAlpha(a);
                c.drawBitmap(door, null, sq, bmp);
                c.restore();
            } else if (chest != null && compass) {
                mark(c, chest, px, py, 0.085f, 0, opened((int) mk[i + 4]) ? 0.35f * a : a);
            }
        }
    }

    /** an icon at (px, py), upright on the turned map, `size` of the map's side wide, turned by `deg` */
    private void mark(Canvas c, Bitmap b, float px, float py, float size, float deg, float alpha) {
        float w = shownSide * size, h = w * b.getHeight() / b.getWidth();
        upright(c, px, py);
        c.rotate(deg, px, py);
        sq.set(px - w / 2, py - h / 2, px + w / 2, py + h / 2);
        bmp.setAlpha(Math.round(alpha));
        c.drawBitmap(b, null, sq, bmp);
        c.restore();
    }

    private Bitmap tinted(String texture, int black, int white) { return tinted(texture, black, white, 0xFF, false); }
    private Bitmap tinted(String texture, int black, int white, int whiteAlpha) { return tinted(texture, black, white, whiteAlpha, false); }

    /** a texture of the layout as its material draws it: grey levels between the black and the white colour, the
     *  texture's alpha times the white colour's (the black one's 0 in these materials; GameIcons' PNGs are grey for
     *  one-channel textures), or null while not built; sharp: from its enlarged texture (sharpened) */
    private Bitmap tinted(String texture, int black, int white, int whiteAlpha, boolean sharp) {
        String key = texture + "#" + Integer.toHexString(black) + Integer.toHexString(white) + "/" + whiteAlpha;
        Bitmap b = tinted.get(key + (sharp ? "@hd" : ""));
        if (b != null) return b;
        Bitmap orig = GameIcons.get(view.getContext(), texture, this::repaint);
        if (orig == null) return null;
        Bitmap src = sharp ? sharpened(texture, orig) : orig;
        if (src != orig) key += "@hd";
        else if (sharp && (b = tinted.get(key)) != null) return b;  // (the HD one still being built)
        b = tint(src, black, white, whiteAlpha);
        tinted.put(key, b);
        return b;
    }

    /** the texture enlarged with crisp edges (sharpen), or the original while that is being built in the background */
    private Bitmap sharpened(String texture, Bitmap orig) {
        Bitmap b = hdSrc.get(texture);
        if (b != null) return b;
        if (hdPending.add(texture))
            HD_WORK.execute(() -> {
                Bitmap s = sharpen(orig);
                view.post(() -> {
                    hdPending.remove(texture);
                    hdSrc.put(texture, s);
                    repaint();
                });
            });
        return orig;
    }

    /** src (grey + alpha map art) enlarged to about 320 texels across (2x..4x; already that big: as it is): Catmull-Rom
     *  on premultiplied colour, then the alpha's edge steepened to about a texel of the result and the grey levels
     *  spread a little, so the islands' and rooms' ink edges stay crisp instead of blurring when the map shows them
     *  several screen pixels per texel */
    static Bitmap sharpen(Bitmap src) {
        int w = src.getWidth(), h = src.getHeight();
        int f = Math.max(1, Math.min(4, Math.round(320f / Math.max(w, h))));
        if (f == 1) return src;
        int[] in = new int[w * h];
        src.getPixels(in, 0, w, 0, 0, w, h);
        float[][] ch = new float[4][w * h];  // premultiplied r, g, b and a, 0..1
        for (int i = 0; i < in.length; i++) {
            float a = (in[i] >>> 24) / 255f;
            ch[3][i] = a;
            for (int c = 0; c < 3; c++) ch[c][i] = (in[i] >> (16 - 8 * c) & 0xFF) / 255f * a;
        }
        int W = w * f, H = h * f;
        float[] wx = new float[W * 4], wy = new float[H * 4];
        int[] ix = new int[W], iy = new int[H];
        catmullRom(W, f, wx, ix);
        catmullRom(H, f, wy, iy);
        int[] out = new int[W * H];
        float[] v = new float[4];
        for (int y = 0; y < H; y++)
            for (int x = 0; x < W; x++) {
                v[0] = v[1] = v[2] = v[3] = 0;
                for (int j = 0; j < 4; j++) {
                    int sy = Math.max(0, Math.min(h - 1, iy[y] + j - 1)) * w;
                    for (int i = 0; i < 4; i++) {
                        int s = sy + Math.max(0, Math.min(w - 1, ix[x] + i - 1));
                        float k = wy[y * 4 + j] * wx[x * 4 + i];
                        for (int c = 0; c < 4; c++) v[c] += ch[c][s] * k;
                    }
                }
                float a = Math.max(0, Math.min(1, v[3]));
                if (a <= 0) continue;
                float e = Math.max(0, Math.min(1, (a - 0.5f) * 3 + 0.5f));
                int A = Math.round(e * e * (3 - 2 * e) * 255), p = A << 24;
                for (int c = 0; c < 3; c++) {
                    float g = Math.max(0, Math.min(1, (v[c] / a - 0.5f) * 1.25f + 0.5f));
                    p |= Math.round(g * 255) << (16 - 8 * c);
                }
                out[y * W + x] = p;
            }
        return Bitmap.createBitmap(out, W, H, Bitmap.Config.ARGB_8888);
    }

    // the four Catmull-Rom weights and first source texel for each of n result texels, f result texels per source one
    private static void catmullRom(int n, int f, float[] wt, int[] first) {
        for (int o = 0; o < n; o++) {
            float s = (o + 0.5f) / f - 0.5f;
            int i0 = (int) Math.floor(s);
            float t = s - i0;
            first[o] = i0;
            wt[o * 4] = ((-t + 2) * t - 1) * t / 2;
            wt[o * 4 + 1] = ((3 * t - 5) * t * t + 2) / 2;
            wt[o * 4 + 2] = ((-3 * t + 4) * t + 1) * t / 2;
            wt[o * 4 + 3] = (t - 1) * t * t / 2;
        }
    }

    /** src's grey levels between the black and the white colour, its alpha times whiteAlpha (also ProgressMapView) */
    static Bitmap tint(Bitmap src, int black, int white, int whiteAlpha) {
        int w = src.getWidth(), h = src.getHeight();
        int[] px = new int[w * h];
        src.getPixels(px, 0, w, 0, 0, w, h);
        int br = black >> 16 & 0xFF, bg = black >> 8 & 0xFF, bb = black & 0xFF;
        int wr = white >> 16 & 0xFF, wg = white >> 8 & 0xFF, wb = white & 0xFF;
        for (int i = 0; i < px.length; i++) {
            int p = px[i], l = p >> 16 & 0xFF;
            px[i] = ((p >>> 24) * whiteAlpha / 255) << 24 | (br + (wr - br) * l / 255) << 16 | (bg + (wg - bg) * l / 255) << 8 | (bb + (wb - bb) * l / 255);
        }
        return Bitmap.createBitmap(px, w, h, Bitmap.Config.ARGB_8888);
    }
}
