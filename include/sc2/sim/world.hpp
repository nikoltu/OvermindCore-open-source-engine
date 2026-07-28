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
};

struct Unit {
    uint32_t id = 0;
    int32_t player = 0;
    Fixed x, y;
    Fixed hp, hp_max, shields, armor;
    Fixed wpn_damage, wpn_period, wpn_range;
    Fixed speed, radius;
    Fixed speed_mult_creep = Fixed::from_int(1);
    Fixed cooldown;
    uint32_t target = kNoTarget;
    bool alive = true;
    std::vector<Order> orders;

    Fixed supply_provides;
    Fixed supply_uses;
    int32_t resource_left = 0;
    int32_t carrying = 0;
    int32_t carry_amount = 5;
    Fixed mine_time;
    Fixed mine_cd;
    uint32_t gather_target = kNoTarget;
    uint32_t home_depot = kNoTarget;

    Fixed buff_timer;
    Fixed buff_damage;

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

    bool queue_train(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                     int32_t gas_cost, Fixed build_time);
    size_t pending_count() const { return pending_.size(); }

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
    std::vector<VeterancyDef> vet_defs_;
    std::vector<AbilityDef> abilities_;
    CreepSystem creep_;
    NavGrid nav_;
    Rng rng_;
    int tick_ = 0;
    bool separation_ = false;

    uint32_t acquire_target(const Unit& u) const;
    Fixed move_step(const Unit& u, Fixed dt) const;
    bool move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step);
    void apply_separation();
    void step_gather(Unit& u, Fixed dt);
    void step_cast(Unit& u, Fixed dt);
    void step_production(Fixed dt);
    void award_kill_xp(Unit& killer, int64_t victim_xp);
    static Fixed dist2(const Unit& a, const Unit& b);
};

}
