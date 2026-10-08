package org.wwhdrecomp.app;

import android.graphics.Canvas;
import android.graphics.Paint;
import android.graphics.Path;
import android.graphics.RectF;

/**
 * Simple pictograms for the context buttons, drawn as paths in a unit square (-0.5..0.5) scaled to
 * the button: neutral symbols of our own (speech bubble, door, hand, ...), in the button's text
 * colour so they follow its style, opacity and pressed state. No image data.
 */
final class Pictograms {
    private Pictograms() {}

    private static final Paint line = new Paint(Paint.ANTI_ALIAS_FLAG), solid = new Paint(Paint.ANTI_ALIAS_FLAG);
    static {
        line.setStyle(Paint.Style.STROKE);
        line.setStrokeCap(Paint.Cap.ROUND);
        line.setStrokeJoin(Paint.Join.ROUND);
        solid.setStyle(Paint.Style.FILL);
    }
    private static final Path p = new Path();
    private static final RectF r = new RectF();
    private static float X, Y, S;  // centre and size of the current pictogram

    private static float x(float u) { return X + u * S; }
    private static float y(float v) { return Y + v * S; }

    /** the pictogram for a game action code (dActStts), or null (then the button keeps its text) */
    static String forAction(int code) {
        switch (code) {
            case 0x02: return "speak";
            case 0x01: case 0x0A: return "look";
            case 0x26: return "read";
            case 0x0B: return "open";
            case 0x04: return "lift";
            case 0x1B: case 0x11: return "hand";
            case 0x05: return "climb";
            case 0x06: case 0x16: return "letgo";
            case 0x0E: return "throw";
            case 0x08: case 0x09: return "drop";
            case 0x12: return "jump";
            case 0x0C: return "roll";  // the decompilation's "Attack": A while running rolls forward
            case 0x19: return "next";
            case 0x17: return "choose";
            case 0x27: return "cancel";
            case 0x1C: return "getin";
            case 0x1D: return "getout";
            case 0x10: return "sidle";
            case 0x13: return "stop";
            case 0x2F: return "swing";
            case 0x0F: return "crouch";
            case 0x36: return "defend";
            default: return null;
        }
    }

    // ---- coloured pictograms: flat colours, a thick dark outline and a light highlight, so they sit
    // well next to the game's own icons (generic sticker style, drawn here)
    private static final int INK = 0xFF1B2A4A, WOOD = 0xFFB9783F, WOOD_D = 0xFF7A4A22, BRASS = 0xFFF2C14E,
            STEEL = 0xFFDCE6F0, BLUE = 0xFF2F6FD8, GREEN = 0xFF4CB848, RED = 0xFFE0473A, CREAM = 0xFFF6EBD0;
    private static final Paint fillC = new Paint(Paint.ANTI_ALIAS_FLAG), inkC = new Paint(Paint.ANTI_ALIAS_FLAG);
    static {
        fillC.setStyle(Paint.Style.FILL);
        inkC.setStyle(Paint.Style.STROKE);
        inkC.setStrokeCap(Paint.Cap.ROUND);
        inkC.setStrokeJoin(Paint.Join.ROUND);
    }
    private static int alpha = 255;

    private static int a(int col) { return (col & 0xFFFFFF) | (alpha * (col >>> 24) / 255) << 24; }

    // a filled path with the outline
    private static void shape(Canvas c, Path path, int col) {
        fillC.setColor(a(col));
        c.drawPath(path, fillC);
        c.drawPath(path, inkC);
    }

    private static Path rect(float u0, float v0, float u1, float v1, float rad) {
        Path q = new Path();
        q.addRoundRect(new RectF(x(u0), y(v0), x(u1), y(v1)), rad * S, rad * S, Path.Direction.CW);
        return q;
    }

    // a fat arrow (filled, outlined) from (u0,v0) to (u1,v1)
    private static void fatArrow(Canvas c, float u0, float v0, float u1, float v1, int col) {
        float dx = u1 - u0, dy = v1 - v0, len = (float) Math.hypot(dx, dy);
        dx /= len;
        dy /= len;
        float nx = -dy, ny = dx, w = 0.09f, hw = 0.2f, hl = 0.22f;
        float bu = u1 - dx * hl, bv = v1 - dy * hl;
        Path q = new Path();
        q.moveTo(x(u0 + nx * w), y(v0 + ny * w));
        q.lineTo(x(bu + nx * w), y(bv + ny * w));
        q.lineTo(x(bu + nx * hw), y(bv + ny * hw));
        q.lineTo(x(u1), y(v1));
        q.lineTo(x(bu - nx * hw), y(bv - ny * hw));
        q.lineTo(x(bu - nx * w), y(bv - ny * w));
        q.lineTo(x(u0 - nx * w), y(v0 - ny * w));
        q.close();
        shape(c, q, col);
    }

    /** the coloured version of a pictogram, if there is one */
    private static boolean drawColoured(Canvas c, String name) {
        inkC.setColor(a(INK));
        inkC.setStrokeWidth(Math.max(2f, S * 0.065f));
        switch (name) {
            case "open": {  // a wooden door with planks and a brass knob, arched top
                Path d = new Path();
                d.moveTo(x(-0.3f), y(0.42f));
                d.lineTo(x(-0.3f), y(-0.18f));
                d.quadTo(x(-0.3f), y(-0.44f), x(0), y(-0.44f));
                d.quadTo(x(0.3f), y(-0.44f), x(0.3f), y(-0.18f));
                d.lineTo(x(0.3f), y(0.42f));
                d.close();
                shape(c, d, WOOD);
                inkC.setStrokeWidth(Math.max(1.5f, S * 0.035f));
                inkC.setColor(a(WOOD_D));
                c.drawLine(x(-0.1f), y(-0.4f), x(-0.1f), y(0.4f), inkC);
                c.drawLine(x(0.1f), y(-0.4f), x(0.1f), y(0.4f), inkC);
                inkC.setColor(a(INK));
                inkC.setStrokeWidth(Math.max(2f, S * 0.05f));
                Path k = new Path();
                k.addCircle(x(0.18f), y(0.06f), S * 0.06f, Path.Direction.CW);
                shape(c, k, BRASS);
                return true;
            }
            case "climb": {  // a wooden ladder and an arrow up
                inkC.setStrokeWidth(Math.max(2f, S * 0.05f));
                shape(c, rect(-0.34f, -0.36f, -0.24f, 0.44f, 0.04f), WOOD);
                shape(c, rect(0.04f, -0.36f, 0.14f, 0.44f, 0.04f), WOOD);
                for (int i = 0; i < 4; i++) shape(c, rect(-0.24f, -0.26f + i * 0.2f, 0.04f, -0.19f + i * 0.2f, 0.02f), WOOD);
                fatArrow(c, 0.34f, 0.3f, 0.34f, -0.42f, GREEN);
                return true;
            }
            case "drop": {  // put away: a sword sliding into a blue scabbard
                Path sc = new Path();  // the scabbard, diagonal, tip down left
                sc.moveTo(x(-0.4f), y(0.42f));
                sc.lineTo(x(-0.44f), y(0.32f));
                sc.lineTo(x(0.08f), y(-0.2f));
                sc.lineTo(x(0.2f), y(-0.08f));
                sc.lineTo(x(-0.32f), y(0.44f));
                sc.close();
                shape(c, sc, BLUE);
                Path guard = new Path();  // the hilt above the scabbard's mouth
                guard.moveTo(x(0.02f), y(-0.3f));
                guard.lineTo(x(0.3f), y(-0.02f));
                guard.lineTo(x(0.36f), y(-0.08f));
                guard.lineTo(x(0.08f), y(-0.36f));
                guard.close();
                shape(c, guard, BRASS);
                Path grip = new Path();
                grip.moveTo(x(0.2f), y(-0.2f));
                grip.lineTo(x(0.36f), y(-0.36f));
                grip.lineTo(x(0.42f), y(-0.3f));
                grip.lineTo(x(0.26f), y(-0.14f));
                grip.close();
                shape(c, grip, WOOD_D);
                fatArrow(c, -0.1f, -0.42f, -0.36f, -0.16f, GREEN);  // into the scabbard
                return true;
            }
            case "lift": {  // a pot lifted: arrow up over a clay pot
                Path pot = new Path();
                pot.moveTo(x(-0.2f), y(0.02f));
                pot.lineTo(x(0.2f), y(0.02f));
                pot.quadTo(x(0.4f), y(0.24f), x(0.2f), y(0.44f));
                pot.lineTo(x(-0.2f), y(0.44f));
                pot.quadTo(x(-0.4f), y(0.24f), x(-0.2f), y(0.02f));
                pot.close();
                shape(c, pot, 0xFFC8743A);
                fatArrow(c, 0, -0.02f, 0, -0.44f, GREEN);
                return true;
            }
            case "hand": {  // pick up / grab: an open hand
                Path h = new Path();
                h.moveTo(x(-0.22f), y(0.42f));
                h.lineTo(x(-0.22f), y(0.06f));
                h.lineTo(x(-0.38f), y(-0.06f));
                h.lineTo(x(-0.3f), y(-0.14f));
                h.lineTo(x(-0.16f), y(-0.04f));
                h.lineTo(x(-0.16f), y(-0.36f));
                h.lineTo(x(-0.06f), y(-0.36f));
                h.lineTo(x(-0.06f), y(-0.1f));
                h.lineTo(x(-0.02f), y(-0.42f));
                h.lineTo(x(0.08f), y(-0.42f));
                h.lineTo(x(0.08f), y(-0.1f));
                h.lineTo(x(0.12f), y(-0.36f));
                h.lineTo(x(0.22f), y(-0.36f));
                h.lineTo(x(0.22f), y(-0.04f));
                h.lineTo(x(0.26f), y(-0.24f));
                h.lineTo(x(0.36f), y(-0.24f));
                h.lineTo(x(0.34f), y(0.18f));
                h.quadTo(x(0.3f), y(0.42f), x(0.1f), y(0.42f));
                h.close();
                shape(c, h, CREAM);
                return true;
            }
            case "crouch": {  // a ducking figure on the ground, chevrons pointing down above it
                inkC.setStrokeWidth(Math.max(2f, S * 0.05f));
                shape(c, rect(-0.42f, 0.36f, 0.42f, 0.44f, 0.04f), GREEN);  // ground
                // the figure: thick limbs as outlined strokes (ink under colour)
                Path body = new Path();
                body.moveTo(x(-0.02f), y(-0.06f));   // neck
                body.lineTo(x(0.1f), y(0.14f));      // back, bent forward
                body.lineTo(x(-0.12f), y(0.2f));     // thigh to the knee
                body.lineTo(x(0.04f), y(0.34f));     // shin to the foot
                Path arm = new Path();
                arm.moveTo(x(0.02f), y(0.0f));
                arm.lineTo(x(-0.2f), y(0.08f));
                for (Path limb : new Path[] {body, arm}) {
                    inkC.setStrokeWidth(Math.max(5f, S * 0.19f));
                    c.drawPath(limb, inkC);
                    inkC.setStrokeWidth(Math.max(3f, S * 0.12f));
                    inkC.setColor(a(BLUE));
                    c.drawPath(limb, inkC);
                    inkC.setColor(a(INK));
                }
                inkC.setStrokeWidth(Math.max(2f, S * 0.05f));
                Path head = new Path();
                head.addCircle(x(-0.06f), y(-0.2f), S * 0.11f, Path.Direction.CW);
                shape(c, head, CREAM);
                // two chevrons pointing down, above right
                inkC.setStrokeWidth(Math.max(3f, S * 0.11f));
                for (int i = 0; i < 2; i++) {
                    Path ch = new Path();
                    float v = -0.42f + i * 0.14f;
                    ch.moveTo(x(0.16f), y(v));
                    ch.lineTo(x(0.28f), y(v + 0.1f));
                    ch.lineTo(x(0.4f), y(v));
                    c.drawPath(ch, inkC);
                    inkC.setStrokeWidth(Math.max(2f, S * 0.06f));
                    inkC.setColor(a(GREEN));
                    c.drawPath(ch, inkC);
                    inkC.setColor(a(INK));
                    inkC.setStrokeWidth(Math.max(3f, S * 0.11f));
                }
                return true;
            }
            case "letgo":
                fatArrow(c, 0, -0.42f, 0, 0.42f, RED);
                return true;
            case "roll": {  // a ball with an arrow curving around it, and speed lines behind
                inkC.setStrokeWidth(Math.max(2f, S * 0.05f));
                Path ball = new Path();
                ball.addCircle(x(0.06f), y(0.08f), S * 0.2f, Path.Direction.CW);
                shape(c, ball, GREEN);
                // the curved arrow: a thick arc over the ball with a head pointing forward-down
                RectF arcR = new RectF(x(-0.28f), y(-0.26f), x(0.4f), y(0.42f));
                inkC.setStrokeWidth(Math.max(4f, S * 0.17f));
                c.drawArc(arcR, 160, 160, false, inkC);
                inkC.setStrokeWidth(Math.max(2f, S * 0.09f));
                inkC.setColor(a(BLUE));
                c.drawArc(arcR, 160, 160, false, inkC);
                inkC.setColor(a(INK));
                inkC.setStrokeWidth(Math.max(2f, S * 0.065f));
                fatArrow(c, 0.3f, -0.12f, 0.42f, 0.12f, BLUE);
                // speed lines
                inkC.setStrokeWidth(Math.max(1.5f, S * 0.05f));
                for (int i = 0; i < 3; i++) c.drawLine(x(-0.44f), y(0.0f + i * 0.13f), x(-0.24f), y(0.0f + i * 0.13f), inkC);
                return true;
            }
            case "jump": {
                shape(c, rect(-0.42f, 0.32f, 0.42f, 0.42f, 0.04f), GREEN);
                fatArrow(c, 0, 0.24f, 0, -0.44f, BLUE);
                return true;
            }
            case "throw": {  // an arrow curving up and over
                Path arc = new Path();
                inkC.setStrokeWidth(Math.max(4f, S * 0.2f));
                arc.moveTo(x(-0.36f), y(0.34f));
                arc.quadTo(x(-0.16f), y(-0.36f), x(0.16f), y(-0.12f));
                c.drawPath(arc, inkC);
                inkC.setStrokeWidth(Math.max(2f, S * 0.11f));
                inkC.setColor(a(BLUE));
                c.drawPath(arc, inkC);
                inkC.setColor(a(INK));
                inkC.setStrokeWidth(Math.max(2f, S * 0.065f));
                fatArrow(c, 0.06f, -0.2f, 0.4f, 0.16f, BLUE);
                return true;
            }
            default:
                return false;
        }
    }

    /** draws pictogram `name` centred at cx, cy within size s; false if there is no such pictogram */
    static boolean draw(Canvas c, String name, float cx, float cy, float s, int color, boolean on) {
        if (name == null) return false;
        X = cx;
        Y = cy;
        S = s;
        alpha = color >>> 24;
        if (drawColoured(c, name)) return true;
        line.setColor(color);
        solid.setColor(color);
        line.setStrokeWidth(Math.max(1.5f, s * 0.085f));
        p.reset();
        switch (name) {
            case "speak":  // speech bubble with three dots
                r.set(x(-0.42f), y(-0.36f), x(0.42f), y(0.2f));
                c.drawRoundRect(r, s * 0.14f, s * 0.14f, line);
                p.moveTo(x(-0.18f), y(0.2f));
                p.lineTo(x(-0.26f), y(0.4f));
                p.lineTo(x(0.02f), y(0.2f));
                c.drawPath(p, line);
                for (int i = -1; i <= 1; i++) c.drawCircle(x(i * 0.18f), y(-0.08f), s * 0.05f, solid);
                return true;
            case "look":  // magnifier
                c.drawCircle(x(-0.08f), y(-0.08f), s * 0.24f, line);
                c.drawLine(x(0.1f), y(0.1f), x(0.36f), y(0.36f), line);
                return true;
            case "read":  // open book
                p.moveTo(x(0), y(-0.26f));
                p.quadTo(x(-0.2f), y(-0.36f), x(-0.42f), y(-0.28f));
                p.lineTo(x(-0.42f), y(0.3f));
                p.quadTo(x(-0.2f), y(0.22f), x(0), y(0.32f));
                p.quadTo(x(0.2f), y(0.22f), x(0.42f), y(0.3f));
                p.lineTo(x(0.42f), y(-0.28f));
                p.quadTo(x(0.2f), y(-0.36f), x(0), y(-0.26f));
                p.lineTo(x(0), y(0.32f));
                c.drawPath(p, line);
                return true;
            case "open":  // door with handle
                r.set(x(-0.26f), y(-0.4f), x(0.26f), y(0.4f));
                c.drawRoundRect(r, s * 0.04f, s * 0.04f, line);
                c.drawCircle(x(0.12f), y(0.04f), s * 0.05f, solid);
                return true;
            case "lift":  // box with an arrow up
                r.set(x(-0.3f), y(0.04f), x(0.3f), y(0.4f));
                c.drawRect(r, line);
                arrow(c, 0, -0.06f, 0, -0.42f);
                return true;
            case "hand":  // open hand: palm and four fingers
                r.set(x(-0.26f), y(-0.02f), x(0.22f), y(0.4f));
                c.drawRoundRect(r, s * 0.12f, s * 0.12f, line);
                for (int i = 0; i < 4; i++) {
                    float fx = -0.18f + i * 0.12f;
                    c.drawLine(x(fx), y(0.0f), x(fx), y(-0.32f + (i == 0 || i == 3 ? 0.08f : 0)), line);
                }
                c.drawLine(x(-0.26f), y(0.16f), x(-0.4f), y(0.0f), line);  // thumb
                return true;
            case "climb":  // ladder
                c.drawLine(x(-0.2f), y(-0.42f), x(-0.2f), y(0.42f), line);
                c.drawLine(x(0.2f), y(-0.42f), x(0.2f), y(0.42f), line);
                for (int i = 0; i < 4; i++) c.drawLine(x(-0.2f), y(-0.3f + i * 0.2f), x(0.2f), y(-0.3f + i * 0.2f), line);
                return true;
            case "letgo":  // arrow down
                arrow(c, 0, -0.38f, 0, 0.38f);
                return true;
            case "throw":  // arc with an arrow head
                p.moveTo(x(-0.38f), y(0.3f));
                p.quadTo(x(-0.1f), y(-0.5f), x(0.3f), y(-0.04f));
                c.drawPath(p, line);
                head(c, 0.3f, -0.04f, 0.62f, 0.75f);
                return true;
            case "drop":  // arrow into a tray
                arrow(c, 0, -0.42f, 0, 0.12f);
                p.moveTo(x(-0.36f), y(0.06f));
                p.lineTo(x(-0.36f), y(0.36f));
                p.lineTo(x(0.36f), y(0.36f));
                p.lineTo(x(0.36f), y(0.06f));
                c.drawPath(p, line);
                return true;
            case "jump":  // arched arrow up
                p.moveTo(x(-0.36f), y(0.36f));
                p.quadTo(x(-0.3f), y(-0.3f), x(0.18f), y(-0.3f));
                c.drawPath(p, line);
                head(c, 0.18f, -0.3f, 1f, 0f);
                c.drawLine(x(-0.42f), y(0.4f), x(0.42f), y(0.4f), line);
                return true;
            case "attack":  // a plain straight sword, diagonal
                c.drawLine(x(-0.3f), y(0.3f), x(0.34f), y(-0.34f), line);
                c.drawLine(x(-0.36f), y(0.08f), x(-0.08f), y(0.36f), line);  // guard
                c.drawLine(x(-0.3f), y(0.3f), x(-0.42f), y(0.42f), line);    // grip
                return true;
            case "next":  // triangle pointing down
                p.moveTo(x(-0.3f), y(-0.18f));
                p.lineTo(x(0.3f), y(-0.18f));
                p.lineTo(x(0), y(0.26f));
                p.close();
                c.drawPath(p, solid);
                return true;
            case "choose":  // check mark
                p.moveTo(x(-0.34f), y(0.02f));
                p.lineTo(x(-0.1f), y(0.28f));
                p.lineTo(x(0.36f), y(-0.28f));
                c.drawPath(p, line);
                return true;
            case "cancel":  // cross
                c.drawLine(x(-0.28f), y(-0.28f), x(0.28f), y(0.28f), line);
                c.drawLine(x(0.28f), y(-0.28f), x(-0.28f), y(0.28f), line);
                return true;
            case "getin":  // arrow into a frame
            case "getout": {  // arrow out of a frame
                p.moveTo(x(0.04f), y(-0.36f));
                p.lineTo(x(0.38f), y(-0.36f));
                p.lineTo(x(0.38f), y(0.36f));
                p.lineTo(x(0.04f), y(0.36f));
                c.drawPath(p, line);
                if (name.equals("getin")) arrow(c, -0.42f, 0, 0.2f, 0);
                else arrow(c, 0.2f, 0, -0.42f, 0);
                return true;
            }
            case "sidle":  // arrows left and right
                arrow(c, 0, 0, -0.4f, 0);
                arrow(c, 0, 0, 0.4f, 0);
                return true;
            case "stop":  // square
                r.set(x(-0.24f), y(-0.24f), x(0.24f), y(0.24f));
                c.drawRect(r, solid);
                return true;
            case "swing":  // a rope hanging from a point, swung to the side
                c.drawCircle(x(-0.1f), y(-0.36f), s * 0.05f, solid);
                p.moveTo(x(-0.1f), y(-0.36f));
                p.quadTo(x(-0.18f), y(0.04f), x(0.26f), y(0.3f));
                c.drawPath(p, line);
                c.drawCircle(x(0.26f), y(0.3f), s * 0.08f, line);
                return true;
            case "crouch":  // chevron down over a floor line
                p.moveTo(x(-0.3f), y(-0.24f));
                p.lineTo(x(0), y(0.06f));
                p.lineTo(x(0.3f), y(-0.24f));
                c.drawPath(p, line);
                c.drawLine(x(-0.38f), y(0.3f), x(0.38f), y(0.3f), line);
                return true;
            case "defend":  // shield outline
                p.moveTo(x(0), y(-0.4f));
                p.lineTo(x(0.34f), y(-0.28f));
                p.quadTo(x(0.34f), y(0.18f), x(0), y(0.42f));
                p.quadTo(x(-0.34f), y(0.18f), x(-0.34f), y(-0.28f));
                p.close();
                c.drawPath(p, line);
                return true;
            case "target": {  // crosshair; the centre filled while latched
                c.drawCircle(x(0), y(0), s * 0.28f, line);
                c.drawLine(x(0), y(-0.44f), x(0), y(-0.16f), line);
                c.drawLine(x(0), y(0.16f), x(0), y(0.44f), line);
                c.drawLine(x(-0.44f), y(0), x(-0.16f), y(0), line);
                c.drawLine(x(0.16f), y(0), x(0.44f), y(0), line);
                if (on) c.drawCircle(x(0), y(0), s * 0.09f, solid);
                return true;
            }
            case "eye":  // first person
                p.moveTo(x(-0.42f), y(0));
                p.quadTo(x(0), y(-0.4f), x(0.42f), y(0));
                p.quadTo(x(0), y(0.4f), x(-0.42f), y(0));
                p.close();
                c.drawPath(p, line);
                c.drawCircle(x(0), y(0), s * 0.11f, solid);
                return true;
            case "menu":  // 2 x 2 grid
                for (int i = 0; i < 4; i++) {
                    float u = (i % 2 == 0 ? -0.3f : 0.06f), v = (i < 2 ? -0.3f : 0.06f);
                    r.set(x(u), y(v), x(u + 0.24f), y(v + 0.24f));
                    c.drawRoundRect(r, s * 0.04f, s * 0.04f, line);
                }
                return true;
            case "swap":  // arrows both ways (swap the screens)
                arrow(c, -0.36f, -0.14f, 0.36f, -0.14f);
                arrow(c, 0.36f, 0.16f, -0.36f, 0.16f);
                return true;
            case "note": {  // music note (the Wind Waker)
                c.drawCircle(x(-0.14f), y(0.26f), s * 0.12f, solid);
                c.drawLine(x(-0.03f), y(0.26f), x(-0.03f), y(-0.38f), line);
                p.moveTo(x(-0.03f), y(-0.38f));
                p.quadTo(x(0.2f), y(-0.3f), x(0.3f), y(-0.12f));
                c.drawPath(p, line);
                return true;
            }
            case "cannon":  // wheel and barrel
                c.drawCircle(x(-0.1f), y(0.2f), s * 0.16f, line);
                p.moveTo(x(-0.24f), y(0.02f));
                p.lineTo(x(0.3f), y(-0.3f));
                p.lineTo(x(0.4f), y(-0.14f));
                p.lineTo(x(-0.12f), y(0.16f));
                p.close();
                c.drawPath(p, line);
                return true;
            case "hook":  // a hook on a rope
                c.drawLine(x(0), y(-0.42f), x(0), y(0.1f), line);
                p.moveTo(x(0), y(0.1f));
                p.quadTo(x(0), y(0.36f), x(-0.2f), y(0.3f));
                p.quadTo(x(-0.32f), y(0.24f), x(-0.3f), y(0.08f));
                c.drawPath(p, line);
                return true;
            default:
                return false;
        }
    }

    // a line from (u0,v0) to (u1,v1) with a head at the end
    private static void arrow(Canvas c, float u0, float v0, float u1, float v1) {
        c.drawLine(x(u0), y(v0), x(u1), y(v1), line);
        float dx = u1 - u0, dy = v1 - v0, len = (float) Math.hypot(dx, dy);
        head(c, u1, v1, dx / len, dy / len);
    }

    // an arrow head at (u,v) pointing along (dx,dy)
    private static void head(Canvas c, float u, float v, float dx, float dy) {
        float len = (float) Math.hypot(dx, dy);
        dx /= len;
        dy /= len;
        float k = 0.16f;
        Path h = new Path();
        h.moveTo(x(u - dx * k - dy * k * 0.8f), y(v - dy * k + dx * k * 0.8f));
        h.lineTo(x(u), y(v));
        h.lineTo(x(u - dx * k + dy * k * 0.8f), y(v - dy * k - dx * k * 0.8f));
        c.drawPath(h, line);
    }
}
