#include "omc/app/match_result.hpp"

#include <cstdlib>

namespace omc::app {

bool MatchResult::sole_player_rule() {
    return std::getenv("OMC_MELEE_SOLE_PLAYER_WIN") != nullptr;
}

bool MatchResult::viewer_defeated(const sim::World& w, int human_player) {
    return w.player(human_player).defeated || w.defeat_holders(human_player) == 0;
}

MatchResult::Outcome MatchResult::evaluate(const sim::World& w, int human_player, int stalemate_ticks) {
    if (stalemate_ticks > 0 && w.tick() - w.last_progress_tick() >= stalemate_ticks) return Outcome::Draw;
    if (!sole_player_rule()) {
        const int32_t viewer_key = w.alliance_key(human_player);
        bool viewer_stands = false, other_stands = false;
        for (const sim::Unit& u : w.units()) {
            if (!u.alive || u.player < 0) continue;
            if (w.defeat_rule_authored() && !u.prevent_defeat) continue;
            if (w.player(u.player).defeated) continue;
            if (w.alliance_key(u.player) == viewer_key) viewer_stands = true;
            else other_stands = true;
            if (viewer_stands && other_stands) return Outcome::Ongoing;
        }
        if (viewer_stands) return Outcome::Victory;
        return other_stands ? Outcome::Defeat : Outcome::Ongoing;
    }
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

std::string MatchResult::banner_text(Outcome o, const data::StringTable* strings) {
    static const bool kOld = std::getenv("OMC_RESULT_TEXT_OLD") != nullptr;
    const char* key = o == Outcome::Victory ? "UI/Text/Victory" : o == Outcome::Defeat ? "UI/Text/Defeat"
                    : o == Outcome::Draw    ? "UI/Text/Tie" : nullptr;
    if (!kOld && key && strings)
        if (const std::string* v = strings->find(key); v && !v->empty()) return *v;
    return banner(o);
}

}
