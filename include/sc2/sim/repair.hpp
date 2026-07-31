#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class RepairSystem {
public:
    explicit RepairSystem(Fixed hp_per_sec = Fixed::from_int(9),
                          Fixed mineral_per_hp = Fixed::from_ratio(1, 3),
                          Fixed range = Fixed::from_int(1))
        : rate_(hp_per_sec), cost_per_hp_(mineral_per_hp), range_(range) {}

    void assign(std::uint32_t repairer, std::uint32_t target) {
        for (const Job& j : jobs_)
            if (j.repairer == repairer && j.target == target) return;
        jobs_.push_back(Job{repairer, target, Fixed()});
    }
    int active_count() const { return int(jobs_.size()); }

    void advance(World& world, Fixed dt) {
        const std::uint32_t n = std::uint32_t(world.units().size());
        std::vector<Job> kept;
        kept.reserve(jobs_.size());
        for (Job& j : jobs_) {
            if (j.repairer >= n || j.target >= n) continue;
            const Unit& r = world.units()[j.repairer];
            Unit& t = world.unit(j.target);
            if (!r.alive || !t.alive) continue;
            if (!world.are_allied(r.player, t.player)) continue;
            if (t.hp >= t.hp_max) continue;

            const Fixed dx = r.x - t.x, dy = r.y - t.y;
            if (dx * dx + dy * dy > range_ * range_) {
                kept.push_back(j);
                continue;
            }

            Fixed heal = rate_ * dt;
            const Fixed missing = t.hp_max - t.hp;
            if (heal > missing) heal = missing;

            Player& p = world.player(t.player);
            if (cost_per_hp_.raw() > 0) {
                const Fixed budget = Fixed::from_int(p.minerals) - j.cost_accum;
                if (budget.raw() <= 0) {
                    kept.push_back(j);
                    continue;
                }
                const Fixed affordable_hp = budget / cost_per_hp_;
                if (heal > affordable_hp) heal = affordable_hp;
                if (heal.raw() <= 0) {
                    kept.push_back(j);
                    continue;
                }
            }

            j.cost_accum = j.cost_accum + heal * cost_per_hp_;
            const int due = j.cost_accum.floor();
            if (due > 0) {
                p.minerals -= due;
                j.cost_accum = j.cost_accum - Fixed::from_int(due);
            }

            t.hp = t.hp + heal;
            if (t.hp > t.hp_max) t.hp = t.hp_max;
            if (t.hp < t.hp_max) kept.push_back(j);
        }
        jobs_.swap(kept);
    }

private:
    struct Job {
        std::uint32_t repairer = kNoTarget;
        std::uint32_t target = kNoTarget;
        Fixed cost_accum;
    };
    Fixed rate_, cost_per_hp_, range_;
    std::vector<Job> jobs_;
};

}
