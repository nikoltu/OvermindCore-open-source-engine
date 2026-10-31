#pragma once

#include <string>

#include "omc/data/strings.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

struct MatchResult {
    enum class Outcome {
        Ongoing,
        Victory,
        Defeat,
        Draw,
    };

    static Outcome evaluate(const sim::World& w, int human_player, int stalemate_ticks = 0);

    static bool viewer_defeated(const sim::World& w, int human_player);
    static bool sole_player_rule();

    static const char* banner(Outcome o);
    static std::string banner_text(Outcome o, const data::StringTable* strings);
};

}
