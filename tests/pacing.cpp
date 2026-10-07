// Test of the frame pacing and frame-skip policy
// (platform/zune_pacing.h), on a simulated clock.
#include <assert.h>
#include <stdio.h>
#include "../platform/zune_pacing.h"

static const double PERIOD = 1.0 / 30.0;
static const double SKIPPED_COST = 0.009;   // a tick whose drawing is skipped still runs the game logic

struct Run {
    int ticks, skipped, longestSkipRun;
    double elapsed;
};

// Runs ticks whose drawn cost comes from cost(tick), until `ticks` are done.
template <class Cost>
static Run Simulate(ZuneFramePacer &pacer, double &now, int ticks, Cost cost)
{
    Run run = {0, 0, 0, 0};
    double start = now;
    int skipRun = 0;
    for (int i = 0; i < ticks; ++i) {
        bool skip = pacer.BeginTick(now);
        now += skip ? SKIPPED_COST : cost(i);
        skipRun = skip ? skipRun + 1 : 0;
        if (skipRun > run.longestSkipRun) run.longestSkipRun = skipRun;
        run.skipped += skip;
        ++run.ticks;
        double sleep = pacer.EndTick(now);
        assert(sleep >= 0 && sleep <= PERIOD + 1e-9);
        now += sleep;
    }
    run.elapsed = now - start;
    return run;
}

struct Steady {
    double seconds;
    explicit Steady(double s) : seconds(s) {}
    double operator()(int) const { return seconds; }
};

// `base` except ticks [from, from + count), which cost `burst`.
struct Burst {
    double base, burst;
    int from, count;
    Burst(double b, double u, int f, int c) : base(b), burst(u), from(f), count(c) {}
    double operator()(int i) const { return i >= from && i < from + count ? burst : base; }
};

static double Speed(const Run &run) { return run.ticks * PERIOD / run.elapsed; }   // 1 = real time

static void TestLightLoad()
{
    for (int mode = ZUNE_SKIP_NEVER; mode <= ZUNE_SKIP_ALWAYS; ++mode) {
        double now = 100;
        ZuneFramePacer pacer(PERIOD, mode, now);
        Run run = Simulate(pacer, now, 600, Steady(0.020));
        assert(run.skipped == 0 && !pacer.Overloaded());
        assert(Speed(run) > 0.999 && Speed(run) < 1.001);
    }
}

static void TestSustainedLoad()
{
    // Bob-omb Battlefield before the libm fix: 42 ms per drawn tick, 126% load.
    double now = 5;
    ZuneFramePacer never(PERIOD, ZUNE_SKIP_NEVER, now);
    Run slow = Simulate(never, now, 600, Steady(0.042));
    assert(slow.skipped == 0 && Speed(slow) > 0.78 && Speed(slow) < 0.80);   // slow motion, 33.3 / 42

    now = 5;
    ZuneFramePacer pacer(PERIOD, ZUNE_SKIP_SUSTAINED, now);
    Simulate(pacer, now, 120, Steady(0.025));   // a light area first
    assert(!pacer.Overloaded());
    Run entering = Simulate(pacer, now, 12, Steady(0.042));
    assert(entering.skipped == 0 && !pacer.Overloaded());           // not yet: could be a burst
    Simulate(pacer, now, 18, Steady(0.042));
    assert(pacer.Overloaded());                                     // within a second it is sustained
    Run heavy = Simulate(pacer, now, 900, Steady(0.042));
    assert(Speed(heavy) > 0.99 && Speed(heavy) < 1.01);             // real time again
    assert(heavy.longestSkipRun <= ZuneFramePacer::MAX_SKIPS_IN_A_ROW);
    double drawn = 1.0 - (double)heavy.skipped / heavy.ticks;
    assert(drawn > 0.70 && drawn < 0.78);                           // (33.3 - 9) / (42 - 9) = 74%
    Simulate(pacer, now, 60, Steady(0.025));                        // leaving the area
    assert(!pacer.Overloaded());
    printf("sustained 126%% load: %.0f%% of ticks drawn at %.3fx speed\n", drawn * 100, Speed(heavy));

    // Too slow even with two skips in three ticks: every third tick is still drawn and the
    // game slows down, (150 + 9 + 9) ms for three ticks.
    for (int mode = ZUNE_SKIP_SUSTAINED; mode <= ZUNE_SKIP_ALWAYS; ++mode) {
        now = 5;
        ZuneFramePacer crushed(PERIOD, mode, now);
        Simulate(crushed, now, 60, Steady(0.150));
        Run run = Simulate(crushed, now, 600, Steady(0.150));
        assert(run.longestSkipRun == ZuneFramePacer::MAX_SKIPS_IN_A_ROW && run.skipped == 400);
        assert(Speed(run) > 0.58 && Speed(run) < 0.61);
    }
}

static void TestBurstsAreNotSkipped()
{
    // Short bursts in a light area: slow motion for a moment, no dropped frames.
    // alwaysSkips: whether unconditional skipping drops frames for the same burst. A single
    // long hitch is forgiven in every mode.
    static const struct { double cost; int count; bool alwaysSkips; } bursts[] = {
        {0.045, 10, true}, {0.040, 16, true}, {0.060, 4, true}, {0.080, 3, true}, {0.300, 1, false}, {0.800, 1, false}};
    for (unsigned b = 0; b < sizeof(bursts) / sizeof(bursts[0]); ++b) {
        double now = 0;
        ZuneFramePacer pacer(PERIOD, ZUNE_SKIP_SUSTAINED, now);
        Run run = Simulate(pacer, now, 400, Burst(0.025, bursts[b].cost, 200, bursts[b].count));
        assert(run.skipped == 0);
        now = 0;
        ZuneFramePacer always(PERIOD, ZUNE_SKIP_ALWAYS, now);
        Run old = Simulate(always, now, 400, Burst(0.025, bursts[b].cost, 200, bursts[b].count));
        assert((old.skipped > 0) == bursts[b].alwaysSkips);
    }
    // A level-load hitch while skipping is active is forgiven, not fast-forwarded.
    double now = 0;
    ZuneFramePacer pacer(PERIOD, ZUNE_SKIP_SUSTAINED, now);
    Simulate(pacer, now, 300, Steady(0.042));
    assert(pacer.Overloaded());
    Run hitch = Simulate(pacer, now, 3, Burst(0.042, 0.800, 0, 1));
    assert(hitch.skipped <= 1);
    Run after = Simulate(pacer, now, 300, Steady(0.042));
    assert(Speed(after) > 0.99 && Speed(after) < 1.01);
}

int main()
{
    TestLightLoad();
    TestSustainedLoad();
    TestBurstsAreNotSkipped();
    printf("pacing tests passed\n");
    return 0;
}
