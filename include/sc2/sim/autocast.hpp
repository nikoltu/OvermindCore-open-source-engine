#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/ability_system.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::sim {

enum class AutocastMode {
    OffensiveNearestEnemy,
    SupportWoundedAlly,
};

class AutocastSystem {
public:
    void enable(std::uint32_t caster, int32_t ability, Fixed range, AutocastMode mode) {
        for (Entry& e : entries_)
            if (e.caster == caster && e.ability == ability) {
                e.range = range;
                e.mode = mode;
                return;
            }
        entries_.push_back(Entry{caster, ability, range, mode});
    }
    void disable(std::uint32_t caster, int32_t ability) {
        for (std::size_t i = 0; i < entries_.size(); ++i)
            if (entries_[i].caster == caster && entries_[i].ability == ability) {
                entries_.erase(entries_.begin() + i);
                return;
            }
    }
    bool enabled(std::uint32_t caster, int32_t ability) const {
        for (const Entry& e : entries_)
            if (e.caster == caster && e.ability == ability) return true;
        return false;
    }

    int advance(World& world, AbilitySystem& abil) {
        int cast = 0;
        const std::uint32_t n = std::uint32_t(world.units().size());
        for (const Entry& e : entries_) {
            if (e.caster >= n) continue;
            const Unit& c = world.units()[e.caster];
            if (!c.alive) continue;
            if (!abil.ready(world, e.caster, e.ability)) continue;

            const std::uint32_t target = pick_target(world, c, e);
            if (target == kNoTarget) continue;
            if (abil.try_cast(world, e.caster, e.ability, target)) ++cast;
        }
        return cast;
    }

private:
    struct Entry {
        std::uint32_t caster = kNoTarget;
        int32_t ability = -1;
        Fixed range;
        AutocastMode mode = AutocastMode::OffensiveNearestEnemy;
    };

    static std::uint32_t pick_target(const World& world, const Unit& c, const Entry& e) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        const Fixed r2 = e.range * e.range;
        std::uint32_t best = kNoTarget;
        Fixed best_d2;
        for (std::uint32_t i = 0; i < n; ++i) {
            const Unit& t = world.units()[i];
            if (!t.alive || t.loaded) continue;
            if (e.mode == AutocastMode::OffensiveNearestEnemy) {
                if (world.are_allied(t.player, c.player)) continue;
            } else {
                if (!world.are_allied(t.player, c.player)) continue;
                if (t.id == c.id) continue;
                if (t.hp >= t.hp_max) continue;
            }
            const Fixed dx = t.x - c.x, dy = t.y - c.y;
            const Fixed d2 = dx * dx + dy * dy;
            if (d2.raw() > r2.raw()) continue;
            if (best == kNoTarget || d2.raw() < best_d2.raw()) {
                best = i;
                best_d2 = d2;
            }
        }
        return best;
    }

    std::vector<Entry> entries_;
};

}
