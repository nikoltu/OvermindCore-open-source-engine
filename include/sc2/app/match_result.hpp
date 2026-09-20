#pragma once

#include "sc2/sim/world.hpp"

namespace sc2::app {

struct MatchResult {
    enum class Outcome {
        Ongoing,
        Victory,
        Defeat,
        Draw,
    };

    static Outcome evaluate(const sim::World& w, int human_player, int stalemate_ticks = 0);

    static const char* banner(Outcome o);
};

}
