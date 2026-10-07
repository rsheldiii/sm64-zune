// Statistical profiler (ZUNE_PROFILE=on): a time-critical thread suspends the game thread
// about once per millisecond (and the audio worker while it is busy), records pc, lr and frame
// phase, and appends batches of samples to \Flash2\SM64\profile.bin. The file is sent with
// the log on exit; tools/profile.py maps the samples to functions with the linker map.
// Cost: one Suspend/GetThreadContext/Resume per sample.
#include <windows.h>
#include "zune_settings.h"
#include "zune_platform.h"

volatile LONG zunePhase;

#if !ZUNE_PROFILE
void ZuneProfileStart() {}
void ZuneProfileAddWorker(HANDLE, volatile LONG *) {}
void ZuneProfileStop() {}
#else
static const WCHAR PROFILE_FILE[] = L"\\Flash2\\SM64\\profile.bin";
enum { BATCH = 4096, MAX_SAMPLES = 256 * 1024 };   // ~4.5 min at 1 kHz, 3 MiB

struct ProfileSample {
    ULONG pc, lr, phase;
};

static ProfileSample batch[BATCH];
static HANDLE target, sampler, worker;
static volatile LONG *volatile workerBusy;
static volatile bool stopSampling;

static bool Sample(HANDLE thread, ULONG phase, ProfileSample &out)
{
    CONTEXT context;
    if (SuspendThread(thread) == 0xFFFFFFFF) return false;
    context.ContextFlags = CONTEXT_CONTROL;
    BOOL ok = GetThreadContext(thread, &context);
    ResumeThread(thread);
    out.pc = context.Pc;
    out.lr = context.Lr;
    out.phase = phase;
    return ok != FALSE;
}

static DWORD WINAPI SamplerMain(LPVOID)
{
    HANDLE file = CreateFile(PROFILE_FILE, GENERIC_WRITE, FILE_SHARE_READ, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (file == INVALID_HANDLE_VALUE) return 0;
    int used = 0;
    DWORD total = 0, failures = 0;
    while (!stopSampling && total < MAX_SAMPLES) {
        Sleep(1);
        for (int t = 0; t < 2 && used < BATCH; ++t) {
            bool isWorker = t == 1;
            if (isWorker && !(worker && workerBusy && *workerBusy)) continue;
            ULONG phase = isWorker ? ZUNE_PHASE_WORKER | ZUNE_PHASE_AUDIO : (ULONG)zunePhase;
            if (Sample(isWorker ? worker : target, phase, batch[used])) ++used;
            else ++failures;
        }
        if (used == BATCH) {
            DWORD written = 0;
            WriteFile(file, batch, sizeof(batch), &written, NULL);
            FlushFileBuffers(file);
            total += BATCH;
            used = 0;
        }
    }
    DWORD written = 0;
    if (used) WriteFile(file, batch, used * sizeof(batch[0]), &written, NULL);
    CloseHandle(file);
    ZuneLog("profiler stopped: %lu samples, %lu context failures", total + used, failures);
    return 0;
}

void ZuneProfileStart()
{
    target = OpenThread(0, FALSE, GetCurrentThreadId());
    if (!target) { ZuneLog("profiler: OpenThread failed (%lu)", GetLastError()); return; }
    sampler = CreateThread(NULL, 64 * 1024, SamplerMain, NULL, 0, NULL);
    if (!sampler) { ZuneLog("profiler: CreateThread failed (%lu)", GetLastError()); return; }
    SetThreadPriority(sampler, THREAD_PRIORITY_TIME_CRITICAL);
    ZuneLog("profiler sampling the game thread every ~1 ms into profile.bin");
}

void ZuneProfileAddWorker(HANDLE thread, volatile LONG *busy)
{
    workerBusy = busy;
    worker = thread;
}

void ZuneProfileStop()
{
    if (!sampler) return;
    stopSampling = true;
    WaitForSingleObject(sampler, 3000);
    CloseHandle(sampler);
    sampler = NULL;
}
#endif
