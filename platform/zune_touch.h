#ifndef ZUNE_TOUCH_H
#define ZUNE_TOUCH_H
#include <math.h>
// Landscape geometry and touch controls for SM64 on the Zune HD. Platform-neutral so it is
// unit-tested on the Linux host (tests/touch.cpp).
//
// The panel is portrait, 272x480. The game renders landscape, 480x272, rotated in the GL
// backend. With the device turned counter-clockwise (portrait top edge on the player's
// left), landscape NDC (lx, ly) maps to panel NDC (-ly, lx).

enum {
    ZUNE_PANEL_W = 272, ZUNE_PANEL_H = 480,      // physical framebuffer and touch panel
    ZUNE_VIEW_W = 480, ZUNE_VIEW_H = 272         // what the game sees
};

// N64 button bits (PR/os_cont.h CONT_*), duplicated so this header stays standalone.
enum {
    ZPAD_A = 0x8000, ZPAD_B = 0x4000, ZPAD_Z = 0x2000, ZPAD_START = 0x1000,
    ZPAD_L = 0x0020, ZPAD_R = 0x0010,
    ZPAD_C_UP = 0x0008, ZPAD_C_DOWN = 0x0004, ZPAD_C_LEFT = 0x0002, ZPAD_C_RIGHT = 0x0001
};

// Landscape GL rectangle (origin bottom-left) -> panel rectangle, for glViewport/glScissor.
inline void ZuneRotateRect(int x, int y, int w, int h, bool clockwise, int out[4])
{
    if (clockwise) { out[0] = y; out[1] = ZUNE_VIEW_W - x - w; }
    else { out[0] = ZUNE_PANEL_W - y - h; out[1] = x; }
    out[2] = h;
    out[3] = w;
}

// Landscape clip-space position -> panel clip-space position (w is unchanged).
inline void ZuneRotateClip(float *x, float *y, bool clockwise)
{
    float lx = *x, ly = *y;
    if (clockwise) { *x = ly; *y = -lx; }
    else { *x = -ly; *y = lx; }
}

// Raw ZDKInput touch -> landscape pixels (origin top-left, y down). Our earlier apps could
// not tell whether the API reports normalized (0..1) or panel-pixel coordinates; the caller
// decides once (see ZuneTouchFormat) because a pixel touch at the corner looks normalized.
inline void ZuneTouchToView(float tx, float ty, bool normalized, bool clockwise, float *vx, float *vy)
{
    if (normalized) { tx *= ZUNE_PANEL_W - 1; ty *= ZUNE_PANEL_H - 1; }
    if (clockwise) { *vx = ty; *vy = ZUNE_PANEL_W - 1 - tx; }
    else { *vx = ZUNE_PANEL_H - 1 - ty; *vy = tx; }
}

// Assumes normalized coordinates until any value above 1 proves the API reports pixels.
struct ZuneTouchFormat {
    bool pixels;
    ZuneTouchFormat() : pixels(false) {}
    bool Normalized(float tx, float ty) {
        if (tx > 1.001f || ty > 1.001f) pixels = true;
        return !pixels;
    }
};

struct ZuneTouchButton { unsigned short bits; short x, y, radius; const char *label; };

// Landscape layout: analog stick anywhere on the left, buttons on the right.
static const ZuneTouchButton ZUNE_BUTTONS[] = {
    {ZPAD_A, 430, 218, 40, "A"},
    {ZPAD_B, 360, 238, 32, "B"},
    {ZPAD_Z, 430, 140, 30, "Z"},
    {ZPAD_R, 300, 60, 24, "R"},
    {ZPAD_START, 254, 26, 24, "START"},
    {ZPAD_C_UP, 400, 26, 22, "CU"},
    {ZPAD_C_DOWN, 400, 92, 22, "CD"},
    {ZPAD_C_LEFT, 360, 59, 22, "CL"},
    {ZPAD_C_RIGHT, 440, 59, 22, "CR"}
};
enum { ZUNE_BUTTON_COUNT = sizeof(ZUNE_BUTTONS) / sizeof(ZUNE_BUTTONS[0]) };
enum { ZUNE_STICK_ZONE_X = 220, ZUNE_STICK_RADIUS = 52, ZUNE_STICK_MAX = 80 };

struct ZunePadState { unsigned short buttons; signed char stickX, stickY; };

struct ZuneTouchPoint { unsigned id; float x, y; };   // landscape pixels

// A floating analog stick: the first touch that lands in the left zone becomes its centre and
// keeps steering (anywhere on screen) until released. Other touches press buttons.
struct ZuneTouchControls {
    bool stickHeld;
    unsigned stickId;
    float originX, originY;
    ZuneTouchControls() : stickHeld(false), stickId(0), originX(0), originY(0) {}

    ZunePadState Update(const ZuneTouchPoint *touches, int count) {
        ZunePadState pad = {0, 0, 0};
        bool stickSeen = false;
        for (int i = 0; i < count; ++i)
            if (stickHeld && touches[i].id == stickId) stickSeen = true;
        if (!stickSeen) stickHeld = false;
        for (int i = 0; i < count; ++i) {
            const ZuneTouchPoint &t = touches[i];
            if (!stickHeld && t.x < ZUNE_STICK_ZONE_X) {
                stickHeld = true; stickId = t.id; originX = t.x; originY = t.y;
            }
            if (stickHeld && t.id == stickId) {
                float dx = (t.x - originX) / ZUNE_STICK_RADIUS, dy = (originY - t.y) / ZUNE_STICK_RADIUS;
                float length2 = dx * dx + dy * dy;
                if (length2 > 1) {   // clamp to the stick's circle
                    float length = sqrtf(length2);
                    dx /= length; dy /= length;
                }
                pad.stickX = (signed char)(dx * ZUNE_STICK_MAX);
                pad.stickY = (signed char)(dy * ZUNE_STICK_MAX);
                continue;
            }
            for (int b = 0; b < ZUNE_BUTTON_COUNT; ++b) {
                const ZuneTouchButton &button = ZUNE_BUTTONS[b];
                float bx = t.x - button.x, by = t.y - button.y, reach = button.radius * 1.3f;
                if (bx * bx + by * by <= reach * reach) pad.buttons |= button.bits;
            }
        }
        return pad;
    }
};

// Button changes between two reads of the pad. The game reads its controller once per 1/30 s
// tick, but a quick tap can start and end inside one tick, and a quick double tap can lift
// and land again inside one. The panel is therefore sampled several times a tick
// (zune_controller.cpp), and this turns those samples into what the game is told:
//   - a button seen down since the last read is reported down, even if it is up again;
//   - a button the game was told is down, and that was seen up since, is reported up for one
//     read, even if it is down again; the new press then registers on the next read.
// The stick and steadily held buttons come from the newest sample.
struct ZunePadLatch {
    ZunePadState newest;
    unsigned short seenDown, seenUp, reported;
    ZunePadLatch() : seenDown(0), seenUp(0), reported(0) { newest.buttons = 0; newest.stickX = newest.stickY = 0; }

    void Sample(const ZunePadState &pad) {
        newest = pad;
        seenDown |= pad.buttons;
        seenUp |= (unsigned short)~pad.buttons;
    }
    ZunePadState Take() {
        ZunePadState pad = newest;
        pad.buttons = (unsigned short)((newest.buttons | seenDown) & ~(reported & seenUp));
        reported = pad.buttons;
        seenDown = seenUp = 0;
        return pad;
    }
};
#endif
