#pragma once

#include "sc2/sim/world.hpp"

namespace sc2::app {

class GameClock {
public:
    enum class Speed { Slower, Slow, Normal, Fast, Faster };

    void set_speed(Speed s) { speed_ = s; }
    Speed speed() const { return speed_; }
    void set_paused(bool p) { paused_ = p; }
    bool paused() const { return paused_; }
    void toggle_pause() { paused_ = !paused_; }

    int advance(double real_dt, int max_steps = 8) {
        if (paused_ || real_dt <= 0.0) return 0;
        accum_ += real_dt * rate();
        int steps = int(accum_);
        if (steps < 0) steps = 0;
        if (steps > max_steps) {
            steps = max_steps;
            accum_ = 0.0;
        } else {
            accum_ -= steps;
        }
        return steps;
    }
    void reset() { accum_ = 0.0; }

    double rate() const {
        return double(sim::World::kTicksPerSecond) * multiplier(speed_);
    }
    static double multiplier(Speed s) {
        switch (s) {
            case Speed::Slower: return 0.5;
            case Speed::Slow: return 0.75;
            case Speed::Normal: return 1.0;
            case Speed::Fast: return 1.21;
            case Speed::Faster: return 1.4;
        }
        return 1.0;
    }

private:
    Speed speed_ = Speed::Faster;
    bool paused_ = false;
    double accum_ = 0.0;
};

}
