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
    ModifyEnergy,
    ApplyBehavior,
    Teleport,
    CreatePersistent,
    CreateUnit,
    Morph,
};

inline constexpr std::uint16_t kCastBehaviorKind = 0xCA5;

struct AbilityDef {
    EffectKind kind = EffectKind::Damage;
    Fixed amount;
    Fixed duration;
    Fixed range;
    Fixed energy_fraction;
    Fixed move_speed_mult = Fixed::from_int(1);
    Fixed attack_speed_mult = Fixed::from_int(1);
    bool beh_root = false;
    bool beh_stun = false;
    Fixed beh_dmg_per_tick;
    Fixed beh_dmg_period;
    Fixed radius;
    Fixed period;
    int32_t period_count = 0;
    int32_t spawn_proto = -1;
    int32_t spawn_count = 0;
    int32_t mineral_cost = 0, gas_cost = 0;
    Fixed morph_heal_life_frac, morph_heal_shield_frac;
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
    Fixed dmg_per_tick;
    Fixed dmg_period;
    Fixed dmg_timer;
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

enum class StatKind { Armor, WeaponDamage, WeaponRange, LifeMax };

struct StatModInstance {
    uint32_t target = kNoTarget;
    StatKind stat = StatKind::Armor;
    Fixed delta;
    Fixed timer;
};

struct KnockImpulse {
    uint32_t target = kNoTarget;
    Fixed dx, dy;
    Fixed speed;
    Fixed remaining;
};

struct ForceFieldInstance {
    Fixed x, y, radius, timer;
    std::vector<std::pair<int, int>> cells;
};

struct Unit {
    uint32_t id = 0;
    int32_t player = 0;
    int32_t type_id = 0;
    Fixed x, y;
    Fixed hp, hp_max, shields, armor;
    Fixed shields_max;
    Fixed shield_regen_rate, shield_regen_delay;
    Fixed shield_since_hit, shield_last_total;
    Fixed shield_armor;
    Fixed wpn_damage, wpn_period, wpn_range;
    Fixed wpn_damage_point;
    Fixed speed, radius;
    Fixed sight;
    Fixed speed_mult_creep = Fixed::from_int(1);
    Fixed atk_speed_mult = Fixed::from_int(1);
    Fixed damage_reduction;
    Fixed aura_dmg_reduction, aura_radius;
    Fixed aura_reduction;
    Fixed cooldown;
    uint32_t target = kNoTarget;
    bool alive = true;
    std::vector<Order> orders;

    Fixed supply_provides;
    Fixed supply_uses;
    int32_t resource_left = 0;
    int32_t miners_max = 0;
    int32_t yield_amount = 0;
    int32_t carrying = 0;
    int32_t carry_amount = 5;
    bool is_gas = false;
    bool gathers_gas = false;
    bool is_worker = false;
    Fixed mine_time;
    Fixed mine_cd;
    Fixed return_delay;
    Fixed return_cd;
    uint32_t gather_target = kNoTarget;
    uint32_t home_depot = kNoTarget;

    int32_t train_slots = 0;

    bool has_rally = false;
    Fixed rally_x, rally_y;

    bool hold_fire = false;

    Fixed chrono_timer;

    Fixed life_regen;
    Fixed time_life;

    Fixed buff_timer;
    Fixed buff_damage;

    Fixed morph_timer;
    int32_t morph_proto = -1;

    int32_t spawner_proto = -1;
    int32_t spawner_cap = 0;
    int32_t spawner_hard_cap = 0;
    Fixed spawner_period;
    Fixed spawner_timer;
    uint32_t spawn_parent = kNoTarget;

    Fixed inject_timer;
    int32_t inject_proto = -1;
    int32_t inject_count = 0;
    bool larva_injected = false;

    Fixed warp_cd;

    int32_t cargo_space = 0;
    int32_t cargo_size = 0;
    bool cargo_death = false;
    uint32_t transport_id = kNoTarget;
    bool attack_relay = false;
    Fixed relay_range_bonus;

    Fixed power_radius;
    bool needs_power = false;
    bool powered = false;
    Fixed morph_heal_life_frac, morph_heal_shield_frac;

    bool constructing = false;
    Fixed build_time;
    Fixed build_progress;
    Fixed build_hp0;
    bool needs_builder = false;
    uint32_t builder = kNoTarget;
    uint32_t construct_target = kNoTarget;

    Fixed energy, energy_max, energy_regen;

    std::vector<BehaviorInstance> behaviors;

    bool cloaked = false;
    bool detector = false;
    Fixed detect_range;
    Fixed cloak_drain;
    Fixed cloak_field_radius;
    bool field_cloaked = false;
    bool cloaked_effective() const { return cloaked || field_cloaked; }

    bool loaded = false;
    bool burrowed = false;

    bool flying = false;
    uint32_t attributes = 0;
    uint32_t wpn_target_filter = 3;

    Fixed wpn2_damage, wpn2_period, wpn2_range;
    uint32_t wpn2_target_filter = 3;

    Fixed splash_inner, splash_mid, splash_outer;

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
    int32_t weapon_upgrade = 0;
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

    void set_game_speed(Fixed mult) { game_speed_ = mult; }
    Fixed game_speed() const { return game_speed_; }

    explicit World(uint64_t seed = 1) : rng_(seed) {}

    uint32_t add_unit(Unit u) {
        u.id = uint32_t(units_.size());
        units_.push_back(u);
        return u.id;
    }

    void issue(uint32_t id, const Order& o, bool queued = false) {
        if (id >= units_.size()) return;
        detach_builder(id);
        Unit& u = units_[id];
        if (!queued) u.orders.clear();
        u.orders.push_back(o);
    }
    void stop(uint32_t id) {
        if (id >= units_.size()) return;
        detach_builder(id);
        units_[id].orders.clear();
    }

    void detach_builder(uint32_t id) {
        if (id >= units_.size()) return;
        const uint32_t bld = units_[id].construct_target;
        if (bld == kNoTarget) return;
        units_[id].construct_target = kNoTarget;
        if (bld < units_.size() && units_[bld].builder == id) units_[bld].builder = kNoTarget;
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
    bool queue_train_at(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                        int32_t gas_cost, Fixed build_time, Fixed x, Fixed y);
    size_t pending_count() const { return pending_.size(); }

    bool issue_worker_build(uint32_t builder, Fixed x, Fixed y, const Unit& proto,
                            int32_t mineral_cost, int32_t gas_cost, Fixed build_time,
                            Fixed arrive = Fixed::from_int(2));
    size_t worker_build_count() const { return worker_builds_.size(); }

    bool worker_committed(uint32_t id) const {
        if (id < units_.size() && units_[id].construct_target != kNoTarget) return true;
        for (const WorkerBuild& wb : worker_builds_)
            if (wb.builder == id) return true;
        return false;
    }

    bool assign_builder(uint32_t worker, uint32_t building);
    bool is_halted(uint32_t id) const {
        return id < units_.size() && units_[id].constructing && units_[id].needs_builder &&
               units_[id].builder == kNoTarget;
    }

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

    bool cast_chrono(uint32_t producer, uint32_t caster, Fixed energy_cost, Fixed duration) {
        if (producer >= units_.size() || caster >= units_.size()) return false;
        Unit& c = units_[caster];
        if (c.energy < energy_cost) return false;
        c.energy = c.energy - energy_cost;
        units_[producer].chrono_timer = duration;
        return true;
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

    bool set_cloak(uint32_t id, bool engage) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (engage && u.energy.raw() <= 0) return false;
        u.cloaked = engage;
        return u.cloaked;
    }

    int32_t add_persistent(PersistentInstance p);
    size_t persistent_count() const { return persistents_.size(); }

    int32_t add_force_field(Fixed x, Fixed y, Fixed radius, Fixed duration) {
        ForceFieldInstance ff;
        ff.x = x;
        ff.y = y;
        ff.radius = radius;
        ff.timer = duration.raw() > 0 ? duration : Fixed::from_raw(1);
        if (nav_.configured() && radius.raw() > 0) {
            int cx0, cy0, cx1, cy1;
            nav_.to_cell(x - radius, y - radius, cx0, cy0);
            nav_.to_cell(x + radius, y + radius, cx1, cy1);
            const Fixed r2 = radius * radius;
            for (int cy = cy0; cy <= cy1; ++cy)
                for (int cx = cx0; cx <= cx1; ++cx) {
                    Fixed wx, wy;
                    nav_.cell_center(cx, cy, wx, wy);
                    const Fixed dx = wx - x, dy = wy - y;
                    if (dx * dx + dy * dy <= r2 && nav_.in_bounds(cx, cy) &&
                        !nav_.is_blocked_cell(cx, cy)) {
                        nav_.block_cell(cx, cy);
                        ff.cells.emplace_back(cx, cy);
                    }
                }
        }
        force_fields_.push_back(std::move(ff));
        return int32_t(force_fields_.size()) - 1;
    }
    size_t force_field_count() const { return force_fields_.size(); }

    void apply_stat_mod(uint32_t target, StatKind stat, Fixed delta, Fixed duration) {
        if (target >= units_.size()) return;
        apply_stat_delta(units_[target], stat, delta);
        StatModInstance m;
        m.target = target;
        m.stat = stat;
        m.delta = delta;
        m.timer = duration.raw() > 0 ? duration : Fixed::from_raw(1);
        stat_mods_.push_back(m);
    }
    size_t stat_mod_count() const { return stat_mods_.size(); }

    Fixed repair(uint32_t target, Fixed hp_per_sec, Fixed dt) {
        if (target >= units_.size()) return Fixed();
        Unit& u = units_[target];
        if (!u.alive || u.hp >= u.hp_max) return Fixed();
        Fixed add = hp_per_sec * dt;
        if (u.hp + add > u.hp_max) add = u.hp_max - u.hp;
        u.hp = u.hp + add;
        return add;
    }

    void apply_knockback_from(uint32_t target, Fixed fx, Fixed fy, Fixed distance, Fixed duration) {
        if (target >= units_.size()) return;
        add_knockback(target, units_[target].x - fx, units_[target].y - fy, distance, duration);
    }
    void apply_knockback_to(uint32_t target, Fixed tx, Fixed ty, Fixed distance, Fixed duration) {
        if (target >= units_.size()) return;
        add_knockback(target, tx - units_[target].x, ty - units_[target].y, distance, duration);
    }
    size_t knockback_count() const { return knockbacks_.size(); }

    int32_t add_spawn_proto(Unit proto) {
        spawn_protos_.push_back(proto);
        return int32_t(spawn_protos_.size()) - 1;
    }

    void begin_morph(uint32_t id, int32_t proto, Fixed time) {
        if (id >= units_.size() || proto < 0 || proto >= int32_t(spawn_protos_.size())) return;
        units_[id].morph_proto = proto;
        units_[id].morph_timer = time;
    }
    bool is_morphing(uint32_t id) const {
        return id < units_.size() && units_[id].morph_proto >= 0;
    }

    void begin_spawner(uint32_t id, int32_t proto, int32_t cap, Fixed period, int32_t hard_cap = 0) {
        if (id >= units_.size() || proto < 0 || proto >= int32_t(spawn_protos_.size())) return;
        units_[id].spawner_proto = proto;
        units_[id].spawner_cap = cap;
        units_[id].spawner_hard_cap = hard_cap;
        units_[id].spawner_period = period;
        units_[id].spawner_timer = period;
    }
    int spawner_brood(uint32_t id) const {
        int n = 0;
        for (const Unit& u : units_)
            if (u.alive && u.spawn_parent == id && !u.larva_injected) ++n;
        return n;
    }
    int larva_total(uint32_t id) const {
        int n = 0;
        for (const Unit& u : units_)
            if (u.alive && u.spawn_parent == id) ++n;
        return n;
    }

    bool cast_inject(uint32_t queen, uint32_t hall, Fixed energy_cost, int32_t proto, int32_t count,
                     Fixed hatch_delay, Fixed stun_dur) {
        if (queen >= units_.size() || hall >= units_.size()) return false;
        if (proto < 0 || proto >= int32_t(spawn_protos_.size()) || count <= 0) return false;
        Unit& q = units_[queen];
        if (q.energy < energy_cost) return false;
        q.energy = q.energy - energy_cost;
        Unit& h = units_[hall];
        h.inject_proto = proto;
        h.inject_count = count;
        h.inject_timer = hatch_delay;
        if (stun_dur.raw() > 0) {
            BehaviorInstance b;
            b.kind = kCastBehaviorKind;
            b.timer = stun_dur;
            b.stun = true;
            add_behavior(queen, b);
        }
        return true;
    }
    bool inject_pending(uint32_t id) const {
        return id < units_.size() && units_[id].inject_proto >= 0;
    }

    bool train_larva(uint32_t larva, const Unit& proto, Fixed build_time,
                     int32_t mineral_cost, int32_t gas_cost) {
        if (larva >= units_.size()) return false;
        Unit& lv = units_[larva];
        if (!lv.alive || lv.spawn_parent == kNoTarget) return false;
        const uint32_t hall = lv.spawn_parent;
        if (hall >= units_.size()) return false;
        Player& pl = player(lv.player);
        if (pl.minerals < mineral_cost || pl.gas < gas_cost) return false;
        const Fixed hx = lv.x, hy = lv.y;
        lv.alive = false;
        return queue_train_at(hall, proto, mineral_cost, gas_cost, build_time, hx, hy);
    }

    bool warp_train(uint32_t gate, const Unit& proto, Fixed tx, Fixed ty, Fixed forming_time,
                    Fixed cooldown, int32_t mineral_cost, int32_t gas_cost) {
        if (gate >= units_.size()) return false;
        Unit& g = units_[gate];
        if (!g.alive || g.warp_cd.raw() > 0 || proto.hp_max.raw() <= 0) return false;
        const int32_t owner = g.player;
        if (!powered_at(tx, ty, owner)) return false;
        Player& pl = player(owner);
        if (pl.minerals < mineral_cost || pl.gas < gas_cost) return false;
        pl.minerals -= mineral_cost;
        pl.gas -= gas_cost;
        g.warp_cd = cooldown;
        Unit u = proto;
        u.player = owner;
        u.x = tx;
        u.y = ty;
        const uint32_t id = add_unit(u);
        start_construction(id, forming_time, units_[id].hp_max);
        return true;
    }
    bool warp_ready(uint32_t gate) const {
        return gate < units_.size() && units_[gate].alive && units_[gate].warp_cd.raw() <= 0;
    }

    bool powered_at(Fixed x, Fixed y, int32_t player) const {
        for (const Unit& s : units_) {
            if (!s.alive || s.power_radius.raw() <= 0 || !are_allied(s.player, player)) continue;
            const Fixed dx = x - s.x, dy = y - s.y;
            if (dx * dx + dy * dy <= s.power_radius * s.power_radius) return true;
        }
        return false;
    }

    int cargo_used(uint32_t transport) const {
        int n = 0;
        for (const Unit& u : units_)
            if (u.alive && u.loaded && u.transport_id == transport) n += u.cargo_size;
        return n;
    }
    bool load_unit(uint32_t transport, uint32_t unit) {
        if (transport >= units_.size() || unit >= units_.size() || transport == unit) return false;
        Unit& t = units_[transport];
        Unit& u = units_[unit];
        if (!t.alive || !u.alive || t.cargo_space <= 0 || u.loaded || u.cargo_size <= 0) return false;
        if (cargo_used(transport) + u.cargo_size > t.cargo_space) return false;
        u.loaded = true;
        u.transport_id = transport;
        return true;
    }
    int unload_all(uint32_t transport) {
        if (transport >= units_.size()) return 0;
        const Fixed tx = units_[transport].x, ty = units_[transport].y;
        static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        int n = 0;
        for (Unit& u : units_) {
            if (!u.alive || !u.loaded || u.transport_id != transport) continue;
            u.loaded = false;
            u.transport_id = kNoTarget;
            u.x = tx + Fixed::from_int(DX[n & 7]);
            u.y = ty + Fixed::from_int(DY[n & 7]);
            ++n;
        }
        return n;
    }

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
        bool has_pos = false;
        Fixed px, py;
    };

    struct WorkerBuild {
        uint32_t builder;
        Fixed x, y;
        Unit proto;
        Fixed build_time;
        Fixed arrive;
    };

    std::vector<Unit> units_;
    std::vector<Player> players_;
    std::vector<Pending> pending_;
    std::vector<WorkerBuild> worker_builds_;
    std::vector<PersistentInstance> persistents_;
    std::vector<StatModInstance> stat_mods_;
    std::vector<KnockImpulse> knockbacks_;
    std::vector<ForceFieldInstance> force_fields_;
    void add_knockback(uint32_t target, Fixed vx, Fixed vy, Fixed distance, Fixed duration) {
        if (distance.raw() <= 0 || duration.raw() <= 0) return;
        const Fixed len = isqrt(vx * vx + vy * vy);
        KnockImpulse im;
        im.target = target;
        if (len.raw() <= 0) { im.dx = Fixed::from_int(1); im.dy = Fixed(); }
        else { im.dx = vx / len; im.dy = vy / len; }
        im.remaining = distance;
        im.speed = distance / duration;
        knockbacks_.push_back(im);
    }
    static void apply_stat_delta(Unit& u, StatKind stat, Fixed d) {
        switch (stat) {
            case StatKind::Armor: u.armor = u.armor + d; break;
            case StatKind::WeaponDamage: u.wpn_damage = u.wpn_damage + d; break;
            case StatKind::WeaponRange: u.wpn_range = u.wpn_range + d; break;
            case StatKind::LifeMax:
                u.hp_max = u.hp_max + d;
                if (u.hp > u.hp_max) u.hp = u.hp_max;
                break;
        }
    }
    std::vector<Unit> spawn_protos_;
    struct PendingSpawn { int32_t proto; int32_t count; Fixed x, y; int32_t owner;
                          uint32_t parent = kNoTarget; bool injected = false; };
    std::vector<PendingSpawn> pending_spawns_;
    std::vector<VeterancyDef> vet_defs_;
    std::vector<AbilityDef> abilities_;
    CreepSystem creep_;
    NavGrid nav_;
    Rng rng_;
    Fixed game_speed_ = Fixed::from_int(1);
    int tick_ = 0;
    bool separation_ = false;
    bool fog_acquire_ = false;

    uint32_t acquire_target(const Unit& u) const;
    bool detected_by(int32_t player, const Unit& target) const {
        for (const Unit& d : units_) {
            if (!d.alive || d.detect_range.raw() <= 0 || !are_allied(d.player, player)) continue;
            const Fixed dx = d.x - target.x, dy = d.y - target.y;
            if (dx * dx + dy * dy <= d.detect_range * d.detect_range) return true;
        }
        return false;
    }
    Fixed move_step(const Unit& u, Fixed dt) const;
    bool move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step);
    void apply_separation();
    void step_gather(Unit& u, Fixed dt);
    void step_cast(Unit& u, Fixed dt);
    void step_production(Fixed dt);
    void step_worker_builds();
    void step_construction(Unit& u, Fixed dt);
    void step_persistents(Fixed dt);
    void step_bunker_fire(Fixed dt);
    void step_auras();
    void step_power();
    void step_cloak_field();
    static Fixed behavior_speed_mult(const Unit& u);
    static bool behavior_stunned(const Unit& u);
    static bool behavior_rooted(const Unit& u);
    void award_kill_xp(Unit& killer, int64_t victim_xp);
    static Fixed dist2(const Unit& a, const Unit& b);
};

}
