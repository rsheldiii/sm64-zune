#ifndef ZUNE_OVERLAY_H
#define ZUNE_OVERLAY_H
// Touch-control overlay geometry: translucent N64-coloured discs with an outline ring and a
// stroked label (A, B, Z, R, START, C arrows), plus the floating stick. Triangles only, in
// landscape pixels (origin top-left); ZuneOverlayToClip turns them into rotated clip space.
// Platform-neutral so tests/overlay.cpp checks it on the host.
#include <math.h>
#include "zune_touch.h"

// How the controls are drawn; the values are those of ZUNE_TOUCH_BUTTONS in zune_settings.h.
// Touch input does not depend on it.
enum { ZUNE_BUTTONS_HIDDEN = 0, ZUNE_BUTTONS_OUTLINE = 1, ZUNE_BUTTONS_FILLED = 2 };

struct ZuneOverlayStyle {
    int buttons;     // ZUNE_BUTTONS_*
    float opacity;   // scales every alpha; 1 is full strength
};

struct ZuneOverlayVertex { float x, y, r, g, b, a; };

struct ZuneColor { float r, g, b; };

class ZuneOverlayBuilder {
public:
    ZuneOverlayBuilder(ZuneOverlayVertex *out, int capacity) : out_(out), capacity_(capacity), count_(0), overflow_(false) {}
    int Count() const { return count_; }
    bool Overflowed() const { return overflow_; }

    void Triangle(float x0, float y0, float x1, float y1, float x2, float y2, ZuneColor c, float a) {
        if (count_ + 3 > capacity_) { overflow_ = true; return; }
        Put(x0, y0, c, a); Put(x1, y1, c, a); Put(x2, y2, c, a);
    }
    void Quad(float x0, float y0, float x1, float y1, float x2, float y2, float x3, float y3, ZuneColor c, float a) {
        Triangle(x0, y0, x1, y1, x2, y2, c, a);
        Triangle(x0, y0, x2, y2, x3, y3, c, a);
    }
    void Disc(float cx, float cy, float radius, ZuneColor c, float a) {
        for (int i = 0; i < SEGMENTS; ++i)
            Triangle(cx, cy, cx + radius * Cos(i), cy + radius * Sin(i), cx + radius * Cos(i + 1), cy + radius * Sin(i + 1), c, a);
    }
    void Ring(float cx, float cy, float radius, float thickness, ZuneColor c, float a) {
        float inner = radius - thickness;
        for (int i = 0; i < SEGMENTS; ++i)
            Quad(cx + inner * Cos(i), cy + inner * Sin(i), cx + radius * Cos(i), cy + radius * Sin(i),
                 cx + radius * Cos(i + 1), cy + radius * Sin(i + 1), cx + inner * Cos(i + 1), cy + inner * Sin(i + 1), c, a);
    }
    // A stroke of the given thickness from (x0,y0) to (x1,y1), with square caps.
    void Line(float x0, float y0, float x1, float y1, float thickness, ZuneColor c, float a) {
        float dx = x1 - x0, dy = y1 - y0, length = sqrtf(dx * dx + dy * dy);
        if (length < 0.001f) return;
        float h = thickness * 0.5f, ux = dx / length * h, uy = dy / length * h;
        Quad(x0 - ux - uy, y0 - uy + ux, x1 + ux - uy, y1 + uy + ux, x1 + ux + uy, y1 + uy - ux, x0 - ux + uy, y0 - uy - ux, c, a);
    }
    // One stroked glyph in a box `width` x `height` with top-left (x, y). Returns false for
    // characters without strokes.
    bool Glyph(char ch, float x, float y, float width, float height, float thickness, ZuneColor c, float a) {
        const signed char *strokes = Strokes(ch);
        if (!strokes) return false;
        for (int i = 0; strokes[i] >= 0; i += 4)
            Line(x + strokes[i] * width / 4, y + strokes[i + 1] * height / 6,
                 x + strokes[i + 2] * width / 4, y + strokes[i + 3] * height / 6, thickness, c, a);
        return true;
    }
    // A text label centred on (cx, cy).
    void Text(const char *text, float cx, float cy, float height, float thickness, ZuneColor c, float a) {
        int n = 0;
        while (text[n]) ++n;
        float width = height * 0.6f, advance = width + height * 0.3f, total = n * advance - height * 0.3f;
        for (int i = 0; i < n; ++i) Glyph(text[i], cx - total / 2 + i * advance, cy - height / 2, width, height, thickness, c, a);
    }
    // A filled triangle pointing up (0), down (1), left (2) or right (3).
    void Arrow(float cx, float cy, float size, int direction, ZuneColor c, float a) {
        float s = size * 0.5f;
        switch (direction) {
        case 0: Triangle(cx, cy - s, cx + s, cy + s * 0.7f, cx - s, cy + s * 0.7f, c, a); break;
        case 1: Triangle(cx, cy + s, cx - s, cy - s * 0.7f, cx + s, cy - s * 0.7f, c, a); break;
        case 2: Triangle(cx - s, cy, cx + s * 0.7f, cy - s, cx + s * 0.7f, cy + s, c, a); break;
        default: Triangle(cx + s, cy, cx - s * 0.7f, cy + s, cx - s * 0.7f, cy - s, c, a); break;
        }
    }

    enum { SEGMENTS = 20 };

private:
    void Put(float x, float y, ZuneColor c, float a) {
        ZuneOverlayVertex &v = out_[count_++];
        v.x = x; v.y = y; v.r = c.r; v.g = c.g; v.b = c.b; v.a = a;
    }
    // The unit circle's SEGMENTS points, computed once. The stick is rebuilt every frame it is
    // held, and on the device each double cos/sin is a 13.7 us software call into coredll:
    // 320 of them per frame made up most of the present time.
    static const float *Circle() {
        static float points[2 * SEGMENTS];
        static bool ready;
        if (!ready) {
            for (int i = 0; i < SEGMENTS; ++i) {
                points[2 * i] = (float)cos(i * 6.283185307 / SEGMENTS);
                points[2 * i + 1] = (float)sin(i * 6.283185307 / SEGMENTS);
            }
            ready = true;
        }
        return points;
    }
    static float Cos(int i) { return Circle()[2 * (i % SEGMENTS)]; }
    static float Sin(int i) { return Circle()[2 * (i % SEGMENTS) + 1]; }
    // Strokes on a 4x6 grid: x0,y0,x1,y1 groups ending with -1.
    static const signed char *Strokes(char ch) {
        static const signed char A[] = {0,6,2,0, 2,0,4,6, 1,4,3,4, -1};
        static const signed char B[] = {0,0,0,6, 0,0,3,0, 3,0,4,1, 4,1,4,2, 4,2,3,3, 0,3,3,3, 3,3,4,4, 4,4,4,5, 4,5,3,6, 3,6,0,6, -1};
        static const signed char R[] = {0,0,0,6, 0,0,3,0, 3,0,4,1, 4,1,4,2, 4,2,3,3, 3,3,0,3, 2,3,4,6, -1};
        static const signed char S[] = {4,0,1,0, 1,0,0,1, 0,1,0,2, 0,2,1,3, 1,3,3,3, 3,3,4,4, 4,4,4,5, 4,5,3,6, 3,6,0,6, -1};
        static const signed char T[] = {0,0,4,0, 2,0,2,6, -1};
        static const signed char Z[] = {0,0,4,0, 4,0,0,6, 0,6,4,6, -1};
        switch (ch) {
        case 'A': return A;
        case 'B': return B;
        case 'R': return R;
        case 'S': return S;
        case 'T': return T;
        case 'Z': return Z;
        default: return 0;
        }
    }

    ZuneOverlayVertex *out_;
    int capacity_, count_;
    bool overflow_;
};

// N64 colours for each control.
inline ZuneColor ZuneButtonColor(unsigned short bits)
{
    ZuneColor blue = {0.25f, 0.45f, 1.0f}, green = {0.2f, 0.8f, 0.3f}, yellow = {1.0f, 0.82f, 0.1f};
    ZuneColor red = {0.95f, 0.25f, 0.2f}, grey = {0.75f, 0.75f, 0.78f};
    if (bits == ZPAD_A) return blue;
    if (bits == ZPAD_B) return green;
    if (bits == ZPAD_START) return red;
    if (bits & (ZPAD_C_UP | ZPAD_C_DOWN | ZPAD_C_LEFT | ZPAD_C_RIGHT)) return yellow;
    return grey;
}

// One button, idle or pressed. Filled: translucent disc, outline ring, label or C arrow.
// Outline: the ring alone, and a press fills it faintly so the touch is still acknowledged.
inline void ZuneBuildButton(ZuneOverlayBuilder &b, const ZuneTouchButton &button, bool pressed, ZuneOverlayStyle style)
{
    if (style.buttons == ZUNE_BUTTONS_HIDDEN) return;
    ZuneColor colour = ZuneButtonColor(button.bits), white = {1, 1, 1};
    float x = button.x, y = button.y, r = button.radius, o = style.opacity;
    if (style.buttons == ZUNE_BUTTONS_OUTLINE) {
        if (pressed) b.Disc(x, y, r, colour, 0.3f * o);
        b.Ring(x, y, r, 2.0f, colour, (pressed ? 1.0f : 0.75f) * o);
        return;
    }
    b.Disc(x, y, r, colour, (pressed ? 0.55f : 0.2f) * o);
    b.Ring(x, y, r, 2.0f, colour, (pressed ? 1.0f : 0.75f) * o);
    int arrow = button.bits == ZPAD_C_UP ? 0 : button.bits == ZPAD_C_DOWN ? 1 : button.bits == ZPAD_C_LEFT ? 2
              : button.bits == ZPAD_C_RIGHT ? 3 : -1;
    if (arrow >= 0) b.Arrow(x, y, r * 0.9f, arrow, colour, (pressed ? 1.0f : 0.85f) * o);
    else if (button.bits == ZPAD_START) b.Text("START", x, y, 8, 1.5f, white, 0.9f * o);
    else b.Text(button.label, x, y, r * 0.75f, r > 30 ? 3.0f : 2.5f, white, 0.9f * o);
}

// The floating stick: a base ring where the thumb landed and a knob at the clamped offset.
// At most 12 * SEGMENTS vertices in every style.
inline void ZuneBuildStick(ZuneOverlayBuilder &b, float originX, float originY, float x, float y, ZuneOverlayStyle style)
{
    if (style.buttons == ZUNE_BUTTONS_HIDDEN) return;
    ZuneColor grey = {0.85f, 0.85f, 0.9f};
    float dx = x - originX, dy = y - originY, length2 = dx * dx + dy * dy, reach = (float)ZUNE_STICK_RADIUS;
    float o = style.opacity;
    if (length2 > reach * reach) {
        float scale = reach / sqrtf(length2);
        dx *= scale; dy *= scale;
    }
    if (style.buttons == ZUNE_BUTTONS_OUTLINE) {
        b.Ring(originX, originY, reach, 2.0f, grey, 0.6f * o);
        b.Ring(originX + dx, originY + dy, 18, 2.0f, grey, 0.75f * o);
        return;
    }
    b.Disc(originX, originY, reach, grey, 0.12f * o);
    b.Ring(originX, originY, reach, 2.0f, grey, 0.6f * o);
    b.Disc(originX + dx, originY + dy, 18, grey, 0.55f * o);
}

// Landscape pixels -> rotated panel clip space, in place.
inline void ZuneOverlayToClip(ZuneOverlayVertex *v, int count, bool clockwise)
{
    for (int i = 0; i < count; ++i) {
        float cx = v[i].x * (2.0f / ZUNE_VIEW_W) - 1.0f, cy = 1.0f - v[i].y * (2.0f / ZUNE_VIEW_H);
        ZuneRotateClip(&cx, &cy, clockwise);
        v[i].x = cx;
        v[i].y = cy;
    }
}
#endif
