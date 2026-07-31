#pragma once

#include <cstdint>

#include "sc2/ai/economy.hpp"
#include "sc2/sim/fixed.hpp"

namespace sc2::ai {

enum class MacroAction {
    Idle,
    BuildSupply,
    BuildWorker,
    Expand,
    BuildArmy,
};

struct MacroState {
    int workers = 0;
    int mineral_patches = 0;
    int bank_minerals = 0;
    sim::Fixed supply_headroom;
    bool supply_maxed = false;

    int worker_cost = 50;
    int supply_cost = 100;
    int base_cost = 400;
    int ideal_per_patch = 2;
    sim::Fixed supply_margin = sim::Fixed::from_int(2);
    int worker_target_slack = 0;
};

inline MacroAction macro_decide(const MacroState& s) {
    if (!s.supply_maxed && s.supply_headroom <= s.supply_margin && s.bank_minerals >= s.supply_cost)
        return MacroAction::BuildSupply;

    const int ideal = s.mineral_patches * s.ideal_per_patch;
    if (s.workers < ideal - s.worker_target_slack && s.bank_minerals >= s.worker_cost)
        return MacroAction::BuildWorker;

    if (should_expand(s.workers, s.mineral_patches, s.bank_minerals, s.base_cost,
                      s.ideal_per_patch))
        return MacroAction::Expand;

    if (s.bank_minerals >= s.worker_cost) return MacroAction::BuildArmy;

    return MacroAction::Idle;
}

}
