#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct AbilitySpec {
    int32_t effect = -1;
    Fixed energy_cost;
    Fixed cooldown;
};

class AbilitySystem {
public:
    int32_t add(AbilitySpec spec) {
        specs_.push_back(spec);
        return int32_t(specs_.size()) - 1;
    }
    std::size_t count() const { return specs_.size(); }

    Fixed cooldown_left(std::uint32_t caster, int32_t ability) const {
        auto it = cd_.find(key(caster, ability));
        return it == cd_.end() ? Fixed() : it->second;
    }
    bool ready(std::uint32_t caster, int32_t ability) const {
        return cooldown_left(caster, ability).raw() <= 0;
    }

    bool try_cast(World& world, std::uint32_t caster, int32_t ability, std::uint32_t target) {
        if (ability < 0 || std::size_t(ability) >= specs_.size()) return false;
        if (caster >= world.units().size()) return false;
        if (!ready(caster, ability)) return false;
        const AbilitySpec& s = specs_[ability];
        if (s.energy_cost.raw() > 0 && !world.spend_energy(caster, s.energy_cost)) return false;
        if (s.cooldown.raw() > 0) cd_[key(caster, ability)] = s.cooldown;
        world.cast(caster, s.effect, target);
        return true;
    }

    void advance(Fixed dt) {
        for (auto it = cd_.begin(); it != cd_.end();) {
            it->second = it->second - dt;
            if (it->second.raw() <= 0)
                it = cd_.erase(it);
            else
                ++it;
        }
    }

private:
    static std::uint64_t key(std::uint32_t caster, int32_t ability) {
        return (std::uint64_t(caster) << 32) | std::uint32_t(ability);
    }

    std::vector<AbilitySpec> specs_;
    std::unordered_map<std::uint64_t, Fixed> cd_;
};

}
