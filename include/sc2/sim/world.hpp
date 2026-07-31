#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/creep.hpp"
#include "sc2/sim/fixed.hpp"
#include "sc2/sim/nav.hpp"
#include "sc2/sim/rng.hpp"

namespace sc2::sim {

constexpr uint32_t kNoTarget = 0xFFFFFFFF;
constexpr uint32_t kNoAbility = 0xFFFFFFFF;

enum class OrderType {
    Idle,
    Move,
    AttackMove,
    Attack,
    HoldPosition,
    Gather,
    Cast,
    Patrol,
};

enum class EffectKind {
    Damage,
    Heal,
    Buff,
};

struct AbilityDef {
    EffectKind kind = EffectKind::Damage;
    Fixed amount;
    Fixed duration;
    Fixed range;
};

struct Order {
    OrderType type = OrderType::Idle;
    Fixed tx, ty;
    uint32_t target = kNoTarget;
    uint32_t ability = kNoAbility;
    Fixed px, py;
};

struct BehaviorInstance {
    uint16_t kind = 0;
    Fixed timer;
    Fixed speed_mult = Fixed::from_int(1);
    bool root = false;
    bool stun = false;
};

struct PersistentInstance {
    Fixed x, y;
    Fixed radius;
    Fixed period;
    Fixed timer;
    Fixed amount;
    int32_t count = 0;
    int32_t owner = -1;
    BehaviorInstance on_hit;
};

struct Unit {
    uint32_t id = 0;
    int32_t player = 0;
    int32_t type_id = 0;
    Fixed x, y;
    Fixed hp, hp_max, shields, armor;
    Fixed shields_max;
    Fixed wpn_damage, wpn_period, wpn_range;
    Fixed speed, radius;
    Fixed sight;
    Fixed speed_mult_creep = Fixed::from_int(1);
    Fixed atk_speed_mult = Fixed::from_int(1);
    Fixed damage_reduction;
    Fixed cooldown;
    uint32_t target = kNoTarget;
    bool alive = true;
    std::vector<Order> orders;

    Fixed supply_provides;
    Fixed supply_uses;
    int32_t resource_left = 0;
    int32_t carrying = 0;
    int32_t carry_amount = 5;
    bool is_gas = false;
    bool gathers_gas = false;
    bool is_worker = false;
    Fixed mine_time;
    Fixed mine_cd;
    uint32_t gather_target = kNoTarget;
    uint32_t home_depot = kNoTarget;

    bool has_rally = false;
    Fixed rally_x, rally_y;

    Fixed life_regen;
    Fixed time_life;

    Fixed buff_timer;
    Fixed buff_damage;

    bool constructing = false;
    Fixed build_time;
    Fixed build_progress;
    Fixed build_hp0;

    Fixed energy, energy_max, energy_regen;

    std::vector<BehaviorInstance> behaviors;

    bool cloaked = false;
    bool detector = false;

    bool loaded = false;
    bool burrowed = false;

    bool flying = false;
    uint32_t attributes = 0;
    uint32_t wpn_target_filter = 3;

    int32_t vet_index = -1;
    int64_t xp = 0;
    int32_t level = 0;
    int64_t kill_xp = 0;
    bool heroic = false;
    bool hero_ui = false;

    std::vector<Waypoint> path;
    size_t path_next = 0;
    int32_t path_gx = INT32_MIN;
    int32_t path_gy = INT32_MIN;
};

struct Player {
    int32_t minerals = 50;
    int32_t gas = 0;
    int32_t team = -1;
    std::vector<int32_t> upgrades;
};

struct VeterancyDef {
    std::vector<int64_t> xp_threshold;
    std::vector<Fixed> cum_life;
    std::vector<Fixed> cum_damage;
    Fixed xp_fraction = Fixed::from_int(1);
    int max_level() const { return int(xp_threshold.size()) - 1; }
};

class World {
public:
    static constexpr int kTicksPerSecond = 16;
    static Fixed dt() { return Fixed::from_ratio(1, kTicksPerSecond); }

    explicit World(uint64_t seed = 1) : rng_(seed) {}

    uint32_t add_unit(Unit u) {
        u.id = uint32_t(units_.size());
        units_.push_back(u);
        return u.id;
    }

    void issue(uint32_t id, const Order& o, bool queued = false) {
        if (id >= units_.size()) return;
        Unit& u = units_[id];
        if (!queued) u.orders.clear();
        u.orders.push_back(o);
    }
    void stop(uint32_t id) {
        if (id < units_.size()) units_[id].orders.clear();
    }

    void set_rally(uint32_t id, Fixed x, Fixed y) {
        if (id >= units_.size()) return;
        units_[id].has_rally = true;
        units_[id].rally_x = x;
        units_[id].rally_y = y;
    }
    void clear_rally(uint32_t id) {
        if (id < units_.size()) units_[id].has_rally = false;
    }

    Order smart_order(uint32_t actor, uint32_t target, Fixed tx, Fixed ty) const;

    void issue_smart(uint32_t actor, uint32_t target, Fixed tx, Fixed ty,
                     bool queued = false) {
        issue(actor, smart_order(actor, target, tx, ty), queued);
    }

    Unit& unit(uint32_t id) { return units_[id]; }

    void step();
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) step();
    }

    Player& player(int32_t p);
    const Player& player(int32_t p) const;

    int32_t add_ability(AbilityDef def);
    size_t ability_count() const { return abilities_.size(); }

    void cast(uint32_t caster, int32_t ability, uint32_t target) {
        Order o;
        o.type = OrderType::Cast;
        o.ability = uint32_t(ability);
        o.target = target;
        issue(caster, o, false);
    }

    int32_t add_veterancy(VeterancyDef def);
    Fixed supply_used(int32_t p) const;
    Fixed supply_max(int32_t p) const;

    static bool is_neutral(int32_t p) { return p < 0; }
    int32_t team_of(int32_t p) const;
    bool are_allied(int32_t a, int32_t b) const;

    int32_t upgrade_level(int32_t player, int32_t upgrade_id) const;
    void set_upgrade_level(int32_t player, int32_t upgrade_id, int32_t level);

    bool queue_train(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                     int32_t gas_cost, Fixed build_time);
    size_t pending_count() const { return pending_.size(); }

    int boost_production(uint32_t producer, Fixed extra) {
        int affected = 0;
        for (Pending& p : pending_) {
            if (p.producer != producer || p.timer.raw() <= 0) continue;
            p.timer = p.timer - extra;
            if (p.timer.raw() < 0) p.timer = Fixed();
            ++affected;
        }
        return affected;
    }

    void start_construction(uint32_t id, Fixed build_time, Fixed start_hp) {
        if (id >= units_.size() || build_time.raw() <= 0) return;
        Unit& u = units_[id];
        u.constructing = true;
        u.build_time = build_time;
        u.build_progress = Fixed();
        u.build_hp0 = start_hp;
        u.hp = start_hp;
    }
    bool is_constructing(uint32_t id) const {
        return id < units_.size() && units_[id].constructing;
    }
    Fixed build_progress(uint32_t id) const {
        return id < units_.size() ? units_[id].build_progress : Fixed();
    }

    void add_behavior(uint32_t id, const BehaviorInstance& b);

    bool spend_energy(uint32_t id, Fixed amount) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (u.energy < amount) return false;
        u.energy = u.energy - amount;
        return true;
    }
    Fixed energy(uint32_t id) const {
        return id < units_.size() ? units_[id].energy : Fixed();
    }

    int32_t add_persistent(PersistentInstance p);
    size_t persistent_count() const { return persistents_.size(); }

    uint64_t state_hash() const;

    const std::vector<Unit>& units() const { return units_; }
    int tick() const { return tick_; }
    int alive_count(int player) const;

    CreepSystem& creep() { return creep_; }
    const CreepSystem& creep() const { return creep_; }

    NavGrid& nav() { return nav_; }
    const NavGrid& nav() const { return nav_; }

    void set_separation(bool on) { separation_ = on; }
    bool separation() const { return separation_; }

    void set_fog_acquire(bool on) { fog_acquire_ = on; }
    bool fog_acquire() const { return fog_acquire_; }
    bool visible_to(int32_t player, const Unit& target) const;

    int winner() const;

private:
    struct Pending {
        uint32_t producer;
        Unit proto;
        Fixed timer;
    };

    std::vector<Unit> units_;
    std::vector<Player> players_;
    std::vector<Pending> pending_;
    std::vector<PersistentInstance> persistents_;
    std::vector<VeterancyDef> vet_defs_;
    std::vector<AbilityDef> abilities_;
    CreepSystem creep_;
    NavGrid nav_;
    Rng rng_;
    int tick_ = 0;
    bool separation_ = false;
    bool fog_acquire_ = false;

    uint32_t acquire_target(const Unit& u) const;
    Fixed move_step(const Unit& u, Fixed dt) const;
    bool move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step);
    void apply_separation();
    void step_gather(Unit& u, Fixed dt);
    void step_cast(Unit& u, Fixed dt);
    void step_production(Fixed dt);
    void step_construction(Unit& u, Fixed dt);
    void step_persistents(Fixed dt);
    static Fixed behavior_speed_mult(const Unit& u);
    static bool behavior_stunned(const Unit& u);
    static bool behavior_rooted(const Unit& u);
    void award_kill_xp(Unit& killer, int64_t victim_xp);
    static Fixed dist2(const Unit& a, const Unit& b);
};

}
