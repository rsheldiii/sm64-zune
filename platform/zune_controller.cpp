// N64 controller over the Zune HD touch screen (replaces sm64ex's controller_entry_point.c).
// Layout and mapping live in zune_touch.h. Holding three or more fingers down for 1.5 s exits.
//
// The game reads its controller once per 1/30 s tick, and a quick tap can start and end
// inside one tick. So the panel is sampled by its own thread, several times a tick, and
// ZunePadLatch (zune_touch.h) turns the samples into what the game is told: no tap or quick
// double tap is lost between two reads. If ZDKInput will not answer on that thread, the game
// thread samples once per read, as it used to.
#include <windows.h>
#include <string.h>
#include <zdk.h>
extern "C" {
#include "lib/src/libultra_internal.h"
#include "lib/src/osContInternal.h"
#include "macros.h"
#include "pc/controller/controller_api.h"
}
#include "zune_platform.h"

ZunePadState zuneLastPad;      // what the game was last told (also drives the overlay)
ZuneStickView zuneStickView;
bool zuneExitRequested;

// One ZDKInput_GetState takes ~0.7 ms, mostly waiting on the system; 12 ms of sleep between
// them gives about 2.6 samples per tick, so any touch of 13 ms or longer is seen.
enum { SAMPLE_SLEEP_MS = 12, SAMPLER_PRIORITY = 220, SAMPLER_FAILURES = 5 };   // CE priority: audio 200, game 251

static bool inputReady, lockReady;
static CRITICAL_SECTION inputLock;   // guards everything below, shared with the sampling thread
static ZuneTouchFormat touchFormat;
static ZuneTouchControls touchControls;
static ZunePadLatch latch;
static ZuneStickView sampledStick;
static DWORD multiTouchStart;
static int statPolls;
static double statPollSeconds;

static HANDLE samplerThread;
static volatile bool samplerQuit, samplerActive;
static volatile DWORD lastTouchTick;   // GetTickCount when a sample last contained a touch

// Reads the panel once and feeds the latch. False if ZDKInput did not answer.
static bool SampleTouches()
{
    ZDK_INPUT_STATE input;
    memset(&input, 0, sizeof(input));
    double start = ZuneSeconds();
    bool gotInput = inputReady && SUCCEEDED(ZDKInput_GetState(&input));
    double seconds = ZuneSeconds() - start;
    EnterCriticalSection(&inputLock);
    ++statPolls;
    statPollSeconds += seconds;
    ZunePadState pad = {0, 0, 0};
    sampledStick.held = false;
    if (gotInput) {
        ZuneTouchPoint points[4];
        int count = input.TouchState.Count;
        if (count < 0) count = 0;
        if (count > 4) count = 4;
        for (int i = 0; i < count; ++i) {
            const ZDK_TOUCH_LOCATION &t = input.TouchState.Locations[i];
            points[i].id = t.Id;
            ZuneTouchToView(t.X, t.Y, touchFormat.Normalized(t.X, t.Y), zuneRotateClockwise, &points[i].x, &points[i].y);
        }
        DWORD tick = GetTickCount();
        if (count > 0) lastTouchTick = tick;
        pad = touchControls.Update(points, count);
        for (int i = 0; i < count && touchControls.stickHeld; ++i) {
            if (points[i].id != touchControls.stickId) continue;
            sampledStick.held = true;
            sampledStick.originX = touchControls.originX;
            sampledStick.originY = touchControls.originY;
            sampledStick.x = points[i].x;
            sampledStick.y = points[i].y;
        }
        if (count >= 3) {
            if (!multiTouchStart) multiTouchStart = tick;
            else if (tick - multiTouchStart > 1500) zuneExitRequested = true;
        } else {
            multiTouchStart = 0;
        }
    }
    latch.Sample(pad);
    LeaveCriticalSection(&inputLock);
    return gotInput;
}

static DWORD WINAPI SamplerMain(LPVOID)
{
    int failures = 0;
    __try {
        while (!samplerQuit) {
            if (SampleTouches()) failures = 0;
            else if (++failures >= SAMPLER_FAILURES) break;
            Sleep(SAMPLE_SLEEP_MS);
        }
    } __except (ZuneLogException(GetExceptionInformation())) {
        ZuneLog("touch sampling thread raised an exception");
    }
    if (!samplerQuit) ZuneLog("touch sampling thread stopped; the game thread samples once per tick instead");
    samplerActive = false;
    return 0;
}

// Safety net for the sampling thread: nothing documents that ZDKInput answers truthfully on a
// second thread, and dead controls would also disable the exit gesture. While the thread has
// reported no touch for a second, the game thread looks once a second itself; if it sees a
// touch on two looks in a row that the thread still has not seen, the thread is blind and the
// game thread takes over (one sample per tick, as before the thread existed).
static void CrossCheckSampler()
{
    static DWORD lastCheck, suspectedAt;
    DWORD now = GetTickCount();
    if (now - lastTouchTick < 1000) { suspectedAt = 0; return; }
    if (now - lastCheck < 1000) return;
    lastCheck = now;
    ZDK_INPUT_STATE input;
    memset(&input, 0, sizeof(input));
    if (FAILED(ZDKInput_GetState(&input)) || input.TouchState.Count <= 0) { suspectedAt = 0; return; }
    if (!suspectedAt) { suspectedAt = now; return; }
    samplerQuit = true;
    samplerActive = false;
    ZuneLog("touch sampling thread saw no touch for %lu ms while the game thread sees one; sampling once per tick instead",
            now - lastTouchTick);
}

void ZuneInputTakeStats(int *polls, double *seconds)
{
    *polls = 0;
    *seconds = 0;
    if (!lockReady) return;
    EnterCriticalSection(&inputLock);
    *polls = statPolls;
    *seconds = statPollSeconds;
    statPolls = 0;
    statPollSeconds = 0;
    LeaveCriticalSection(&inputLock);
}

extern "C" {
s32 osContInit(UNUSED OSMesgQueue *mq, u8 *controllerBits, UNUSED OSContStatus *status)
{
    if (!lockReady) InitializeCriticalSection(&inputLock);
    lockReady = true;
    inputReady = SUCCEEDED(ZDKInput_Initialize());
    if (inputReady) {
        samplerActive = true;
        samplerThread = CreateThread(NULL, 64 * 1024, SamplerMain, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
        if (samplerThread) CeSetThreadPriority(samplerThread, SAMPLER_PRIORITY);
        else samplerActive = false;
    }
    ZuneLog("touch input %s%s", inputReady ? "ready" : "unavailable",
            samplerThread ? ", sampled on its own thread" : inputReady ? ", sampled once per tick" : "");
    *controllerBits = 1;
    return 0;
}

s32 osMotorStart(UNUSED void *pfs) { return 0; }
s32 osMotorStop(UNUSED void *pfs) { return 0; }
u32 osMotorInit(UNUSED OSMesgQueue *mq, UNUSED void *pfs, UNUSED s32 port) { return 0; }
s32 osContStartReadData(UNUSED OSMesgQueue *mesg) { return 0; }

void osContGetReadData(OSContPad *pad)
{
    if (lockReady) {
        if (samplerActive) CrossCheckSampler();
        if (!samplerActive) SampleTouches();
        EnterCriticalSection(&inputLock);
        zuneLastPad = latch.Take();
        zuneStickView = sampledStick;
        LeaveCriticalSection(&inputLock);
    }
    pad->button = zuneLastPad.buttons;
    pad->stick_x = zuneLastPad.stickX;
    pad->stick_y = zuneLastPad.stickY;
    pad->ext_stick_x = 0;
    pad->ext_stick_y = 0;
    pad->errnum = 0;
}

u32 controller_get_raw_key(void) { return VK_INVALID; }

void controller_shutdown(void)
{
    if (samplerThread) {
        samplerQuit = true;
        WaitForSingleObject(samplerThread, 1000);
        CloseHandle(samplerThread);
        samplerThread = NULL;
    }
    if (inputReady) ZDKInput_Shutdown();
    inputReady = false;
}

void controller_reconfigure(void) {}
void controller_rumble_play(float str, float time) { (void)str; (void)time; }
void controller_rumble_stop(void) {}
}
