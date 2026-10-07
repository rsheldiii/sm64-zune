// AudioAPI for the Zune HD: 32 kHz stereo 16-bit output of sm64ex's synthesized audio, fed
// one audio update at a time by the audio thread (zune_main.cpp). Any failure or exception
// turns output off; the game continues silently.
//
// Preferred backend: CE's waveOut (resolved from coredll at run time). The driver marks
// each queued buffer done when it has played, so pacing follows the real output clock and
// an underrun plays silence.
//
// Fallback: ZDKAudio (zdksystem.dll), whose OpenZDK header is wrong for the HD. From device
// code dumps (tools/symbolize.py --disassemble on a log of the exports' code):
//  - ZDKAudio_Initialize(void **engine) stores the new engine through its argument;
//  - ZDKAudio_CreateVoice(engine, const WAVEFORMATEX *, HVOICE *);
//  - ZDKAudio_SubmitPacket(voice, const ZunePacket *) copies the 20-byte packet into the
//    voice's single packet slot (no queue) and only while the voice is stopped (state 4,
//    else 0x8BAD0002); loop bounds are checked when loopCount != 0;
//  - ZDKAudio_Play(voice) needs a submitted packet (else 0x8BAD0003);
//  - ZDKAudio_GetVoiceState(voice, &state): state 1 playing, 2 stopping, 4 stopped, |8 paused.
// A voice plays one buffer and nothing reports its position, so that backend loops one ring
// buffer and writes ahead of a wall-clock estimate of the read position, keeping the space
// just ahead of the writer silent.
#include <windows.h>
#include <mmsystem.h>
#include <string.h>
#include <zdk.h>
extern "C" {
#include "pc/audio/audio_api.h"
}
#include "zune_platform.h"

enum { SAMPLE_RATE = 32000, UPDATE_FRAMES_MAX = 544 };

bool zuneAudioActive;

// CE declares WAVEFORMATEX byte-packed (alignment 1); zdksystem reads it with word loads.
static union {
    WAVEFORMATEX wave;
    DWORD align[(sizeof(WAVEFORMATEX) + 3) / 4];
} format;

// Underrun and scheduling diagnostics, reported with the frame stats (ZuneAudioTakeStats).
static int underruns, loggedUnderruns;
static double minBuffered = 1e9, maxGap, lastPlay;

void ZuneAudioTakeStats(int *underrunCount, int *lowestBuffered, double *longestGapMs)
{
    *underrunCount = underruns;
    *lowestBuffered = minBuffered < 1e9 ? (int)minBuffered : -1;
    *longestGapMs = maxGap * 1000;
    underruns = 0;
    minBuffered = 1e9;
    maxGap = 0;
}

static void NoteUpdate(double buffered, bool underrun)
{
    double now = ZuneSeconds();
    if (lastPlay && now - lastPlay > maxGap) maxGap = now - lastPlay;
    lastPlay = now;
    if (buffered < minBuffered) minBuffered = buffered;
    if (!underrun) return;
    ++underruns;
    if (++loggedUnderruns <= 40)
        ZuneLog("audio underrun: output ran dry; longest update gap so far %.0f ms", maxGap * 1000);
}

// ---------------------------------------------------------------- waveOut backend

enum { WAVE_BUFFERS = 8, WAVE_TARGET = 5 * 533 };   // 8 updates of capacity, ~83 ms queued

typedef MMRESULT (WINAPI *WaveOutOpenFn)(LPHWAVEOUT, UINT, LPCWAVEFORMATEX, DWORD, DWORD, DWORD);
typedef MMRESULT (WINAPI *WaveOutHeaderFn)(HWAVEOUT, LPWAVEHDR, UINT);
typedef MMRESULT (WINAPI *WaveOutFn)(HWAVEOUT);

static struct {
    WaveOutOpenFn open;
    WaveOutHeaderFn prepare, unprepare, write;
    WaveOutFn reset, close;
} wave;
static HWAVEOUT waveOut;
static WAVEHDR waveHeaders[WAVE_BUFFERS];
static DWORD waveData[WAVE_BUFFERS][UPDATE_FRAMES_MAX];
static bool waveQueued[WAVE_BUFFERS], waveStarted;

static bool WaveInit()
{
    HMODULE coredll = GetModuleHandle(L"coredll.dll");
    if (!coredll) return false;
    wave.open = (WaveOutOpenFn)GetProcAddress(coredll, L"waveOutOpen");
    wave.prepare = (WaveOutHeaderFn)GetProcAddress(coredll, L"waveOutPrepareHeader");
    wave.unprepare = (WaveOutHeaderFn)GetProcAddress(coredll, L"waveOutUnprepareHeader");
    wave.write = (WaveOutHeaderFn)GetProcAddress(coredll, L"waveOutWrite");
    wave.reset = (WaveOutFn)GetProcAddress(coredll, L"waveOutReset");
    wave.close = (WaveOutFn)GetProcAddress(coredll, L"waveOutClose");
    if (!wave.open || !wave.prepare || !wave.unprepare || !wave.write || !wave.reset || !wave.close) {
        ZuneLog("waveOut: coredll lacks the API");
        return false;
    }
    MMRESULT result = wave.open(&waveOut, WAVE_MAPPER, &format.wave, 0, 0, CALLBACK_NULL);
    if (result != MMSYSERR_NOERROR) {
        ZuneLog("waveOutOpen(32 kHz stereo) failed: %u", result);
        waveOut = NULL;
        return false;
    }
    for (int i = 0; i < WAVE_BUFFERS; ++i) {
        WAVEHDR &h = waveHeaders[i];
        memset(&h, 0, sizeof(h));
        h.lpData = (LPSTR)waveData[i];
        h.dwBufferLength = sizeof(waveData[i]);
        result = wave.prepare(waveOut, &h, sizeof(h));
        if (result != MMSYSERR_NOERROR) {
            ZuneLog("waveOutPrepareHeader failed: %u", result);
            return false;
        }
    }
    ZuneLog("audio output: waveOut, %d buffers of up to %d frames, ~%d frames queued", WAVE_BUFFERS,
            UPDATE_FRAMES_MAX, WAVE_TARGET);
    return true;
}

static int WaveBuffered()
{
    int frames = 0;
    for (int i = 0; i < WAVE_BUFFERS; ++i)
        if (waveQueued[i] && !(waveHeaders[i].dwFlags & WHDR_DONE)) frames += waveHeaders[i].dwBufferLength / 4;
    return frames;
}

static void WavePlay(const uint8_t *buf, size_t len)
{
    int buffered = WaveBuffered();
    NoteUpdate(buffered, waveStarted && buffered == 0);
    for (int i = 0; i < WAVE_BUFFERS; ++i) {
        WAVEHDR &h = waveHeaders[i];
        if (waveQueued[i] && !(h.dwFlags & WHDR_DONE)) continue;
        if (len > sizeof(waveData[i])) len = sizeof(waveData[i]);
        memcpy(waveData[i], buf, len);
        h.dwBufferLength = (DWORD)len;
        MMRESULT result = wave.write(waveOut, &h, sizeof(h));
        if (result != MMSYSERR_NOERROR) {
            ZuneLog("waveOutWrite failed: %u; audio off", result);
            zuneAudioActive = false;
            return;
        }
        waveQueued[i] = true;
        waveStarted = true;
        return;
    }
    // Every buffer is queued: the caller ran ahead of desired_buffered. Drop this update.
}

static void WaveShutdown()
{
    if (!waveOut) return;
    wave.reset(waveOut);
    for (int i = 0; i < WAVE_BUFFERS; ++i) wave.unprepare(waveOut, &waveHeaders[i], sizeof(waveHeaders[i]));
    wave.close(waveOut);
    waveOut = NULL;
}

// ---------------------------------------------------------------- ZDKAudio ring backend

enum {
    RING_FRAMES = 32768,     // ~1 s of stereo frames
    RING_TARGET = 3072,      // ~96 ms of latency
    SILENCE_FRAMES = 16384,  // kept silent ahead of the writer: a stall up to ~0.5 s plays silence
    LOOP_INFINITE = 255      // XAudio2's convention
};

struct ZunePacket {
    const BYTE *data;
    DWORD size;           // bytes, a multiple of nBlockAlign
    DWORD loopBegin;      // frames
    DWORD loopLength;     // frames
    BYTE loopCount;
    BYTE pad[3];
};

typedef HRESULT (WINAPI *InitializeFn)(void **engine);
typedef HRESULT (WINAPI *ShutdownFn)(void *engine);
typedef HRESULT (WINAPI *CreateVoiceFn)(void *engine, const WAVEFORMATEX *format, HVOICE *voice);
typedef HRESULT (WINAPI *SubmitPacketFn)(HVOICE voice, const ZunePacket *packet);

static void *engine;
static HVOICE voice;
static DWORD ring[RING_FRAMES];
static ZunePacket packet;
static double startTime;
static double writeFrames;          // frames written since startTime (ahead of the reader)
static double clearedTo;            // ring frames in [writeFrames, clearedTo) hold silence

static double PlayedFrames() { return (ZuneSeconds() - startTime) * SAMPLE_RATE; }

static void ClearRing(double from, int frames)
{
    unsigned position = (unsigned)((unsigned long)from % RING_FRAMES);
    while (frames > 0) {
        int run = RING_FRAMES - (int)position;
        if (run > frames) run = frames;
        memset(&ring[position], 0, run * sizeof(ring[0]));
        frames -= run;
        position = 0;
    }
}

static void KeepSilenceAhead()
{
    if (clearedTo < writeFrames) clearedTo = writeFrames;
    double want = writeFrames + SILENCE_FRAMES;
    if (want > clearedTo) {
        ClearRing(clearedTo, (int)(want - clearedTo));
        clearedTo = want;
    }
}

static bool RingInit()
{
    HRESULT hr = ((InitializeFn)ZDKAudio_Initialize)(&engine);
    if (FAILED(hr)) { ZuneLog("ZDKAudio_Initialize failed 0x%08lx", (unsigned long)hr); return false; }
    hr = ((CreateVoiceFn)ZDKAudio_CreateVoice)(engine, &format.wave, &voice);
    if (FAILED(hr) || !voice) { ZuneLog("ZDKAudio_CreateVoice failed 0x%08lx", (unsigned long)hr); return false; }
    memset(ring, 0, sizeof(ring));
    memset(&packet, 0, sizeof(packet));
    packet.data = (const BYTE *)ring;
    packet.size = sizeof(ring);
    packet.loopLength = RING_FRAMES;
    packet.loopCount = LOOP_INFINITE;
    hr = ((SubmitPacketFn)ZDKAudio_SubmitPacket)(voice, &packet);
    if (FAILED(hr)) { ZuneLog("ZDKAudio_SubmitPacket failed 0x%08lx", (unsigned long)hr); return false; }
    hr = ZDKAudio_Play(voice);
    if (FAILED(hr)) { ZuneLog("ZDKAudio_Play failed 0x%08lx", (unsigned long)hr); return false; }
    startTime = ZuneSeconds();
    writeFrames = RING_TARGET;
    clearedTo = RING_FRAMES;
    ZuneLog("audio output: ZDKAudio looping %d-frame ring (clock-estimated position)", RING_FRAMES);
    return true;
}

static int RingBuffered()
{
    double left = writeFrames - PlayedFrames();
    return left > 0 ? (int)left : 0;
}

static void RingPlay(const uint8_t *buf, size_t len)
{
    double played = PlayedFrames();
    NoteUpdate(writeFrames - played, writeFrames < played);
    if (writeFrames < played) {   // underrun: resume just ahead of the reader
        writeFrames = played + RING_TARGET / 2;
        KeepSilenceAhead();
    }
    int frames = (int)(len / 4);
    if (writeFrames + frames > played + RING_FRAMES - SILENCE_FRAMES) return;   // would lap the reader
    const DWORD *source = (const DWORD *)buf;
    unsigned position = (unsigned)((unsigned long)writeFrames % RING_FRAMES);
    for (int left = frames; left > 0;) {
        int run = RING_FRAMES - (int)position;
        if (run > left) run = left;
        memcpy(&ring[position], source, run * sizeof(ring[0]));
        source += run;
        left -= run;
        position = 0;
    }
    writeFrames += frames;
    KeepSilenceAhead();
}

static void RingShutdown()
{
    if (voice) {
        ZDKAudio_Stop(voice, TRUE);
        ZDKAudio_DestroyVoice(voice);
    }
    if (engine) ((ShutdownFn)ZDKAudio_Shutdown)(engine);
    engine = NULL;
    voice = NULL;
}

// ---------------------------------------------------------------- AudioAPI

enum Backend { NONE, WAVE, RING };
static Backend backend;
static double silentStart, silentWritten;   // keeps time when there is no output

static bool zune_audio_init(void)
{
    memset(&format, 0, sizeof(format));
    format.wave.wFormatTag = WAVE_FORMAT_PCM;
    format.wave.nChannels = 2;
    format.wave.nSamplesPerSec = SAMPLE_RATE;
    format.wave.wBitsPerSample = 16;
    format.wave.nBlockAlign = 4;
    format.wave.nAvgBytesPerSec = SAMPLE_RATE * 4;
    silentStart = ZuneSeconds();
    __try {
        ZuneStep("audio output: waveOut");
        if (WaveInit()) backend = WAVE;
        else {
            WaveShutdown();
            ZuneStep("audio output: ZDKAudio");
            if (RingInit()) backend = RING;
        }
    } __except (ZuneLogException(GetExceptionInformation())) {
        ZuneLog("audio init raised an exception; continuing silently");
        backend = NONE;
    }
    zuneAudioActive = backend != NONE;
    return zuneAudioActive;
}

static int zune_audio_buffered(void)
{
    if (zuneAudioActive && backend == WAVE) return WaveBuffered();
    if (zuneAudioActive && backend == RING) return RingBuffered();
    double left = silentWritten - (ZuneSeconds() - silentStart) * SAMPLE_RATE;   // no output: keep time
    return left > 0 ? (int)left : 0;
}

static int zune_audio_get_desired_buffered(void) { return backend == WAVE ? WAVE_TARGET : RING_TARGET; }

// Called from the audio thread, one audio update at a time.
static void zune_audio_play(const uint8_t *buf, size_t len)
{
    if (!zuneAudioActive) {
        double played = (ZuneSeconds() - silentStart) * SAMPLE_RATE;
        silentWritten = (silentWritten < played ? played : silentWritten) + len / 4;
        return;
    }
    __try {
        if (backend == WAVE) WavePlay(buf, len);
        else RingPlay(buf, len);
    } __except (ZuneLogException(GetExceptionInformation())) {
        ZuneLog("audio output raised an exception; audio off");
        zuneAudioActive = false;
    }
}

static void zune_audio_shutdown(void)
{
    __try {
        if (backend == WAVE) WaveShutdown();
        else if (backend == RING) RingShutdown();
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    backend = NONE;
    zuneAudioActive = false;
}

extern "C" {
struct AudioAPI zune_audio_api = {
    zune_audio_init,
    zune_audio_buffered,
    zune_audio_get_desired_buffered,
    zune_audio_play,
    zune_audio_shutdown
};
}
