#include <assert.h>
#include <math.h>
#include <stdio.h>
#include "../platform/zune_touch.h"

// A landscape pixel, as GL sees it, must land where the matching touch is reported.
static void TestGeometryAgrees(bool clockwise)
{
    for (int lx = 0; lx < ZUNE_VIEW_W; lx += 37) {
        for (int ly = 0; ly < ZUNE_VIEW_H; ly += 29) {
            // Clip-space centre of landscape pixel (lx, ly), origin bottom-left.
            float x = (lx + 0.5f) / ZUNE_VIEW_W * 2 - 1, y = (ly + 0.5f) / ZUNE_VIEW_H * 2 - 1;
            ZuneRotateClip(&x, &y, clockwise);
            float px = (x + 1) / 2 * ZUNE_PANEL_W, py = (y + 1) / 2 * ZUNE_PANEL_H;   // panel, bottom-left origin
            int rect[4];
            ZuneRotateRect(lx, ly, 1, 1, clockwise, rect);
            assert(rect[0] <= px && px <= rect[0] + rect[2] && rect[1] <= py && py <= rect[1] + rect[3]);
            // The touch panel reports top-down pixels.
            float vx, vy;
            ZuneTouchToView(px, ZUNE_PANEL_H - py, false, clockwise, &vx, &vy);
            assert(fabsf(vx - lx) <= 1.5f && fabsf(vy - (ZUNE_VIEW_H - 1 - ly)) <= 1.5f);
        }
    }
    int full[4];
    ZuneRotateRect(0, 0, ZUNE_VIEW_W, ZUNE_VIEW_H, clockwise, full);
    assert(full[0] == 0 && full[1] == 0 && full[2] == ZUNE_PANEL_W && full[3] == ZUNE_PANEL_H);
}

static void TestNormalizedTouches()
{
    float vx, vy;
    ZuneTouchToView(0, 1, true, false, &vx, &vy);      // normalized bottom-left of the panel
    assert(vx == 0 && vy == 0);                        // = top-left of the landscape view
    ZuneTouchToView(271, 0, false, false, &vx, &vy);   // pixel top-right of the panel
    assert(vx == ZUNE_PANEL_H - 1 && vy == 271);
    ZuneTouchFormat format;
    assert(format.Normalized(0.5f, 0.25f) && format.Normalized(1, 1));
    assert(!format.Normalized(200, 0.5f) && !format.Normalized(0.5f, 0.5f));   // pixels is sticky
}

static void TestControls()
{
    ZuneTouchControls controls;
    ZuneTouchPoint touches[3] = {{7, 100, 150}, {9, 430, 218}, {11, 360, 238}};
    ZunePadState pad = controls.Update(touches, 3);               // stick anchors, A and B pressed
    assert(pad.stickX == 0 && pad.stickY == 0 && pad.buttons == (ZPAD_A | ZPAD_B));
    touches[0].x = 100 + ZUNE_STICK_RADIUS / 2;                    // half right
    pad = controls.Update(touches, 1);
    assert(pad.stickX == ZUNE_STICK_MAX / 2 && pad.stickY == 0 && pad.buttons == 0);
    touches[0].x = 100; touches[0].y = 150 - ZUNE_STICK_RADIUS * 3; // far up: clamps to full up
    pad = controls.Update(touches, 1);
    assert(pad.stickX == 0 && pad.stickY >= ZUNE_STICK_MAX - 1);
    touches[0].x = 400; touches[0].y = 150;                        // stick keeps steering off-zone
    pad = controls.Update(touches, 1);
    assert(pad.stickX >= ZUNE_STICK_MAX - 1 && pad.buttons == 0);
    touches[0].x = 100 + 300; touches[0].y = 150 - 300;
    pad = controls.Update(touches, 1);
    assert(abs(pad.stickX - 56) <= 1 && abs(pad.stickY - 56) <= 1);
    pad = controls.Update(touches, 0);                             // release resets
    assert(!controls.stickHeld && pad.stickX == 0);
    ZuneTouchPoint right = {3, 300, 60};                           // a right-side touch is never the stick
    pad = controls.Update(&right, 1);
    assert(!controls.stickHeld && pad.buttons == ZPAD_R);
    for (int i = 0; i < ZUNE_BUTTON_COUNT; ++i) {
        const ZuneTouchButton &b = ZUNE_BUTTONS[i];
        assert(b.x - b.radius >= ZUNE_STICK_ZONE_X && b.x + b.radius <= ZUNE_VIEW_W);
        assert(b.y - b.radius >= 0 && b.y + b.radius <= ZUNE_VIEW_H);
    }
}

static ZunePadState Pad(unsigned short buttons, signed char stickX = 0)
{
    ZunePadState pad = {buttons, stickX, 0};
    return pad;
}

// What the game is told when the panel is sampled several times between its reads.
static void TestLatch()
{
    ZunePadLatch latch;
    assert(latch.Take().buttons == 0);
    // A tap that starts and ends between two reads is still one press, one read long.
    latch.Sample(Pad(0)); latch.Sample(Pad(ZPAD_A)); latch.Sample(Pad(0));
    assert(latch.Take().buttons == ZPAD_A);
    latch.Sample(Pad(0)); latch.Sample(Pad(0));
    assert(latch.Take().buttons == 0);
    // A held button stays down; releasing it is reported at the next read, not a read late.
    latch.Sample(Pad(ZPAD_B)); latch.Sample(Pad(ZPAD_B));
    assert(latch.Take().buttons == ZPAD_B);
    latch.Sample(Pad(ZPAD_B)); latch.Sample(Pad(ZPAD_B));
    assert(latch.Take().buttons == ZPAD_B);
    latch.Sample(Pad(ZPAD_B)); latch.Sample(Pad(0)); latch.Sample(Pad(0));
    assert(latch.Take().buttons == 0);
    // A quick double tap: lifting and landing again between two reads shows as a release,
    // then the second press on the next read.
    latch.Sample(Pad(ZPAD_A)); assert(latch.Take().buttons == ZPAD_A);
    latch.Sample(Pad(ZPAD_A)); latch.Sample(Pad(0)); latch.Sample(Pad(ZPAD_A));
    assert(latch.Take().buttons == 0);
    latch.Sample(Pad(ZPAD_A)); latch.Sample(Pad(ZPAD_A));
    assert(latch.Take().buttons == ZPAD_A);
    // Buttons are independent: B stays held while A is released, then tapped.
    latch.Sample(Pad(ZPAD_A | ZPAD_B)); latch.Sample(Pad(ZPAD_B));
    assert(latch.Take().buttons == ZPAD_B);
    latch.Sample(Pad(ZPAD_B)); latch.Sample(Pad(ZPAD_A | ZPAD_B)); latch.Sample(Pad(ZPAD_B));
    assert(latch.Take().buttons == (ZPAD_A | ZPAD_B));
    latch.Sample(Pad(ZPAD_B));
    assert(latch.Take().buttons == ZPAD_B);
    // The stick is the newest sample; with no sample between reads the state simply holds.
    latch.Sample(Pad(ZPAD_Z, 20)); latch.Sample(Pad(ZPAD_Z, 70));
    ZunePadState pad = latch.Take();
    assert(pad.buttons == ZPAD_Z && pad.stickX == 70);
    pad = latch.Take();
    assert(pad.buttons == ZPAD_Z && pad.stickX == 70);
    // One sample per read (the fallback without the sampling thread) passes straight through.
    static const unsigned short sequence[] = {0, ZPAD_A, ZPAD_A, 0, ZPAD_B, ZPAD_A | ZPAD_B, 0};
    ZunePadLatch plain;
    for (unsigned i = 0; i < sizeof(sequence) / sizeof(sequence[0]); ++i) {
        plain.Sample(Pad(sequence[i]));
        assert(plain.Take().buttons == sequence[i]);
    }
}

int main()
{
    TestLatch();
    TestGeometryAgrees(false);
    TestGeometryAgrees(true);
    TestNormalizedTouches();
    TestControls();
    puts("touch tests passed");
    return 0;
}
