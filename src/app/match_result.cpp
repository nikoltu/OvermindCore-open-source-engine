#include "sc2/app/match_result.hpp"

namespace sc2::app {

MatchResult::Outcome MatchResult::evaluate(const sim::World& w, int human_player, int stalemate_ticks) {
    if (stalemate_ticks > 0 && w.tick() - w.last_progress_tick() >= stalemate_ticks) return Outcome::Draw;
    const int sole = w.winner();
    if (sole >= 0 && sole == human_player) return Outcome::Victory;

    if (w.defeat_holders(human_player) == 0) {
        if (sole >= 0) return Outcome::Defeat;
        for (const sim::Unit& u : w.units())
            if (u.alive && u.player >= 0 && u.player != human_player && (!w.defeat_rule_authored() || u.prevent_defeat))
                return Outcome::Defeat;
    }
    return Outcome::Ongoing;
}

const char* MatchResult::banner(Outcome o) {
    switch (o) {
        case Outcome::Victory: return "Victory!";
        case Outcome::Defeat:  return "Defeat";
        case Outcome::Draw:    return "Stalemate";
        case Outcome::Ongoing: return "";
    }
    return "";
}

}
