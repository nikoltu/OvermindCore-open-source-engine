#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

enum class UpgradeStat {
    WeaponDamage,
    LifeMax,
    Armor,
    Shields,
    WeaponRange,
};

struct UpgradeDef {
    UpgradeStat stat = UpgradeStat::WeaponDamage;
    Fixed per_level;
    int32_t target_type = -1;
    int32_t max_level = 3;
};

class UpgradeSystem {
public:
    int32_t add(UpgradeDef def) {
        defs_.push_back(def);
        return int32_t(defs_.size()) - 1;
    }
    std::size_t count() const { return defs_.size(); }

    void research(World& world, int32_t player, int32_t upgrade, int32_t to_level) {
        if (upgrade < 0 || std::size_t(upgrade) >= defs_.size()) return;
        const UpgradeDef& d = defs_[upgrade];
        int32_t cur = world.upgrade_level(player, upgrade);
        if (to_level > d.max_level) to_level = d.max_level;
        if (to_level <= cur) return;
        const int32_t gained = to_level - cur;
        world.set_upgrade_level(player, upgrade, to_level);
        const std::uint32_t n = std::uint32_t(world.units().size());
        for (std::uint32_t id = 0; id < n; ++id) {
            Unit& u = world.unit(id);
            if (!u.alive || u.player != player) continue;
            if (d.target_type >= 0 && u.type_id != d.target_type) continue;
            apply_delta(u, d, gained);
        }
    }

    void apply_to_unit(const World& world, Unit& u) const {
        for (std::size_t i = 0; i < defs_.size(); ++i) {
            const UpgradeDef& d = defs_[i];
            if (d.target_type >= 0 && u.type_id != d.target_type) continue;
            int32_t lvl = world.upgrade_level(u.player, int32_t(i));
            if (lvl > 0) apply_delta(u, d, lvl);
        }
    }

private:
    static void apply_delta(Unit& u, const UpgradeDef& d, int32_t levels) {
        const Fixed amt = d.per_level * Fixed::from_int(levels);
        switch (d.stat) {
            case UpgradeStat::WeaponDamage: u.wpn_damage = u.wpn_damage + amt; break;
            case UpgradeStat::LifeMax:
                u.hp_max = u.hp_max + amt;
                u.hp = u.hp + amt;
                break;
            case UpgradeStat::Armor: u.armor = u.armor + amt; break;
            case UpgradeStat::Shields: u.shields = u.shields + amt; break;
            case UpgradeStat::WeaponRange: u.wpn_range = u.wpn_range + amt; break;
        }
    }

    std::vector<UpgradeDef> defs_;
};

}
