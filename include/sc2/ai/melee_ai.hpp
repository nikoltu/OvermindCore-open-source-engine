#pragma once

#include <cstdint>
#include <vector>

#include "sc2/ai/combat.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::ai {

struct BuildItem {
    sim::Unit proto;
    int minerals = 0;
    int gas = 0;
    sim::Fixed build_time = sim::Fixed::from_int(1);
    bool army = false;
};

struct MeleeConfig {
    std::vector<BuildItem> build_order;
    BuildItem army_unit;
    BuildItem supply_unit;
    BuildItem worker_unit;
    int worker_target = 0;
    BuildItem producer_unit;
    bool has_producer = false;
    bool has_supply = false;
    int army_threshold = 6;
    int max_pending = 2;
    bool smart_combat = false;
    sim::Fixed defense_radius = sim::Fixed::from_int(14);
    sim::Fixed supply_margin = sim::Fixed::from_int(2);
    sim::Fixed rally_x, rally_y;
    sim::Fixed enemy_x, enemy_y;
};

struct MeleeBase {
    uint32_t producer = sim::kNoTarget;
    uint32_t depot = sim::kNoTarget;
    uint32_t mineral = sim::kNoTarget;
    std::vector<uint32_t> workers;
    std::vector<uint32_t> structures;
};

class MeleeAI {
public:
    MeleeAI(int player, MeleeConfig cfg);

    void set_base(MeleeBase base);
    int player() const { return player_; }
    bool attacking() const { return attacking_; }
    int army_size(const sim::World& w) const;

    void update(sim::World& w);

private:
    bool is_worker(uint32_t id) const;
    int worker_count(const sim::World& w) const;
    bool is_structure(uint32_t id) const;
    bool is_army(const sim::Unit& u) const;
    bool afford(const sim::World& w, const BuildItem& it) const;
    bool supply_ok(const sim::World& w, const BuildItem& it) const;
    void run_workers(sim::World& w);
    void run_production(sim::World& w);
    void run_army(sim::World& w);
    bool enemy_target(const sim::World& w, sim::Fixed& tx, sim::Fixed& ty) const;

    int player_;
    MeleeConfig cfg_;
    MeleeBase base_;
    CombatController combat_;
    size_t build_idx_ = 0;
    bool attacking_ = false;
    bool producer_pending_ = false;
};

}
