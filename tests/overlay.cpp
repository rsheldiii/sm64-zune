// Test of the touch overlay geometry (platform/zune_overlay.h).
// With an output path argument it also rasterizes the overlay (idle buttons, the A button
// pressed, the stick held) into a binary PPM for a visual check; two more arguments choose
// the style (0 hidden, 1 outline, 2 filled) and the opacity in percent.
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../platform/zune_overlay.h"

enum { CAPACITY = 8192 };
static ZuneOverlayVertex vertices[CAPACITY];

static const ZuneOverlayStyle FULL = {ZUNE_BUTTONS_FILLED, 1.0f};

static int BuildAll(bool pressedA, bool stick, ZuneOverlayStyle style, ZuneOverlayVertex *out = vertices)
{
    ZuneOverlayBuilder b(out, CAPACITY);
    for (int i = 0; i < ZUNE_BUTTON_COUNT; ++i)
        ZuneBuildButton(b, ZUNE_BUTTONS[i], pressedA && ZUNE_BUTTONS[i].bits == ZPAD_A, style);
    if (stick) ZuneBuildStick(b, 110, 170, 190, 120, style);
    assert(!b.Overflowed());
    return b.Count();
}

// The build settings ZUNE_TOUCH_BUTTONS and ZUNE_TOUCH_OPACITY.
static void TestStyles()
{
    const int segments = ZuneOverlayBuilder::SEGMENTS, ring = 6 * segments, disc = 3 * segments;
    static ZuneOverlayVertex other[CAPACITY];
    // Hidden draws nothing at all, buttons or stick.
    ZuneOverlayStyle hidden = {ZUNE_BUTTONS_HIDDEN, 1.0f};
    assert(BuildAll(true, true, hidden) == 0);
    // Outline is one ring per button; a pressed button adds a faint fill; the stick is two rings.
    ZuneOverlayStyle outline = {ZUNE_BUTTONS_OUTLINE, 1.0f};
    assert(BuildAll(false, false, outline) == ZUNE_BUTTON_COUNT * ring);
    assert(BuildAll(true, false, outline) == ZUNE_BUTTON_COUNT * ring + disc);
    assert(BuildAll(false, true, outline) == ZUNE_BUTTON_COUNT * ring + 2 * ring);
    // The renderer's stick buffer holds 12 * SEGMENTS vertices; every style fits.
    for (int style = ZUNE_BUTTONS_HIDDEN; style <= ZUNE_BUTTONS_FILLED; ++style) {
        ZuneOverlayStyle s = {style, 1.0f};
        ZuneOverlayBuilder stick(other, 12 * segments);
        ZuneBuildStick(stick, 110, 170, 190, 120, s);
        assert(!stick.Overflowed());
    }
    // Filled draws more than the outline, and a press changes colours, not the triangle count.
    int filled = BuildAll(false, false, FULL);
    assert(filled > ZUNE_BUTTON_COUNT * (ring + disc) && BuildAll(true, false, FULL) == filled);
    // Opacity scales every alpha and nothing else.
    ZuneOverlayStyle faint = {ZUNE_BUTTONS_FILLED, 0.8f};
    int count = BuildAll(true, true, FULL), faintCount = BuildAll(true, true, faint, other);
    assert(count == faintCount);
    for (int i = 0; i < count; ++i) {
        assert(vertices[i].x == other[i].x && vertices[i].y == other[i].y && vertices[i].r == other[i].r);
        assert(fabs(other[i].a - 0.8f * vertices[i].a) < 1e-6f);
    }
}

static void TestGeometry()
{
    int count = BuildAll(false, false, FULL);
    assert(count > 0 && count % 3 == 0);
    for (int i = 0; i < count; ++i) {
        assert(vertices[i].x >= 0 && vertices[i].x <= ZUNE_VIEW_W);
        assert(vertices[i].y >= 0 && vertices[i].y <= ZUNE_VIEW_H);
        assert(vertices[i].a > 0 && vertices[i].a <= 1);
    }
    // Every label has strokes: letters render, START spells out.
    ZuneOverlayVertex scratch[512];
    ZuneOverlayBuilder letters(scratch, 512);
    ZuneColor white = {1, 1, 1};
    const char *glyphs = "ABRSTZ";
    for (int i = 0; glyphs[i]; ++i) assert(letters.Glyph(glyphs[i], 0, 0, 6, 10, 2, white, 1));
    assert(!letters.Glyph('?', 0, 0, 6, 10, 2, white, 1));
    // Clip conversion keeps everything on screen and is a rotation of the landscape view.
    ZuneOverlayToClip(vertices, count, false);
    for (int i = 0; i < count; ++i) assert(fabs(vertices[i].x) <= 1.0001f && fabs(vertices[i].y) <= 1.0001f);
    ZuneOverlayVertex corner = {0, 0, 1, 1, 1, 1};   // landscape top-left -> panel clip (-1, -1)
    ZuneOverlayToClip(&corner, 1, false);
    assert(fabs(corner.x + 1) < 1e-5f && fabs(corner.y + 1) < 1e-5f);
    // Overflow is reported, not written past the end.
    ZuneOverlayVertex small[6];
    ZuneOverlayBuilder tiny(small, 6);
    tiny.Disc(10, 10, 5, white, 1);
    assert(tiny.Overflowed() && tiny.Count() == 6);
    printf("overlay: %d vertices for idle buttons\n", count);
}

// Rasterizes landscape-pixel triangles with source-over blending onto a dark background.
static void Render(const char *path, ZuneOverlayStyle style)
{
    static float image[ZUNE_VIEW_H][ZUNE_VIEW_W][3];
    for (int y = 0; y < ZUNE_VIEW_H; ++y)
        for (int x = 0; x < ZUNE_VIEW_W; ++x) {
            float shade = 0.18f + 0.12f * (float)y / ZUNE_VIEW_H;   // stand-in for the game
            image[y][x][0] = shade * 0.8f; image[y][x][1] = shade; image[y][x][2] = shade * 1.3f;
        }
    int count = BuildAll(true, true, style);
    for (int t = 0; t + 2 < count; t += 3) {
        const ZuneOverlayVertex &a = vertices[t], &b = vertices[t + 1], &c = vertices[t + 2];
        int x0 = (int)floor(fmin(a.x, fmin(b.x, c.x))), x1 = (int)ceil(fmax(a.x, fmax(b.x, c.x)));
        int y0 = (int)floor(fmin(a.y, fmin(b.y, c.y))), y1 = (int)ceil(fmax(a.y, fmax(b.y, c.y)));
        float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
        if (fabs(area) < 1e-6f) continue;
        for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < ZUNE_VIEW_H; ++y)
            for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < ZUNE_VIEW_W; ++x) {
                float px = x + 0.5f, py = y + 0.5f;
                float w0 = ((b.x - px) * (c.y - py) - (b.y - py) * (c.x - px)) / area;
                float w1 = ((c.x - px) * (a.y - py) - (c.y - py) * (a.x - px)) / area;
                float w2 = 1 - w0 - w1;
                if (w0 < 0 || w1 < 0 || w2 < 0) continue;
                for (int k = 0; k < 3; ++k) {
                    float src = k == 0 ? a.r : k == 1 ? a.g : a.b;
                    image[y][x][k] = src * a.a + image[y][x][k] * (1 - a.a);
                }
            }
    }
    FILE *file = fopen(path, "wb");
    assert(file);
    fprintf(file, "P6\n%d %d\n255\n", ZUNE_VIEW_W, ZUNE_VIEW_H);
    for (int y = 0; y < ZUNE_VIEW_H; ++y)
        for (int x = 0; x < ZUNE_VIEW_W; ++x)
            for (int k = 0; k < 3; ++k) fputc((int)(fmin(1.0f, image[y][x][k]) * 255 + 0.5f), file);
    fclose(file);
}

int main(int argc, char **argv)
{
    TestGeometry();
    TestStyles();
    if (argc > 1) {
        ZuneOverlayStyle style = {argc > 2 ? atoi(argv[2]) : ZUNE_BUTTONS_FILLED, argc > 3 ? atoi(argv[3]) / 100.0f : 0.8f};
        Render(argv[1], style);
    }
    printf("overlay tests passed\n");
    return 0;
}
