#ifndef ZUNE_PACING_H
#define ZUNE_PACING_H
// 30 Hz frame pacing and frame skipping for the SM64 port. Platform-neutral so
// tests/pacing.cpp checks it on the host.
//
// sm64's logic is tied to 30 ticks a second. When a tick (logic + drawing) takes longer than
// that, the port can either let the game run in slow motion, as the N64 did, or keep real
// time by running the next tick's logic without drawing it (frame skipping, as the 3DS port
// does). Skipping is right for an area that is slow for seconds on end, where slow motion
// would drag; it is wrong for a short burst of lag, where a dropped frame is a visible jump
// and the slowdown would hardly be noticed. So the default skips only under sustained load:
//
//   ZUNE_SKIP_NEVER      never skip; slow areas run in slow motion
//   ZUNE_SKIP_SUSTAINED  skip only once drawn ticks have been too slow for about half a second
//   ZUNE_SKIP_ALWAYS     skip whenever a tick starts more than one period late
enum { ZUNE_SKIP_NEVER = 0, ZUNE_SKIP_SUSTAINED = 1, ZUNE_SKIP_ALWAYS = 2 };

class ZuneFramePacer {
public:
    ZuneFramePacer(double period, int mode, double now)
        : period_(period), mode_(mode), next_(now), tickStart_(now), load_(period * 0.5), overloaded_(false),
          skip_(false), skipsInARow_(0), hitches_(0) {}

    // At the top of a tick. True: run the logic but do not draw this tick.
    bool BeginTick(double now) {
        tickStart_ = now;
        skip_ = Skipping() && now - next_ > period_ && skipsInARow_ < MAX_SKIPS_IN_A_ROW;
        skipsInARow_ = skip_ ? skipsInARow_ + 1 : 0;
        return skip_;
    }

    // When the tick is done. Returns the seconds to sleep before the next one.
    double EndTick(double now) {
        if (!skip_) {
            // Only drawn ticks say what a full frame costs. One long hitch (a level's textures
            // loading) counts as two periods at most, so it cannot look like sustained load.
            double cost = now - tickStart_;
            if (cost > 2 * period_) cost = 2 * period_;
            load_ += (cost - load_) * Smoothing();
            if (load_ > period_ * Engage()) overloaded_ = true;
            else if (load_ < period_ * Release()) overloaded_ = false;
        }
        next_ += period_;
        bool late = next_ <= now, hitch = late && now - next_ > period_ * MaxDebt();
        if (!skip_ && !hitch) hitches_ = 0;
        if (!late) return next_ - now;
        // Late. Without skipping there is nothing to catch up with: the game just ran slow.
        if (!Skipping()) next_ = now;
        // With it, a hitch of several periods (a level's textures loading) is forgiven instead
        // of fast-forwarded; but if every drawn tick is that slow, it is load after all, and
        // the debt is only capped.
        else if (hitch) next_ = !skip_ && hitches_++ < MAX_FORGIVEN_HITCHES ? now : now - period_ * MaxDebt();
        return 0;
    }

    bool Overloaded() const { return overloaded_; }
    double Load() const { return load_; }   // smoothed seconds per drawn tick

    // The screen updates at least every third tick.
    enum { MAX_SKIPS_IN_A_ROW = 2, MAX_FORGIVEN_HITCHES = 2 };

private:
    bool Skipping() const { return mode_ == ZUNE_SKIP_ALWAYS || (mode_ == ZUNE_SKIP_SUSTAINED && overloaded_); }

    // Smoothing 1/16 reaches the engage level after ~15 ticks at 125% load, ~30 at 115%.
    // Between Release and Engage the state holds: up to 10% slow motion is preferred to an
    // occasional dropped frame. MaxDebt is in periods.
    static double Smoothing() { return 1.0 / 16; }
    static double Engage() { return 1.10; }
    static double Release() { return 1.0; }
    static double MaxDebt() { return 3.0; }

    double period_;
    int mode_;
    double next_;        // when the current tick was due to start
    double tickStart_;
    double load_;
    bool overloaded_, skip_;
    int skipsInARow_;
    int hitches_;        // drawn ticks in a row that ended more than MaxDebt periods late
};

#endif
