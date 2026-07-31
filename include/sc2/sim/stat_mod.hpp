#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

enum class ModStat {
    Armor,
    WeaponDamage,
    WeaponRange,
    LifeMax,
};

class StatModSystem {
public:
    int apply(World& world, std::uint32_t target, ModStat stat, Fixed delta, Fixed duration) {
        if (target >= world.units().size()) return -1;
        Mod m;
        m.target = target;
        m.stat = stat;
        m.delta = delta;
        m.timer = duration.raw() > 0 ? duration : Fixed::from_raw(1);
        m.active = true;
        add_delta(world.unit(target), stat, delta);
        mods_.push_back(m);
        return int(mods_.size()) - 1;
    }

    int active_count() const {
        int n = 0;
        for (const Mod& m : mods_)
            if (m.active) ++n;
        return n;
    }

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        for (Mod& m : mods_) {
            if (!m.active) continue;
            m.timer = m.timer - dt;
            if (m.timer.raw() > 0) continue;
            m.active = false;
            if (m.target < n) add_delta(world.unit(m.target), m.stat, -m.delta);
        }
    }

private:
    struct Mod {
        std::uint32_t target = kNoTarget;
        ModStat stat = ModStat::Armor;
        Fixed delta;
        Fixed timer;
        bool active = false;
    };

    static void add_delta(Unit& u, ModStat stat, Fixed d) {
        switch (stat) {
            case ModStat::Armor: u.armor = u.armor + d; break;
            case ModStat::WeaponDamage: u.wpn_damage = u.wpn_damage + d; break;
            case ModStat::WeaponRange: u.wpn_range = u.wpn_range + d; break;
            case ModStat::LifeMax:
                u.hp_max = u.hp_max + d;
                if (u.hp > u.hp_max) u.hp = u.hp_max;
                break;
        }
    }

    std::vector<Mod> mods_;
};

}
