#pragma once

#include <cstdint>
#include <string>
#include <utility>
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
    int count = 1;
    bool morph_self = false;
    bool addon_self = false;
};

struct MeleeConfig {
    std::vector<BuildItem> build_order;
    BuildItem army_unit;
    BuildItem army_unit2;
    bool has_army_unit2 = false;
    std::string army_unit2_gate;
    std::vector<BuildItem> tech_chain;
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

    bool has_expansion = false;
    BuildItem expansion_unit;
    std::vector<std::pair<sim::Fixed, sim::Fixed>> expansion_sites;
    sim::Fixed expansion_occupied_radius = sim::Fixed::from_int(6);
    int expansion_worker_bonus = 10;
};

struct MeleeBase {
    uint32_t producer = sim::kNoTarget;
    uint32_t depot = sim::kNoTarget;
    uint32_t mineral = sim::kNoTarget;
    std::vector<uint32_t> workers;
    std::vector<uint32_t> structures;
    uint32_t expansion = sim::kNoTarget;
    uint32_t expansion_mineral = sim::kNoTarget;
};

class MeleeAI {
public:
    MeleeAI(int player, MeleeConfig cfg);

    void set_base(MeleeBase base);
    int player() const { return player_; }
    MeleeConfig& config() { return cfg_; }
    const MeleeConfig& config() const { return cfg_; }
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
    bool army_unit2_gate_ok(const sim::World& w) const;
    void run_expansion(sim::World& w);
    void run_army(sim::World& w);
    int patches_near(const sim::World& w, sim::Fixed x, sim::Fixed y, sim::Fixed radius) const;
    uint32_t nearest_mineral(const sim::World& w, sim::Fixed x, sim::Fixed y) const;
    bool enemy_target(const sim::World& w, sim::Fixed& tx, sim::Fixed& ty) const;

    int player_;
    MeleeConfig cfg_;
    MeleeBase base_;
    CombatController combat_;
    size_t build_idx_ = 0;
    bool attacking_ = false;
    bool producer_pending_ = false;
    bool army_alt_ = false;
    bool expansion_pending_ = false;
    sim::Fixed expand_x_, expand_y_;
};

}
