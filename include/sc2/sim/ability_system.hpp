#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct AbilitySpec {
    int32_t effect = -1;
    Fixed energy_cost;
    Fixed cooldown;
    std::uint32_t req_attr_mask = 0;
    std::uint32_t exc_attr_mask = 0;
    bool require_energy = false;
    bool require_energy_pool = false;
    std::vector<VitalGate> vital_gates{};
    Fixed life_cost{};
};

class AbilitySystem {
public:
    int32_t add(AbilitySpec spec) {
        specs_.push_back(spec);
        return int32_t(specs_.size()) - 1;
    }
    std::size_t count() const { return specs_.size(); }

    Fixed cooldown_left(const World& world, std::uint32_t caster, int32_t ability) const {
        return world.ability_cooldown_left(caster, ability);
    }
    bool ready(const World& world, std::uint32_t caster, int32_t ability) const {
        if (ability < 0 || std::size_t(ability) >= specs_.size()) return false;
        return world.ability_cooldown_left(caster, specs_[ability].effect).raw() <= 0;
    }

    bool try_cast(World& world, std::uint32_t caster, int32_t ability, std::uint32_t target) {
        if (ability < 0 || std::size_t(ability) >= specs_.size()) return false;
        if (caster >= world.units().size()) return false;
        if (!ready(world, caster, ability)) return false;
        const AbilitySpec& s = specs_[ability];
        if (target < world.units().size()) {
            const Unit& t = world.units()[target];
            if ((t.attributes & s.req_attr_mask) != s.req_attr_mask) return false;
            if (t.attributes & s.exc_attr_mask) return false;
            if (s.require_energy && t.energy.raw() <= 0) return false;
            if (s.require_energy_pool && t.energy_max.raw() <= 0) return false;
            for (const VitalGate& g : s.vital_gates)
                if (!vital_gate_passes(g, t)) return false;
        }
        if (s.life_cost.raw() > 0 && !world.spend_life(caster, s.life_cost)) return false;
        if (s.energy_cost.raw() > 0 && !world.spend_energy(caster, s.energy_cost)) return false;
        world.cast(caster, s.effect, target);
        world.arm_ability_cooldown(caster, s.effect, s.cooldown);
        return true;
    }

    bool try_cast_point(World& world, std::uint32_t caster, int32_t ability, Fixed tx, Fixed ty) {
        if (ability < 0 || std::size_t(ability) >= specs_.size()) return false;
        if (caster >= world.units().size()) return false;
        if (!ready(world, caster, ability)) return false;
        const AbilitySpec& s = specs_[ability];
        if (s.energy_cost.raw() > 0 && !world.spend_energy(caster, s.energy_cost)) return false;
        world.cast(caster, s.effect, tx, ty);
        world.arm_ability_cooldown(caster, s.effect, s.cooldown);
        return true;
    }

private:
    std::vector<AbilitySpec> specs_;
};

}
