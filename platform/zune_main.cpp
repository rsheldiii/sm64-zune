// Super Mario 64 (sm64ex) on the Zune HD: entry point, 30 Hz frame loop and the small pieces
// of sm64ex's desktop glue (pc_main.c, platform.c, fs, cliopts) that the game still needs.
#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zdk.h>
extern "C" {
#include "sm64.h"
#include "game/memory.h"
#include "game/game_init.h"
#include "game/thread6.h"
#include "game/main.h"
#include "audio/external.h"
#include "pc/gfx/gfx_pc.h"
#include "pc/gfx/gfx_window_manager_api.h"
#include "pc/audio/audio_api.h"
#include "pc/cliopts.h"
#include "pc/configfile.h"
#include "pc/platform.h"
#include "pc/fs/fs.h"
#include "pc/controller/controller_api.h"
}
#include "zune_build.h"
#include "zune_settings.h"
#include "zune_gl.h"
#include "zune_pacing.h"
#include "zune_platform.h"

static const WCHAR DATA_DIR[] = L"\\Flash2\\SM64";
static const WCHAR LOG_FILE[] = L"\\Flash2\\SM64\\log.txt";
static const char SAVE_DIR[] = "\\Flash2\\SM64";

// ---------------------------------------------------------------- logging and time

static double perfPeriod;   // seconds per performance-counter tick; 0 until the frequency is known

// A non-negative 64-bit count as a double from its halves: the compiler's own conversion
// (__i64tod) is a software floating point call into coredll, and this runs many times a frame.
static double CountToDouble(const LARGE_INTEGER &count)
{
    return (double)(ULONG)count.HighPart * 4294967296.0 + (double)count.LowPart;
}

double ZuneSeconds()
{
    LARGE_INTEGER now;
    if (perfPeriod == 0 || !QueryPerformanceCounter(&now)) return GetTickCount() / 1000.0;
    return CountToDouble(now) * perfPeriod;
}

void ZuneLog(const char *format, ...)
{
    if (!ZUNE_LOG) return;
    char line[900];   // the statistics line is ~700 characters
    int used = _snprintf(line, sizeof(line) - 3, "%9.3f ", ZuneSeconds());
    if (used < 0) used = 0;
    va_list args;
    va_start(args, format);
    int n = _vsnprintf(line + used, sizeof(line) - 3 - used, format, args);
    va_end(args);
    used = n < 0 ? (int)sizeof(line) - 3 : used + n;
    line[used++] = '\r';
    line[used++] = '\n';
    HANDLE file = CreateFile(LOG_FILE, GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    SetFilePointer(file, 0, NULL, FILE_END);
    DWORD written = 0;
    WriteFile(file, line, used, &written, NULL);
    CloseHandle(file);
}

// Runs add to one log until it has been sent (zune_upload.cpp); one that nobody collects
// must not fill the Zune.
static void StartOverIfLogIsHuge()
{
    HANDLE file = CreateFile(LOG_FILE, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return;
    DWORD size = GetFileSize(file, NULL);
    CloseHandle(file);
    if (size > (1u << 20)) DeleteFile(LOG_FILE);   // also when the size is unknown (0xFFFFFFFF)
}

static const char *currentStep = "startup";
static int frameNumber;
enum { TRACED_FRAMES = 3 };

void ZuneStep(const char *step)
{
    currentStep = step;
    if (frameNumber < TRACED_FRAMES) ZuneLog("step %d: %s", frameNumber, step);
}

// The image is linked at a fixed base (/DYNAMICBASE:NO; VC9 ARM has no __ImageBase). Its text
// section bounds pick likely return addresses out of the stack; tools/symbolize.py maps them
// to functions with the linker map.
static const ULONG IMAGE_BASE = 0x10000;

// The executable's data is demand-paged from flash the first time each page is touched, and
// the first notes of each sound stalled the audio thread for hundreds of ms. Touch every page
// of the sound data once at startup. (The PE headers are not mapped at the image base on CE6,
// so the arrays are found by symbol; sound_data.c lays them out back to back.)
extern "C" u8 gSoundDataADSR[], gSoundDataRaw[], gMusicData[], gBankSetsData[];
static volatile LONG prefetchSink;

static void PrefetchSoundData()
{
    const u8 *arrays[4] = {gSoundDataADSR, gSoundDataRaw, gMusicData, gBankSetsData};
    const u8 *low = arrays[0], *high = arrays[0];
    for (int i = 1; i < 4; ++i) {
        if (arrays[i] < low) low = arrays[i];
        if (arrays[i] > high) high = arrays[i];
    }
    high += 4096;   // the last array (bank sets) is a few hundred bytes
    double start = ZuneSeconds();
    LONG sum = 0;
    __try {
        for (const volatile u8 *p = (const volatile u8 *)((ULONG)low & ~4095UL); p < high; p += 4096) sum += *p;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    prefetchSink += sum;
    ZuneLog("prefetched %lu KiB of sound data in %.0f ms", (ULONG)(high - low) / 1024, (ZuneSeconds() - start) * 1000);
}

static bool LooksLikeCode(ULONG value, ULONG textStart, ULONG textEnd)
{
    return value >= textStart && value < textEnd;
}

static void LogStackCandidates(ULONG sp, ULONG textStart, ULONG textEnd)
{
    char line[300];
    int used = 0;
    const ULONG *stack = (const ULONG *)sp;
    __try {
        for (int i = 0; i < 512 && used < 260; ++i) {
            if (!LooksLikeCode(stack[i], textStart, textEnd)) continue;
            int n = _snprintf(line + used, sizeof(line) - used - 1, " %08lx", stack[i]);
            if (n < 0) break;
            used += n;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    line[used] = 0;
    ZuneLog("stack code addresses:%s", line);
}

void ZuneLogWords(const char *title, ULONG address, int before, int after)
{
    const ULONG *start = (const ULONG *)((address & ~3UL) - before * 4);
    for (int i = 0; i < before + after; i += 8) {
        char line[120];
        int used = _snprintf(line, sizeof(line) - 1, "%s %08lx:", title, (ULONG)(start + i));
        if (used < 0) return;
        __try {
            for (int j = i; j < i + 8 && j < before + after; ++j) {
                int n = _snprintf(line + used, sizeof(line) - used - 1, " %08lx", start[j]);
                if (n < 0) break;
                used += n;
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            line[used] = 0;
            ZuneLog("%s (unreadable)", line);
            return;
        }
        line[used] = 0;
        ZuneLog("%s", line);
    }
}

// ARM B/BL (any condition but 0xF) target, or 0.
static ULONG BranchTarget(ULONG address, ULONG word)
{
    if ((word >> 28) == 0xF || (word & 0x0E000000) != 0x0A000000) return 0;
    LONG offset = (LONG)(word << 8) >> 6;
    return address + 8 + offset;
}

void ZuneDumpExports(const WCHAR *module, const WCHAR *const names[], int count, int words, int calleeWords)
{
    HMODULE handle = GetModuleHandle(module);
    char name[64];
    WideCharToMultiByte(CP_ACP, 0, module, -1, name, sizeof(name), NULL, NULL);
    ZuneLog("module %s at %p", name, handle);
    if (!handle) return;
    for (int i = 0; i < count; ++i) {
        ULONG address = (ULONG)GetProcAddress(handle, names[i]);
        WideCharToMultiByte(CP_ACP, 0, names[i], -1, name, sizeof(name), NULL, NULL);
        ZuneLog("export %s %08lx", name, address);
        if (!address) continue;
        ZuneLogWords("code", address, 0, words);
        if (!calleeWords) continue;
        // The implementation behind a short wrapper: its first BL, or a branch leaving the
        // wrapper's neighbourhood (a tail call); local branches are skipped.
        __try {
            for (int w = 0; w < words; ++w) {
                ULONG at = address + 4 * w, word = ((const ULONG *)address)[w];
                ULONG target = BranchTarget(at, word);
                bool call = (word & 0x0F000000) == 0x0B000000;
                if (target && (call || target < address || target >= address + 0x400)) {
                    ZuneLog("callee of %s at %08lx -> %08lx", name, at, target);
                    ZuneLogWords("code", target, 0, calleeWords);
                    break;
                }
            }
        } __except (EXCEPTION_EXECUTE_HANDLER) {
        }
    }
}

// The last exception, for the message wWinMain shows.
static ULONG exceptionCode, exceptionPc, exceptionLr;

LONG ZuneLogException(EXCEPTION_POINTERS *info)
{
    const EXCEPTION_RECORD *record = info->ExceptionRecord;
    const CONTEXT *c = info->ContextRecord;
    exceptionCode = record->ExceptionCode;
    exceptionPc = c->Pc;
    exceptionLr = c->Lr;
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)IMAGE_BASE;
    ULONG textStart = 0x11000, textEnd = 0x200000;   // fallback if the headers are not mapped
    __try {
        const IMAGE_NT_HEADERS *nt = (const IMAGE_NT_HEADERS *)((const char *)dos + dos->e_lfanew);
        textStart = (ULONG)dos + nt->OptionalHeader.BaseOfCode;
        textEnd = textStart + nt->OptionalHeader.SizeOfCode;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    ZuneLog("native exception 0x%08lx during \"%s\" (frame %d): pc %08lx lr %08lx sp %08lx, data address %08lx",
            record->ExceptionCode, currentStep, frameNumber, c->Pc, c->Lr, c->Sp,
            record->NumberParameters > 1 ? record->ExceptionInformation[1] : 0);
    ZuneLog("r0 %08lx r1 %08lx r2 %08lx r3 %08lx r4 %08lx r5 %08lx r6 %08lx r7 %08lx", c->R0, c->R1, c->R2, c->R3,
            c->R4, c->R5, c->R6, c->R7);
    ZuneLog("r8 %08lx r9 %08lx r10 %08lx r11 %08lx r12 %08lx image %p text %08lx-%08lx", c->R8, c->R9, c->R10, c->R11,
            c->R12, dos, textStart, textEnd);
    LogStackCandidates(c->Sp, textStart, textEnd);
    // Faults inside system DLLs: dump their code for offline disassembly (symbolize.py).
    if (!LooksLikeCode(c->Pc, textStart, textEnd)) {
        ZuneLogWords("code", c->Pc, 48, 16);
        if (!LooksLikeCode(c->Lr, textStart, textEnd)) ZuneLogWords("code", c->Lr, 24, 8);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

#if ZUNE_PROFILE
// Where coredll's routines that the game still calls live, so tools/profile.py --log can
// name profile samples that land in them instead of reporting anonymous system code.
static void LogCoredllExports()
{
    static const WCHAR *const names[] = {
        L"sqrtf", L"fabsf", L"floorf", L"ceilf", L"sqrt", L"sin", L"cos", L"floor", L"ceil", L"fabs", L"pow",
        L"memcpy", L"memset", L"memmove", L"memcmp", L"malloc", L"free", L"realloc",
        L"__rt_sdiv", L"__rt_udiv", L"__rt_udiv64by64", L"__rt_sdiv64by64", L"__i64tod", L"__u64tod",
        L"EnterCriticalSection", L"LeaveCriticalSection", L"QueryPerformanceCounter", L"GetTickCount", L"Sleep",
        L"PeekMessageW", L"GetThreadContext", L"SuspendThread", L"ResumeThread", L"WaitForSingleObject"};
    ZuneDumpExports(L"coredll.dll", names, sizeof(names) / sizeof(names[0]), 0, 0);
}
#endif

// ---------------------------------------------------------------- pc_main.c globals

extern "C" {
OSMesg D_80339BEC;
OSMesgQueue gSIEventMesgQueue;
s8 gResetTimer;
s8 D_8032C648;
s8 gDebugLevelSelect;
s8 gShowProfiler;
s8 gShowDebugText;
s32 gRumblePakPfs;
struct RumbleData gRumbleDataQueue[3];
struct StructSH8031D9B0 gCurrRumbleSettings;
struct PCCLIOptions gCLIOpts;

void gfx_run(Gfx *commands);
void create_next_audio_buffer(s16 *samples, u32 num_samples);
void game_loop_one_iteration(void);
}

static struct AudioAPI *audio_api;
static bool inited;

// Per-frame cost split, logged every 150 frames.
static double statGame, statRender, statPresent, statFrame, statWorst;
static double statWindowStart;   // ZuneSeconds when the current window of frames began
static int statFrames;
static DWORD statIdleStart;   // GetIdleTime (ms the CPU was idle since boot) at the last report
static double statGameCpu, statAudioCpu;   // thread CPU seconds at the last report
extern "C" unsigned zune_stat_vertices, zune_stat_triangles_in;   // gfx_pc.c (patch 0011)
extern "C" unsigned zune_stat_texture_rebinds_skipped;             // gfx_pc.c (patch 0012)

// User + kernel CPU seconds a thread has used (GetThreadTimes).
static double ThreadCpuSeconds(HANDLE thread)
{
    FILETIME created, exited, kernel, user;
    if (!thread || !GetThreadTimes(thread, &created, &exited, &kernel, &user)) return 0;
    ULARGE_INTEGER k, u;
    k.LowPart = kernel.dwLowDateTime; k.HighPart = kernel.dwHighDateTime;
    u.LowPart = user.dwLowDateTime; u.HighPart = user.dwHighDateTime;
    return (double)(k.QuadPart + u.QuadPart) / 1e7;
}
static bool skipRender;       // frame skipping (see zune_wapi_start_frame)
static int statSkipped;
static ZuneFramePacer *framePacer;

// VFP "RunFast" mode for the calling thread: results too small for a normal float become zero
// and no floating point exception traps. sm64's asymptotic approaches (camera, speeds) decay
// into denormals, which an ARM1136's VFP otherwise hands to slow support code; the N64 ran
// the game with flush-to-zero as well (libultra sets FPCSR_FS).
static void EnableVfpRunFast(const char *thread)
{
    const unsigned FLUSH_TO_ZERO = 1u << 24, DEFAULT_NAN = 1u << 25, TRAP_ENABLES = 0x00009F00u;
    unsigned before = zune_fpscr_get();
    zune_fpscr_set((before | FLUSH_TO_ZERO | DEFAULT_NAN) & ~TRAP_ENABLES);
    ZuneLog("%s thread VFP status/control %08x -> %08x (flush-to-zero)", thread, before, zune_fpscr_get());
}

// sm64's audio state is shared between the game thread (sound requests, music changes) and
// the audio thread (synthesis). The game thread holds this lock only while running game
// logic, never while rendering, presenting or sleeping; without it the high-priority audio
// thread preempted a music change half-way and crashed in reclaim_notes. CE critical
// sections pass the audio thread's priority on to the holder.
static CRITICAL_SECTION audioLock;
static double statLockWait;   // game thread time spent waiting for audioLock

static void LockAudioState()
{
    double start = ZuneSeconds();
    EnterCriticalSection(&audioLock);
    statLockWait += ZuneSeconds() - start;
}

extern "C" void dispatch_audio_sptask(struct SPTask *spTask) { (void)spTask; }

extern "C" void set_vblank_handler(s32 index, struct VblankHandler *handler, OSMesgQueue *queue, OSMesg *msg)
{
    (void)index; (void)handler; (void)queue; (void)msg;
}

extern "C" void send_display_list(struct SPTask *spTask)
{
    if (!inited) return;
    double start = ZuneSeconds();
    ZuneStep("gfx_run");
    zunePhase = ZUNE_PHASE_RENDER;
    LeaveCriticalSection(&audioLock);   // rendering does not touch audio state
    gfx_run((Gfx *)spTask->task.t.data_ptr);
    LockAudioState();
    zunePhase = ZUNE_PHASE_GAME;
    ZuneStep("game logic");
    statRender += ZuneSeconds() - start;
}

#define SAMPLES_HIGH 544
#define SAMPLES_LOW 528

// Audio has its own above-normal-priority thread, as on the N64 (thread4 ran above the game's
// thread5). It keeps the output ring a little ahead by synthesizing one audio update at a
// time, whatever the game's frame rate, so slow frames slow the game but not the sound. Also
// as on the N64, the game thread only queues sound requests (update_game_sound runs inside
// create_next_audio_buffer once the game has ticked) and calls the sequence API directly.
// Updates alternate 544, 528, 528 frames: 533.3 on average, 60 updates per second of audio
// at 32 kHz, the rate sm64's sequence tempo assumes.
enum { AUDIO_PRIORITY = 200 };   // CE: 0 highest; applications run at 248-255
static HANDLE audioThread;
static volatile bool audioQuit;
static volatile LONG audioBusy;
static double statSynth;
static int statUpdates;

static DWORD WINAPI AudioThreadMain(LPVOID)
{
    static s16 buffer[SAMPLES_HIGH * 2];
    unsigned update = 0;
    __try {
        EnableVfpRunFast("audio");
        while (!audioQuit) {
            if (audio_api->buffered() >= audio_api->get_desired_buffered()) {
                Sleep(2);
                continue;
            }
            u32 samples = update++ % 3 == 0 ? SAMPLES_HIGH : SAMPLES_LOW;
            audioBusy = 1;
            double start = ZuneSeconds();
            EnterCriticalSection(&audioLock);
            create_next_audio_buffer(buffer, samples);
            LeaveCriticalSection(&audioLock);
            audio_api->play((u8 *)buffer, samples * 4);
            statSynth += ZuneSeconds() - start;
            ++statUpdates;
            audioBusy = 0;
        }
    } __except (ZuneLogException(GetExceptionInformation())) {
        ZuneLog("audio thread raised an exception; sound stops");
    }
    audioBusy = 0;
    return 0;
}

static void StartAudioThread()
{
    audioThread = CreateThread(NULL, 256 * 1024, AudioThreadMain, NULL, STACK_SIZE_PARAM_IS_A_RESERVATION, NULL);
    if (!audioThread) { ZuneLog("audio thread unavailable (%lu); no sound", GetLastError()); return; }
    // Above every application thread, and if the system allows, above the graphics server
    // (which preempts us during EndDraw): audio is ~15% of the CPU and must not starve.
    BOOL raised = CeSetThreadPriority(audioThread, AUDIO_PRIORITY);
    if (!raised) SetThreadPriority(audioThread, THREAD_PRIORITY_TIME_CRITICAL);
    ZuneProfileAddWorker(audioThread, &audioBusy);
    ZuneLog("audio synthesis on its own thread at priority %d (game thread %d)", CeGetThreadPriority(audioThread),
            CeGetThreadPriority(GetCurrentThread()));
}

static void StopAudioThread()
{
    if (!audioThread) return;
    audioQuit = true;
    WaitForSingleObject(audioThread, 2000);
    CloseHandle(audioThread);
    audioThread = NULL;
}

static void produce_one_frame(void)
{
    double frameStart = ZuneSeconds();
    gfx_start_frame();
    const f32 master = (f32)configMasterVolume / 127.0f;
    set_sequence_player_volume(SEQ_PLAYER_LEVEL, (f32)configMusicVolume / 127.0f * master);
    set_sequence_player_volume(SEQ_PLAYER_SFX, (f32)configSfxVolume / 127.0f * master);
    set_sequence_player_volume(SEQ_PLAYER_ENV, (f32)configEnvVolume / 127.0f * master);

    double renderBefore = statRender;
    double gameStart = ZuneSeconds();
    ZuneStep("game logic and rendering");
    zunePhase = ZUNE_PHASE_GAME;
    LockAudioState();
    game_loop_one_iteration();    // issues the frame's draws via send_display_list
    thread6_rumble_loop(NULL);
    LeaveCriticalSection(&audioLock);
    statGame += ZuneSeconds() - gameStart - (statRender - renderBefore);

    double presentStart = ZuneSeconds();
    ZuneStep("present");
    zunePhase = ZUNE_PHASE_PRESENT;
    gfx_end_frame();    // swap_buffers_end: touch overlay + ZDKGL_EndDraw
    statPresent += ZuneSeconds() - presentStart;
    zunePhase = ZUNE_PHASE_OTHER;

    double frame = ZuneSeconds() - frameStart;
    statFrame += frame;
    if (frame > statWorst) statWorst = frame;
    if (++frameNumber == TRACED_FRAMES) ZuneLog("first %d frames done; step logging off", TRACED_FRAMES);
    if (++statFrames == 150) {
        MEMORYSTATUS memory;
        memory.dwLength = sizeof(memory);
        GlobalMemoryStatus(&memory);
        ZuneRenderStats &r = zuneRenderStats;
        const int n = statFrames;
        const double ms = 1000.0 / n;
        DWORD idle = GetIdleTime();
        int underrunCount = 0, lowestBuffered = 0;
        double longestGap = 0;
        ZuneAudioTakeStats(&underrunCount, &lowestBuffered, &longestGap);
        // Where the CPU went: our two threads by GetThreadTimes, idle by GetIdleTime, and the
        // rest (the GL server process doing our draws, drivers, the input thread, the system)
        // by difference from the wall-clock time per frame, which includes the pacing sleep.
        double gameCpu = ThreadCpuSeconds(GetCurrentThread()), audioCpu = ThreadCpuSeconds(audioThread);
        double gameMs = (gameCpu - statGameCpu) * ms, audioMs = (audioCpu - statAudioCpu) * ms;
        double now = ZuneSeconds(), wallMs = (now - statWindowStart) * ms;
        double idleMs = (idle - statIdleStart) / (double)n, otherMs = wallMs - gameMs - audioMs - idleMs;
        int inputPolls = 0;
        double inputSeconds = 0;
        ZuneInputTakeStats(&inputPolls, &inputSeconds);
        ZuneLog("%d frames in %.2f s (%d drawn; load %.1f ms per drawn frame, frame skip %s): busy %.1f ms per "
                "frame (game %.1f [audio-lock wait %.2f], render %.1f, EndDraw %.1f), worst %.1f ms; CPU per frame: "
                "game thread %.1f, audio thread %.1f, graphics server+system %.1f, idle %.1f ms; input %.1f reads "
                "per frame at %.2f ms; audio %d updates, "
                "%d underruns, lowest buffer %d, longest gap %.0f ms; per frame %u vertices, %u tris in, %d tris drawn, "
                "%d draws (%u same-texture re-sets kept in the batch), %d program switches, %d array/texture calls; "
                "%d uploads; free %lu KiB",
                n, now - statWindowStart, n - statSkipped, framePacer ? framePacer->Load() * 1000 : 0.0,
                ZUNE_FRAME_SKIP == ZUNE_SKIP_NEVER ? "never" : ZUNE_FRAME_SKIP == ZUNE_SKIP_ALWAYS ? "always"
                    : framePacer && framePacer->Overloaded() ? "on" : "off",
                statFrame * ms, statGame * ms,
                statLockWait * ms, statRender * ms,
                statPresent * ms, statWorst * 1000, gameMs, audioMs, otherMs < 0 ? 0 : otherMs, idleMs,
                (double)inputPolls / n, inputPolls ? inputSeconds * 1000 / inputPolls : 0.0,
                statUpdates, underrunCount, lowestBuffered, longestGap, zune_stat_vertices / n,
                zune_stat_triangles_in / n, r.triangles / n, r.drawCalls / n, zune_stat_texture_rebinds_skipped / n,
                r.programSwitches / n, r.stateCalls / n,
                r.textureUploads, memory.dwAvailPhys / 1024);
        statWindowStart = now;
        statSkipped = 0;
        statLockWait = 0;
        statGameCpu = gameCpu;
        statAudioCpu = audioCpu;
        zune_stat_vertices = zune_stat_triangles_in = zune_stat_texture_rebinds_skipped = 0;
        statUpdates = 0;
        statIdleStart = idle;
        statGame = statRender = statPresent = statFrame = statWorst = statSynth = 0;
        statFrames = 0;
        memset(&zuneRenderStats, 0, sizeof(zuneRenderStats));
    }
}

static void game_deinit(void)
{
    StopAudioThread();
    ZuneProfileStop();
    controller_shutdown();
    if (audio_api && audio_api->shutdown) audio_api->shutdown();
    audio_api = NULL;
    gfx_shutdown();
    inited = false;
}

extern "C" void game_exit(void)
{
    ZuneLog("game_exit");
    game_deinit();
    exit(0);
}

extern "C" const char *configfile_name(void) { return "sm64config.txt"; }
extern "C" void configfile_save(const char *filename) { (void)filename; }

// ---------------------------------------------------------------- window manager API

static void zune_wapi_init(const char *title) { (void)title; }
static void zune_wapi_set_keyboard_callbacks(kb_callback_t down, kb_callback_t up, void (*all_up)(void))
{
    (void)down; (void)up; (void)all_up;
}
static void zune_wapi_main_loop(void (*run_one_game_iter)(void)) { run_one_game_iter(); }
static void zune_wapi_get_dimensions(uint32_t *width, uint32_t *height) { *width = ZUNE_VIEW_W; *height = ZUNE_VIEW_H; }

static void zune_wapi_handle_events(void)
{
    MSG message;
    for (int n = 0; n < 32 && PeekMessage(&message, NULL, 0, 0, PM_REMOVE); ++n) {
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
}

// Frame skipping, as the 3DS port does on the old 3DS: the tick's game logic still runs but
// gfx_run drops its drawing when this returns false. zune_pacing.h decides when
// (ZUNE_FRAME_SKIP in zune_settings.h).
static bool zune_wapi_start_frame(void) { return !skipRender; }
// Presenting waits for the GPU and display; it happens in gfx_end_frame (swap_buffers_end),
// after the game logic, so the audio worker can use that wait.
static void zune_wapi_swap_buffers_begin(void) {}
static void zune_wapi_swap_buffers_end(void) { ZuneRenderPresent(); }
static double zune_wapi_get_time(void) { return ZuneSeconds(); }
static void zune_wapi_shutdown(void) { ZDKGL_Cleanup(); }

static struct GfxWindowManagerAPI zune_wapi = {
    zune_wapi_init,
    zune_wapi_set_keyboard_callbacks,
    zune_wapi_main_loop,
    zune_wapi_get_dimensions,
    zune_wapi_handle_events,
    zune_wapi_start_frame,
    zune_wapi_swap_buffers_begin,
    zune_wapi_swap_buffers_end,
    zune_wapi_get_time,
    zune_wapi_shutdown
};

// ---------------------------------------------------------------- platform.h

extern "C" {
const char *sys_ropaths[] = {SAVE_DIR, NULL};

char *sys_strdup(const char *src)
{
    size_t size = strlen(src) + 1;
    char *copy = (char *)malloc(size);
    if (copy) memcpy(copy, src, size);
    return copy;
}

char *sys_strlwr(char *src)
{
    for (char *p = src; *p; ++p) if (*p >= 'A' && *p <= 'Z') *p = (char)(*p - 'A' + 'a');
    return src;
}

int sys_strcasecmp(const char *s1, const char *s2) { return _stricmp(s1, s2); }
void sys_sleep(const uint64_t us) { Sleep((DWORD)(us / 1000)); }
const char *sys_user_path(void) { return SAVE_DIR; }
const char *sys_exe_path(void) { return SAVE_DIR; }

const char *sys_file_extension(const char *fpath)
{
    const char *dot = strrchr(fpath, '.');
    const char *slash = strrchr(fpath, '\\');
    return dot && (!slash || dot > slash) ? dot + 1 : NULL;
}

const char *sys_file_name(const char *fpath)
{
    const char *slash = strrchr(fpath, '\\');
    return slash ? slash + 1 : fpath;
}

void sys_fatal(const char *fmt, ...)
{
    char text[300];
    va_list args;
    va_start(args, fmt);
    _vsnprintf(text, sizeof(text) - 1, fmt, args);
    va_end(args);
    text[sizeof(text) - 1] = 0;
    ZuneLog("FATAL %s", text);
    WCHAR wide[300];
    MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 300);
    ZDKSystem_ShowMessageBox(wide, MESSAGEBOX_TYPE_OK);
    exit(1);
}

// ---------------------------------------------------------------- fs.h subset (EEPROM save file)

// With EXTERNAL_DATA off the game only touches the filesystem for its 512-byte EEPROM image.
fs_file_t *fs_open(const char *vpath)
{
    FILE *file = fopen(fs_get_write_path(vpath), "rb");
    if (!file) return NULL;
    fs_file_t *handle = (fs_file_t *)malloc(sizeof(fs_file_t));
    if (!handle) { fclose(file); return NULL; }
    handle->handle = file;
    handle->parent = NULL;
    return handle;
}

int64_t fs_read(fs_file_t *file, void *buf, const uint64_t size)
{
    return (int64_t)fread(buf, 1, (size_t)size, (FILE *)file->handle);
}

void fs_close(fs_file_t *file)
{
    if (!file) return;
    fclose((FILE *)file->handle);
    free(file);
}

const char *fs_get_write_path(const char *vpath)
{
    static char path[SYS_MAX_PATH];
    _snprintf(path, sizeof(path) - 1, "%s\\%s", SAVE_DIR, vpath);
    path[sizeof(path) - 1] = 0;
    return path;
}
}

// ---------------------------------------------------------------- entry point

static void KeepAwake()
{
    static DWORD last;
    DWORD now = GetTickCount();
    if (now - last < 5000) return;
    last = now;
    ZDKSystem_SignalUserActivity();
    SystemIdleTimerReset();
}

static int Run()
{
    CreateDirectory(DATA_DIR, NULL);
    if (ZUNE_LOG) StartOverIfLogIsHuge();
    LARGE_INTEGER perfFrequency;
    if (QueryPerformanceFrequency(&perfFrequency) && perfFrequency.QuadPart > 0) perfPeriod = 1.0 / CountToDouble(perfFrequency);
    MEMORYSTATUS memory;
    memory.dwLength = sizeof(memory);
    GlobalMemoryStatus(&memory);
    ZuneLog("Super Mario 64 for Zune HD starting (sm64ex d7ca2c0, US); %lu KiB physical free, %lu KiB virtual free",
            memory.dwAvailPhys / 1024, memory.dwAvailVirtual / 1024);
    // tools/profile.py and tools/symbolize.py find the build's linker map from this line.
    ZuneLog("build %s; settings: %s", ZUNE_BUILD, ZUNE_SETTINGS);
    InitializeCriticalSection(&audioLock);
    EnableVfpRunFast("game");
#if ZUNE_PROFILE
    LogCoredllExports();
    ZuneProfileStart();
#endif
    PrefetchSoundData();
    ZuneStep("splash screen off");
    ZDKSystem_ShowSplashScreen(FALSE);
    ZuneStep("ZDKGL_Initialize");
    if (FAILED(ZDKGL_Initialize())) { ZuneLog("ZDKGL_Initialize failed"); return 1; }

    ZuneStep("main pool");
    void *pool = malloc(DEFAULT_POOL_SIZE);
    if (!pool) sys_fatal("Could not allocate %u bytes for the main pool.", (unsigned)DEFAULT_POOL_SIZE);
    main_pool_init(pool, (u8 *)pool + DEFAULT_POOL_SIZE);
    gEffectsMemoryPool = mem_pool_init(0x4000, MEMORY_POOL_LEFT);
    ZuneLog("main pool %u bytes at %p", (unsigned)DEFAULT_POOL_SIZE, pool);

    ZuneStep("gfx_init");
    gfx_init(&zune_wapi, &zune_rendering_api, "Super Mario 64");
    ZuneStep("audio output");
    audio_api = &zune_audio_api;
    if (!audio_api->init()) ZuneLog("audio output unavailable; continuing silently");
    ZuneStep("audio_init");
    audio_init();
    ZuneStep("sound_init");
    sound_init();
    ZuneStep("thread5_game_loop setup");
    thread5_game_loop(NULL);
    StartAudioThread();
    inited = true;
    statIdleStart = GetIdleTime();
    statWindowStart = ZuneSeconds();
    statGameCpu = ThreadCpuSeconds(GetCurrentThread());
    statAudioCpu = ThreadCpuSeconds(audioThread);
    ZuneLog("game initialised; entering frame loop");

    // The game logic runs at 30 Hz; ZDKGL_EndDraw itself waits for the ~60 Hz display.
    static ZuneFramePacer pacer(1.0 / 30.0, ZUNE_FRAME_SKIP, ZuneSeconds());
    framePacer = &pacer;
    while (!zuneExitRequested) {
        skipRender = pacer.BeginTick(ZuneSeconds());
        if (skipRender) ++statSkipped;
        produce_one_frame();
        KeepAwake();
        double sleep = pacer.EndTick(ZuneSeconds());
        zunePhase = ZUNE_PHASE_IDLE;
        if (sleep > 0) Sleep((DWORD)(sleep * 1000));
    }
    ZuneLog("exit gesture");
    StopAudioThread();
    ZuneProfileStop();   // flushes profile.bin before it is sent
    if (ZUNE_LOG) ZuneUploadLogs();
    game_deinit();
    return 0;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR, int)
{
    int result = 1;
    __try {
        result = Run();
    } __except (ZuneLogException(GetExceptionInformation())) {
        // Enough to find the fault from a photo of the screen: tools/symbolize.py turns the
        // two addresses into function names with this build's linker map.
        static char text[240];
        static WCHAR wide[240];
        _snprintf(text, sizeof(text) - 1, "Super Mario 64 stopped: exception %08lx at %08lx, called from %08lx, "
                  "during \"%s\" in frame %d of build %s.", exceptionCode, exceptionPc, exceptionLr, currentStep,
                  frameNumber, ZUNE_BUILD);
        MultiByteToWideChar(CP_ACP, 0, text, -1, wide, 240);
        wide[239] = 0;
        ZDKSystem_ShowMessageBox(wide, MESSAGEBOX_TYPE_OK);
    }
    return result;
}
