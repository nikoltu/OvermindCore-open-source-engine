#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <array>
#include <vector>

#include "sc2/ai/combat.hpp"
#include "sc2/sim/vision.hpp"
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

enum class StockKind : std::uint8_t { Worker, Supply, Producer, Army, Structure, Skip };
struct StockStep {
    std::string id;
    int count = 0;
    BuildItem item;
    StockKind kind = StockKind::Skip;
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
    int worker_train_delay_ticks = 0;
    bool flee_damage = true;
    BuildItem producer_unit;
    bool has_producer = false;
    bool has_supply = false;
    int army_threshold = 6;
    struct WavePhase {
        int desired_army = 0;
        int attack_duration_s = 0;
    };
    std::vector<WavePhase> wave_phases;
    std::array<std::vector<WavePhase>, 5> wave_ladders;
    sim::Fixed attack_caution = sim::Fixed::from_ratio(9, 10);
    int max_pending = 2;
    bool worker_builds = false;
    int gas_peons_per_geyser = 3;
    bool smart_combat = false;
    sim::Fixed defense_radius = sim::Fixed::from_int(14);
    sim::Fixed supply_margin = sim::Fixed::from_int(2);
    sim::Fixed rally_x, rally_y;
    sim::Fixed enemy_x, enemy_y;

    int num_scouts = 0;
    std::vector<StockStep> stock;
    std::vector<StockStep> hd_stock;
    std::string hd_build_id;
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
    void set_map_bounds(const sim::Bounds& b) { bounds_ = b; has_bounds_ = true; }
    const sim::VisionGrid& vision() const { return vision_; }
    uint32_t scout() const { return scout_; }
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
    void run_stock(sim::World& w);
    bool stock_active() const { return stock_idx_ < cfg_.stock.size(); }
    int owned_or_pending(const sim::World& w, const std::string& id) const;
    bool make_item(sim::World& w, uint32_t from, const BuildItem& item);
    bool place_structure(sim::World& w, const BuildItem& item);
    void prune_site_builds(const sim::World& w);
    void run_gas(sim::World& w);
    void run_scout(sim::World& w);
    void run_army(sim::World& w);
    int patches_near(const sim::World& w, sim::Fixed x, sim::Fixed y, sim::Fixed radius) const;
    uint32_t nearest_mineral(const sim::World& w, sim::Fixed x, sim::Fixed y) const;
    bool enemy_target(const sim::World& w, sim::Fixed& tx, sim::Fixed& ty) const;

    int player_;
    MeleeConfig cfg_;
    MeleeBase base_;
    CombatController combat_;
    size_t build_idx_ = 0;
    size_t stock_idx_ = 0;
    int last_worker_train_tick_ = -1000000;
    bool attacking_ = false;
    std::size_t wave_phase_ = 0;
    int wave_phase_start_tick_ = -1;
    int launched_desired_ = 0;
    bool producer_pending_ = false;
    struct SiteBuild { std::string id; uint32_t builder = sim::kNoTarget; };
    std::vector<SiteBuild> site_builds_;
    std::string producer_id_;
    bool army_alt_ = false;
    bool expansion_pending_ = false;
    sim::Fixed expand_x_, expand_y_;
    uint32_t scout_ = sim::kNoTarget;
    bool scout_done_ = false;
    bool scout_at_start_ = false;
    int scout_leg_deadline_ = 0;
    sim::Fixed scout_best_d2_ = sim::Fixed::from_int(-1);
    int scout_no_progress_ = 0;
    sim::Bounds bounds_{};
    bool has_bounds_ = false;
    sim::VisionGrid vision_;
};

}
