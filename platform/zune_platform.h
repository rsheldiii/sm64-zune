#ifndef ZUNE_PLATFORM_H
#define ZUNE_PLATFORM_H
// Internals shared by the C++ units of the Zune HD platform layer.
#include <windows.h>
#include "zune_touch.h"

// Appends a line to \Flash2\SM64\log.txt; does nothing in a build with ZUNE_LOG=off. Cheap
// enough for milestones and periodic stats, not per frame. Runs add to the same file until
// leaving the game hands it to the computer (zune_upload.cpp).
void ZuneLog(const char *format, ...);
double ZuneSeconds();
// Names the step in progress for the crash handler. During startup and the first frames every
// step is also logged, so a crash that kills the process outright still leaves a trail.
void ZuneStep(const char *step);
// SEH filter: logs the exception, current step, registers and stack code addresses, then
// returns EXCEPTION_EXECUTE_HANDLER. Use as __except (ZuneLogException(GetExceptionInformation())).
LONG ZuneLogException(EXCEPTION_POINTERS *info);
// Logs memory words from address - 4*before to address + 4*after as "<title> <addr>: words"
// lines (stops at an unreadable page), for disassembling system DLL code offline.
void ZuneLogWords(const char *title, ULONG address, int before, int after);
// Logs the address and first `words` code words of each named export of a loaded module,
// then `calleeWords` words of the implementation it calls (0: none).
void ZuneDumpExports(const WCHAR *module, const WCHAR *const names[], int count, int words, int calleeWords);

// Sampling profiler (zune_profile.cpp, ZUNE_PROFILE=on): the game thread's pc/lr/phase every
// ~1 ms, plus a worker's while *busy is non-zero, into \Flash2\SM64\profile.bin.
enum {
    ZUNE_PHASE_OTHER, ZUNE_PHASE_GAME, ZUNE_PHASE_RENDER, ZUNE_PHASE_AUDIO, ZUNE_PHASE_PRESENT,
    ZUNE_PHASE_IDLE, ZUNE_PHASE_WORKER = 0x100
};
extern volatile LONG zunePhase;
void ZuneProfileStart();
void ZuneProfileAddWorker(HANDLE thread, volatile LONG *busy);
void ZuneProfileStop();

// Per-frame renderer counters, summed by the frame loop's periodic stats.
struct ZuneRenderStats {
    int drawCalls, triangles, textureUploads, programSwitches, samplerSets, stateCalls;
};
extern ZuneRenderStats zuneRenderStats;

// Rendering backend hooks used by the window layer.
void ZuneRenderPresent();          // touch overlay + ZDKGL_EndDraw
// A full-screen progress bar (exit upload): state 0 working, 1 done, -1 failed.
void ZuneDrawProgress(float fraction, int state);
// Sends log.txt and profile.bin over Wi-Fi to `./sm64zune logs` (zune_upload.cpp).
bool ZuneUploadLogs();
extern bool zuneRotateClockwise;   // false: hold the Zune turned counter-clockwise
extern bool zuneShowTouchOverlay;

extern bool zuneAudioActive;       // a voice is streaming (zune_audio.cpp)
// Since the last call: underruns, lowest frames buffered ahead, longest gap between updates.
void ZuneAudioTakeStats(int *underrunCount, int *lowestBuffered, double *longestGapMs);

// Controller state exposed for the overlay and the exit gesture.
extern ZunePadState zuneLastPad;
// Since the last call: panel reads (ZDKInput_GetState) and the seconds they took in total.
void ZuneInputTakeStats(int *polls, double *seconds);
extern bool zuneExitRequested;
struct ZuneStickView { bool held; float originX, originY, x, y; };   // landscape pixels
extern ZuneStickView zuneStickView;

extern "C" {
// zune_vfp.asm: the calling thread's VFP status and control register.
unsigned zune_fpscr_get(void);
void zune_fpscr_set(unsigned value);
struct GfxRenderingAPI;
struct AudioAPI;
extern struct GfxRenderingAPI zune_rendering_api;
extern struct AudioAPI zune_audio_api;
}
#endif
