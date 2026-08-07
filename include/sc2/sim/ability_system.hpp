#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>
#include <cstddef>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct VitalGate {
    enum Which { Life, Energy, Shields } vital = Life;
    enum Cmp { LT, LE, GT, GE, EQ, NE } cmp = LT;
    Fixed value;
    bool fraction = false;
};

inline bool vital_gate_passes(const VitalGate& g, const Unit& t) {
    Fixed cur, max;
    switch (g.vital) {
        case VitalGate::Life: cur = t.hp; max = t.hp_max; break;
        case VitalGate::Energy: cur = t.energy; max = t.energy_max; break;
        case VitalGate::Shields: cur = t.shields; max = t.shields_max; break;
    }
    const Fixed lhs = g.fraction ? (max.raw() > 0 ? cur / max : Fixed()) : cur;
    switch (g.cmp) {
        case VitalGate::LT: return lhs < g.value;
        case VitalGate::LE: return lhs <= g.value;
        case VitalGate::GT: return lhs > g.value;
        case VitalGate::GE: return lhs >= g.value;
        case VitalGate::EQ: return lhs == g.value;
        case VitalGate::NE: return !(lhs == g.value);
    }
    return true;
}

struct AbilitySpec {
    int32_t effect = -1;
    Fixed energy_cost;
    Fixed cooldown;
    std::uint32_t req_attr_mask = 0;
    std::uint32_t exc_attr_mask = 0;
    bool require_energy = false;
    std::vector<VitalGate> vital_gates;
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
        if (target < world.units().size() &&
            (s.req_attr_mask || s.exc_attr_mask || s.require_energy)) {
            const Unit& t = world.units()[target];
            if ((t.attributes & s.req_attr_mask) != s.req_attr_mask) return false;
            if (t.attributes & s.exc_attr_mask) return false;
            if (s.require_energy && t.energy.raw() <= 0) return false;
            for (const VitalGate& g : s.vital_gates)
                if (!vital_gate_passes(g, t)) return false;
        }
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
