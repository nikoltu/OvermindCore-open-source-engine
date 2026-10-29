#pragma once
#include <cstdint>

#include "omc/ai/melee_ai.hpp"

namespace omc::ai {

enum class Difficulty : std::uint8_t { Unset = 0, VeryEasy = 1, Easy = 2, Medium = 3, Hard = 4, Harder = 5, VeryHard = 6, Elite = 7 };

struct DifficultyRung {
    Difficulty tier;
    const char* label;
    int apm;
    int worker_delay_s;
    int defense_delay_s;
    bool flee_damage;
    bool press_forward;
    bool limit_apm;
};

inline const DifficultyRung* difficulty_table(int& count) {
    static const DifficultyRung kRungs[] = {
        {Difficulty::VeryEasy, "Very Easy", 400, 10, 40, false, false, true},
        {Difficulty::Easy, "Easy", 600, 6, 20, false, false, true},
        {Difficulty::Medium, "Medium", 800, 4, 10, false, true, true},
        {Difficulty::Hard, "Hard", 1100, 2, 5, false, true, true},
        {Difficulty::Harder, "Harder", 1500, 0, 0, true, true, true},
        {Difficulty::VeryHard, "Very Hard", 2000, 0, 0, true, true, false},
        {Difficulty::Elite, "Elite", 2700, 0, 0, true, true, false},
    };
    count = int(sizeof(kRungs) / sizeof(kRungs[0]));
    return kRungs;
}

inline const DifficultyRung* difficulty_rung(Difficulty d) {
    int n = 0;
    const DifficultyRung* t = difficulty_table(n);
    for (int i = 0; i < n; ++i)
        if (t[i].tier == d) return &t[i];
    return nullptr;
}

inline int difficulty_period_ticks(int apm) {
    if (apm <= 0) return 8;
    int p = (8 * 2000 + apm / 2) / apm;
    if (p < 4) p = 4;
    if (p > 48) p = 48;
    return p;
}

inline void apply_difficulty(MeleeConfig& cfg, Difficulty d, int& period_ticks) {
    const DifficultyRung* r = difficulty_rung(d);
    if (!r) return;
    cfg.worker_train_delay_ticks = r->worker_delay_s * sim::World::kTicksPerSecond;
    cfg.flee_damage = r->flee_damage;
    period_ticks = difficulty_period_ticks(r->apm);
    const int tier = d <= Difficulty::Harder ? int(d) - 1 : 4;
    if (tier >= 0 && tier < 5 && !cfg.wave_ladders[std::size_t(tier)].empty())
        cfg.wave_phases = cfg.wave_ladders[std::size_t(tier)];
    if (d >= Difficulty::VeryHard && !cfg.hd_stock.empty()) cfg.stock = cfg.hd_stock;
}

}
