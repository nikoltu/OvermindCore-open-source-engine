#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "sc2/sim/creep.hpp"
#include "sc2/sim/death_reveal.hpp"
#include "sc2/sim/fixed.hpp"
#include "sc2/sim/nav.hpp"
#include "sc2/sim/rng.hpp"
#include "sc2/sim/scan_sweep.hpp"
#include "sc2/sim/terrain_map.hpp"

namespace sc2::sim {

constexpr uint32_t kNoTarget = 0xFFFFFFFF;

inline constexpr double kInstantTurn = 720.0;
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
    Unload,
    Load,
};

enum class BuildVerb : uint8_t { Committed, Warp, Morph };

enum class EffectKind {
    Damage,
    Heal,
    Buff,
    ModifyEnergy,
    DrainEnergy,
    ApplyBehavior,
    Teleport,
    CreatePersistent,
    CreateUnit,
    Morph,
    RemoveBehavior,
    Set,
    EnumArea,
    Switch,
    IssueOrder,
    CreateHealer,
    WeaponDamage,
    Reveal,
    Force,
    PhaseShift,
    Inject,
};

inline constexpr std::uint16_t kCastBehaviorKind = 0xCA5;
inline constexpr std::uint16_t kCastBehaviorKindControl = 0xCA7;

using EffectRef = int32_t;
inline constexpr EffectRef kNoEffect = -1;
inline constexpr std::int16_t kIssueOrderStop = 100;
inline constexpr std::uint8_t kTargetSortDefault = 0;
inline constexpr std::uint8_t kTargetSortLifeFractionLow = 1;

struct AbilityDef {
    EffectKind kind = EffectKind::Damage;
    Fixed amount;
    Fixed duration;
    Fixed range;
    Fixed energy_fraction{};
    Fixed move_speed_mult = Fixed::from_int(1);
    Fixed attack_speed_mult = Fixed::from_int(1);
    bool beh_root = false;
    bool beh_stun = false;
    bool beh_lift = false;
    bool beh_control_transfer = false;
    Fixed beh_dmg_per_tick{};
    Fixed beh_dmg_period{};
    Fixed radius{};
    Fixed period{};
    int32_t period_count = 0;
    int32_t spawn_proto = -1;
    int32_t spawn_count = 0;
    bool spawn_snap_resource = false;
    int32_t mineral_cost = 0, gas_cost = 0;
    Fixed morph_heal_life_frac{}, morph_heal_shield_frac{};
    EffectRef root = kNoEffect;
};

enum : std::uint8_t {
    kAffilSelf = 1,
    kAffilPlayer = 2,
    kAffilAlly = 4,
    kAffilEnemy = 8,
    kAffilNeutral = 16,
};
inline constexpr std::uint8_t kAffilNotOwn = kAffilAlly | kAffilEnemy | kAffilNeutral;
inline constexpr std::uint8_t kAffilAll = kAffilSelf | kAffilPlayer | kAffilNotOwn;

struct VitalGate {
    enum Which { Life, Energy, Shields } vital = Life;
    enum Cmp { LT, LE, GT, GE, EQ, NE } cmp = LT;
    Fixed value;
    bool fraction = false;
};

struct EffectNode {
    EffectKind kind = EffectKind::Damage;
    Fixed amount, duration, energy_fraction, radius, period;
    Fixed heal_drain_factor;
    Fixed shield_bonus;
    Fixed armor_reduction{Fixed::from_int(1)};
    Fixed move_speed_mult{Fixed::from_int(1)}, attack_speed_mult{Fixed::from_int(1)};
    bool beh_root = false, beh_stun = false;
    bool beh_lift = false;
    bool beh_control_transfer = false;
    Fixed beh_dmg_per_tick, beh_dmg_period;
    int32_t period_count = 0, spawn_proto = -1, spawn_count = 0;
    bool spawn_snap_resource = false;
    int32_t mineral_cost = 0, gas_cost = 0;
    Fixed morph_heal_life_frac, morph_heal_shield_frac;
    std::uint32_t wpn_bonus_attr = 0;
    Fixed wpn_bonus_amount;
    std::uint32_t behavior_tag = 0;
    bool beh_on_caster = false;
    std::vector<EffectRef> children;
    EffectRef initial = kNoEffect, periodic = kNoEffect, expire = kNoEffect, impact = kNoEffect;
    std::uint8_t search_affiliation = kAffilNotOwn;
    bool search_exclude_structure = false;
    std::vector<VitalGate> gates;
    std::vector<VitalGate> switch_case_gates;
    std::int16_t issue_order = -1;
    bool teleport_to_source = false;
};

struct EffectContext {
    uint32_t source = kNoTarget;
    uint32_t target = kNoTarget;
    Fixed sx, sy;
    Fixed tx, ty;
    int32_t origin = -1;
    Fixed range;
    bool has_weapon = false;
    Fixed wpn_amount;
    std::uint32_t wpn_bonus_attr = 0;
    Fixed wpn_bonus_amount;
    int32_t recall_index = -1;
};

struct Order {
    OrderType type = OrderType::Idle;
    Fixed tx{}, ty{};
    uint32_t target = kNoTarget;
    uint32_t ability = kNoAbility;
    Fixed px{}, py{};
};

struct BehaviorInstance {
    uint16_t kind = 0;
    uint32_t src = 0;
    Fixed timer;
    Fixed speed_mult = Fixed::from_int(1);
    bool root = false;
    bool stun = false;
    bool lift = false;
    Fixed dmg_per_tick;
    Fixed dmg_period;
    Fixed dmg_timer;
    std::uint32_t wpn_bonus_attr = 0;
    Fixed wpn_bonus_amount;
    int32_t ctrl_orig_owner = -2;
    std::uint32_t ctrl_caster = kNoTarget;
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
    EffectRef periodic = kNoEffect;
    std::vector<std::pair<Fixed, Fixed>> offsets;
    Fixed face_x = Fixed::from_int(1), face_y;
    int32_t applied = 0;
};

struct HealerInstance {
    std::uint32_t caster = kNoTarget;
    std::uint32_t target = kNoTarget;
    Fixed heal_rate;
    Fixed drain_factor;
    Fixed range;
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

inline uint32_t type_identity(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= uint8_t(c);
        h *= 16777619u;
    }
    return h;
}

struct Unit {
    uint32_t id = 0;
    int32_t player = 0;
    int32_t type_id = 0;
    std::string catalog_id;
    Fixed x, y;
    Fixed hp, hp_max, shields, armor;
    Fixed shields_max;
    Fixed shield_regen_rate, shield_regen_delay;
    Fixed shield_since_hit, shield_last_total;
    Fixed shield_armor;
    Fixed wpn_damage, wpn_period, wpn_range;
    Fixed wpn_min_range;
    Fixed wpn_damage_point;
    Fixed wpn_armor_reduction{Fixed::from_int(1)};
    Fixed wpn_projectile_speed;
    Fixed wpn_backswing;
    int32_t wpn_hits = 1;
    Fixed backswing_timer{};
    uint32_t wpn_bonus_attr = 0;
    Fixed wpn_bonus_amount;
    EffectRef wpn_effect = kNoEffect;
    Fixed speed, radius;
    Fixed sep_radius;
    Fixed inner_radius;
    uint32_t collide_flags = 0;
    int32_t push_priority = 10;
    int32_t allied_push_priority = 0;
    Fixed sight;
    Fixed death_reveal_radius{Fixed::from_int(2)};
    Fixed vision_height;
    int32_t vision_player = -1;
    Fixed speed_mult_creep = Fixed::from_int(1);
    bool can_jump = false;
    bool walks_cliffs = false;
    bool crushes_ff = false;
    int32_t creep_radius = 0;
    bool has_toggle_creep = false;
    bool creep_gen_on = false;
    int32_t toggle_creep_radius = 0;
    int32_t toggle_creep_period = 1;
    int32_t creep_source_idx = -1;
    int32_t creep_grow_period = 0;
    std::vector<std::pair<int16_t, int16_t>> footprint_offsets;
    bool requires_creep = false;
    bool requires_vespene = false;
    Fixed atk_speed_mult = Fixed::from_int(1);
    Fixed damage_reduction;
    Fixed aura_dmg_reduction, aura_radius;
    Fixed aura_reduction;
    Fixed dmg_fraction = Fixed::from_int(1);
    Fixed dmg_clamp_max;
    Fixed dmg_minimum;
    Fixed barrier;
    Fixed barrier_max;
    Fixed barrier_cd;
    Fixed barrier_cd_max;
    Fixed cooldown;
    uint32_t target = kNoTarget;
    bool alive = true;
    std::vector<Order> orders;

    Fixed supply_provides;
    Fixed supply_uses;
    int32_t resource_left = 0;
    int32_t miners_max = 0;
    int32_t yield_amount = 0;
    Fixed harvest_time;
    int32_t carrying = 0;
    uint32_t carry_source = kNoTarget;
    int32_t carry_amount = 5;
    bool carry_override = false;
    bool saturation_exempt = false;
    Fixed mine_time_mult;
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
    int32_t addon = 0;
    int32_t addon_building = 0;
    Fixed addon_build_left;

    bool has_rally = false;
    Fixed rally_x, rally_y;

    bool hold_fire = false;
    bool fire_while_moving = false;

    Fixed chrono_timer;

    Fixed life_regen;
    Fixed time_life;
    Fixed burrow_regen;
    bool burns_down = false;
    Fixed burn_timer;
    bool flees_when_attacked = false;
    uint32_t flee_from = kNoTarget;
    Fixed phase_timer;
    uint32_t phase_shade = kNoTarget;

    Fixed windup_timer;
    uint32_t windup_target = kNoTarget;
    Fixed windup_dmg;
    uint32_t windup_battr = 0;
    Fixed windup_bamt;
    Fixed windup_ps;
    Fixed windup_ar{Fixed::from_int(1)};
    int windup_hits = 1;

    Fixed crush_damage;
    Fixed crush_radius;

    Fixed buff_timer;
    Fixed buff_damage;

    Fixed morph_timer;
    int32_t morph_proto = -1;

    Fixed salvage_timer;
    int32_t salvage_minerals = 0;
    int32_t salvage_gas = 0;

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
    bool power_managed = false;
    Fixed morph_heal_life_frac, morph_heal_shield_frac;
    bool morph_pair = false;

    Fixed zone_x, zone_y, zone_radius;

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
    bool can_burrow_move = false;
    bool can_burrow_attack = false;
    struct BurrowWeapon {
        Fixed damage, period, range, min_range, damage_point, projectile_speed, backswing, bonus_amount, acquire;
        int32_t hits = 1;
        uint32_t target_filter = 3, bonus_attr = 0;
        int splash_line_count = 0;
        Fixed splash_line_first, splash_line_step, splash_line_radius;
        bool splash_line_from_source = false;
    };
    bool has_burrowed_wpn = false;
    bool burrowed_wpn_on = false;
    BurrowWeapon burrowed_wpn;
    void swap_weapon_block(BurrowWeapon& other) {
        std::swap(wpn_damage, other.damage);
        std::swap(wpn_period, other.period);
        std::swap(wpn_range, other.range);
        std::swap(wpn_min_range, other.min_range);
        std::swap(wpn_damage_point, other.damage_point);
        std::swap(wpn_projectile_speed, other.projectile_speed);
        std::swap(wpn_backswing, other.backswing);
        std::swap(wpn_bonus_amount, other.bonus_amount);
        std::swap(acquire_range, other.acquire);
        std::swap(wpn_hits, other.hits);
        std::swap(wpn_target_filter, other.target_filter);
        std::swap(wpn_bonus_attr, other.bonus_attr);
        std::swap(splash_line_count, other.splash_line_count);
        std::swap(splash_line_first, other.splash_line_first);
        std::swap(splash_line_step, other.splash_line_step);
        std::swap(splash_line_radius, other.splash_line_radius);
        std::swap(splash_line_from_source, other.splash_line_from_source);
    }

    bool has_toggle_wpn = false;
    bool toggle_wpn_on = false;
    Fixed toggle_wpn_drain;
    BurrowWeapon toggle_wpn;

    void swap_burrow_weapon() { swap_weapon_block(burrowed_wpn); }

    int32_t attack_spawn_proto = -1;
    int32_t attack_spawn_count = 0;

    bool flying = false;
    bool lowered = false;
    uint32_t attributes = 0;
    uint32_t wpn_target_filter = 3;
    uint8_t target_sort = 0;
    int32_t target_priority = 0;

    Fixed wpn2_damage, wpn2_period, wpn2_range;
    Fixed wpn2_min_range;
    uint32_t wpn2_target_filter = 3;
    uint32_t wpn2_bonus_attr = 0;
    Fixed wpn2_bonus_amount;
    Fixed wpn2_damage_point;
    Fixed wpn2_backswing;
    int32_t wpn2_hits = 1;
    Fixed wpn2_armor_reduction{Fixed::from_int(1)};
    Fixed wpn2_projectile_speed;

    Fixed splash_inner, splash_mid, splash_outer;
    Fixed no_splash_radius;
    Fixed splash_inner_frac = Fixed::from_int(1);
    Fixed splash_mid_frac = Fixed::from_ratio(1, 2);
    Fixed splash_outer_frac = Fixed::from_ratio(1, 4);

    int splash_line_count = 0;
    Fixed splash_line_first, splash_line_step, splash_line_radius;
    bool splash_line_from_source = false;

    Fixed arc_radius[2];
    Fixed arc_cos[2];
    Fixed arc_frac[2];

    static constexpr int kMaxBounce = 4;
    int bounce_count = 0;
    Fixed bounce_range;
    Fixed bounce_amount[kMaxBounce] = {};

    Fixed point_defense_radius;
    int point_defense_max = 0;

    Fixed acquire_range;

    Fixed turn_rate;
    Fixed stationary_turn_rate;
    Fixed accel, decel;
    Fixed lateral_accel;
    Fixed cur_speed;
    bool moving_now = false;
    bool moved_this_tick = false;
    Fixed move_target_x, move_target_y;
    bool pushed_this_tick = false;
    std::uint8_t push_release = 0;
    Fixed push_dir_x, push_dir_y;
    bool has_anchor = false;
    Fixed anchor_x, anchor_y;
    std::uint8_t air_expand = 0;
    std::int32_t air_onset_tick = 0;
    bool ever_ordered = false;
    std::uint32_t cast_count = 0;
    Fixed facing;
    Fixed guard_x{}, guard_y{};
    bool guarding = false;

    int32_t vet_index = -1;
    int64_t xp = 0;
    int32_t level = 0;
    int64_t kill_xp = 0;
    bool heroic = false;
    bool hero_ui = false;

    std::vector<std::pair<int32_t, Fixed>> ability_cd;

    std::vector<Waypoint> path;
    size_t path_next = 0;
    int32_t path_gx = INT32_MIN;
    int32_t path_gy = INT32_MIN;
};

inline bool vital_gate_passes(const VitalGate& g, const Unit& t) {
    Fixed cur, max;
    switch (g.vital) {
        case VitalGate::Life: cur = t.hp; max = t.hp_max; break;
        case VitalGate::Energy: cur = t.energy; max = t.energy_max; break;
        case VitalGate::Shields: cur = t.shields; max = t.shields_max; break;
    }
    const Fixed lhs = g.fraction ? (max.raw() > 0 ? cur / max : Fixed()) : cur;
    switch (g.cmp) {
        case VitalGate::LT: return lhs < g.value;
        case VitalGate::LE: return lhs <= g.value;
        case VitalGate::GT: return lhs > g.value;
        case VitalGate::GE: return lhs >= g.value;
        case VitalGate::EQ: return lhs == g.value;
        case VitalGate::NE: return !(lhs == g.value);
    }
    return true;
}

struct Player {
    int32_t minerals = 50;
    int32_t gas = 0;
    int32_t team = -1;
};

struct VeterancyDef {
    std::vector<int64_t> xp_threshold;
    std::vector<Fixed> cum_life;
    std::vector<Fixed> cum_damage;
    Fixed xp_fraction = Fixed::from_int(1);
    int max_level() const { return int(xp_threshold.size()) - 1; }
};

enum class CommandKind : uint8_t {
    Train = 0,
    TrainAt = 1,
    TrainLarva = 2,
    WorkerBuild = 3,
    Addon = 4,
    RallySet = 5,
    RallyClear = 6,
    Research = 7,
};

struct Command {
    CommandKind kind = CommandKind::Train;
    Unit proto;
    Fixed x, y;
    Fixed build_time;
    Fixed arrive;
    int32_t mineral_cost = 0;
    int32_t gas_cost = 0;
    int32_t count = 1;
    int32_t addon_kind = 0;
    BuildVerb verb = BuildVerb::Committed;
    std::string research_id;
};

struct OrderSink {
    virtual ~OrderSink() = default;
    virtual void on_order(int tick, uint32_t id, const Order& o, bool queued) = 0;
    virtual void on_stop(int tick, uint32_t id) = 0;
    virtual void on_command(int tick, uint32_t id, const Command& c) = 0;
};

class World {
public:
    static constexpr int kTicksPerSecond = 16;
    static Fixed dt() { return Fixed::from_ratio(1, kTicksPerSecond); }

    explicit World(uint64_t seed = 1) : rng_(seed) {}

    uint32_t add_unit(Unit u) {
        u.id = uint32_t(units_.size());
        u.air_onset_tick = tick_;
        if (weapon_executor_ && u.wpn_effect == kNoEffect &&
            (u.wpn_damage.raw() > 0 || u.wpn2_damage.raw() > 0))
            u.wpn_effect = ensure_weapon_effect();
        if (!attack_spawn_reg_.empty() && u.attack_spawn_proto < 0) {
            auto it = attack_spawn_reg_.find(u.catalog_id);
            if (it != attack_spawn_reg_.end()) {
                u.attack_spawn_proto = it->second.first;
                u.attack_spawn_count = it->second.second;
            }
        }
        if (!interceptor_arm_reg_.empty()) {
            auto it = interceptor_arm_reg_.find(u.catalog_id);
            if (it != interceptor_arm_reg_.end()) {
                const InterceptorArm& a = it->second;
                Carrier c;
                c.carrier = u.id;
                c.count = a.count;
                c.count_max = a.count_max;
                c.leash = a.leash;
                c.build_time = a.build_time;
                c.build_timer = a.build_time;
                c.proto_idx = a.proto_idx;
                carriers_.push_back(std::move(c));
            }
        }
        units_.push_back(u);
        if (u.creep_radius > 0) {
            const int period = u.creep_grow_period > 0 ? u.creep_grow_period : 1;
            auto mit = creep_masks_.find(u.catalog_id);
            units_.back().creep_source_idx =
                (mit != creep_masks_.end() && !mit->second.empty())
                    ? creep_.add_source(u.x, u.y, u.creep_radius, period, mit->second)
                    : creep_.add_source(u.x, u.y, u.creep_radius, period);
        }
        return u.id;
    }

    void set_order_sink(OrderSink* s) { order_sink_ = s; }
    OrderSink* order_sink() const { return order_sink_; }

    void issue(uint32_t id, const Order& o, bool queued = false, bool player_order = true) {
        if (id >= units_.size()) return;
        if (order_sink_ && !in_step_) order_sink_->on_order(tick_, id, o, queued);
        detach_builder(id);
        Unit& u = units_[id];
        if (!queued) u.orders.clear();
        u.orders.push_back(o);
        if (player_order) u.ever_ordered = true;
    }
    void stop(uint32_t id) {
        if (id >= units_.size()) return;
        if (order_sink_ && !in_step_) order_sink_->on_stop(tick_, id);
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
        if (reporting()) {
            Command c;
            c.kind = CommandKind::RallySet;
            c.x = x;
            c.y = y;
            report_command(id, c);
        }
        units_[id].has_rally = true;
        units_[id].rally_x = x;
        units_[id].rally_y = y;
    }
    void clear_rally(uint32_t id) {
        if (id >= units_.size()) return;
        if (reporting()) {
            Command c;
            c.kind = CommandKind::RallyClear;
            report_command(id, c);
        }
        units_[id].has_rally = false;
    }

    Order smart_order(uint32_t actor, uint32_t target, Fixed tx, Fixed ty) const;

    void issue_smart(uint32_t actor, uint32_t target, Fixed tx, Fixed ty,
                     bool queued = false) {
        issue(actor, smart_order(actor, target, tx, ty), queued);
    }

    bool teleport_unit(uint32_t id, Fixed tx, Fixed ty);

    void apply_spell_damage(uint32_t target, Fixed dmg, uint32_t caster,
                            Fixed armor_reduction = Fixed::from_int(1));

    void apply_weapon_hit(std::uint32_t target, Fixed base, std::uint32_t bonus_attr,
                          Fixed bonus_amount, std::uint32_t attacker,
                          Fixed armor_reduction = Fixed::from_int(1));

    void record_flee_trigger(Unit& target, std::uint32_t attacker, Fixed vitals_before);

    void apply_weapon_fire(std::uint32_t attacker, std::uint32_t target, Fixed w_dmg,
                           std::uint32_t w_battr, Fixed w_bamt, int hits = 1,
                           Fixed w_ar = Fixed::from_int(1));
    void step_windup(Fixed dt);
    void step_projectiles(Fixed dt);
    void step_point_defense(Fixed dt);
    void queue_projectile(std::uint32_t attacker, Fixed px, Fixed py, std::uint32_t target, Fixed speed,
                          Fixed dmg, std::uint32_t battr, Fixed bamt, Fixed ar = Fixed::from_int(1),
                          int hits = 1);

    Fixed heal_unit(uint32_t id, Fixed amount);

    Unit& unit(uint32_t id) { return units_[id]; }

    void step();
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) step();
    }

    Player& player(int32_t p);
    const Player& player(int32_t p) const;

    int32_t add_ability(AbilityDef def);
    size_t ability_count() const { return abilities_.size(); }
    EffectRef add_effect(EffectNode n) {
        effects_.push_back(std::move(n));
        return EffectRef(effects_.size()) - 1;
    }
    void set_weapon_executor(bool on) {
        weapon_executor_ = on;
        if (!on) return;
        for (Unit& u : units_)
            if (u.wpn_effect == kNoEffect &&
                (u.wpn_damage.raw() > 0 || u.wpn2_damage.raw() > 0))
                u.wpn_effect = ensure_weapon_effect();
    }
    bool weapon_executor() const { return weapon_executor_; }
    int32_t add_ability_with_root(AbilityDef def, EffectRef root) {
        def.root = root;
        abilities_.push_back(def);
        return int32_t(abilities_.size()) - 1;
    }

    void cast(uint32_t caster, int32_t ability, uint32_t target) {
        Order o;
        o.type = OrderType::Cast;
        o.ability = uint32_t(ability);
        o.target = target;
        if (target < units_.size() && units_[target].alive) {
            o.tx = units_[target].x;
            o.ty = units_[target].y;
        } else if (caster < units_.size()) {
            o.tx = units_[caster].x;
            o.ty = units_[caster].y;
        }
        issue(caster, o, false);
    }

    void cast(uint32_t caster, int32_t ability, Fixed tx, Fixed ty) {
        Order o;
        o.type = OrderType::Cast;
        o.ability = uint32_t(ability);
        o.target = kNoTarget;
        o.tx = tx;
        o.ty = ty;
        issue(caster, o, false);
    }

    int32_t add_veterancy(VeterancyDef def);
    Fixed supply_used(int32_t p) const;
    Fixed supply_max(int32_t p) const;

    static bool is_neutral(int32_t p) { return p < 0; }
    int32_t team_of(int32_t p) const;
    bool are_allied(int32_t a, int32_t b) const;

    bool queue_train(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                     int32_t gas_cost, Fixed build_time, int32_t count = 1);
    bool queue_train_at(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                        int32_t gas_cost, Fixed build_time, Fixed x, Fixed y, int32_t count = 1);
    size_t pending_count() const { return pending_.size(); }
    bool cancel_train(uint32_t producer);

    bool cancel_construction(uint32_t building, int32_t mineral_cost, int32_t gas_cost);
    bool cancel_worker_build(uint32_t builder, int32_t mineral_cost, int32_t gas_cost);

    void set_tech_requirement(std::string unit_id, std::vector<std::string> prereqs);
    bool tech_requirements_met(int32_t player, const std::string& unit_id) const;

    static constexpr int32_t kAddonNone = 0, kAddonTechLab = 1, kAddonReactor = 2;
    void attach_addon(uint32_t building, int32_t kind) {
        if (building >= units_.size()) return;
        units_[building].addon = kind;
        units_[building].train_slots = (kind == kAddonReactor) ? 2 : 1;
    }
    int32_t addon_of(uint32_t building) const {
        return building < units_.size() ? units_[building].addon : kAddonNone;
    }
    bool build_addon(uint32_t building, int32_t kind, Fixed build_time) {
        if (building >= units_.size()) return false;
        Unit& b = units_[building];
        if (!b.alive || b.constructing) return false;
        if (b.addon != kAddonNone || b.addon_building != 0) return false;
        if (kind != kAddonTechLab && kind != kAddonReactor) return false;
        if (reporting()) {
            Command cmd;
            cmd.kind = CommandKind::Addon;
            cmd.addon_kind = kind;
            cmd.build_time = build_time;
            report_command(building, cmd);
        }
        if (build_time.raw() <= 0) { attach_addon(building, kind); return true; }
        b.addon_building = kind;
        b.addon_build_left = build_time;
        return true;
    }
    void require_attached_techlab(std::string unit_id) { techlab_gated_.insert(std::move(unit_id)); }

    bool issue_worker_build(uint32_t builder, Fixed x, Fixed y, const Unit& proto,
                            int32_t mineral_cost, int32_t gas_cost, Fixed build_time,
                            Fixed arrive = Fixed::from_int(2),
                            BuildVerb verb = BuildVerb::Committed);
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
        register_footprint_block(id);
    }
    bool lift_off(uint32_t id) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (!u.alive || u.flying) return false;
        u.flying = true;
        return true;
    }
    bool land(uint32_t id, Fixed x, Fixed y) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (!u.alive || !u.flying) return false;
        for (const Unit& o : units_) {
            if (!o.alive || o.id == id || o.flying) continue;
            const Fixed dx = o.x - x, dy = o.y - y;
            const Fixed rr = u.radius + o.radius;
            if (dx * dx + dy * dy < rr * rr) return false;
        }
        u.x = x;
        u.y = y;
        u.flying = false;
        register_footprint_block(id);
        return true;
    }
    bool lower(uint32_t id) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (!u.alive || u.lowered) return false;
        u.lowered = true;
        return true;
    }
    bool raise(uint32_t id) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (!u.alive || !u.lowered) return false;
        for (const Unit& o : units_) {
            if (!o.alive || o.id == id || o.flying) continue;
            const Fixed dx = o.x - u.x, dy = o.y - u.y;
            const Fixed rr = u.radius + o.radius;
            if (dx * dx + dy * dy < rr * rr) return false;
        }
        u.lowered = false;
        register_footprint_block(id);
        return true;
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
    bool spend_life(uint32_t id, Fixed amount) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (u.hp <= amount) return false;
        u.hp = u.hp - amount;
        return true;
    }
    Fixed energy(uint32_t id) const {
        return id < units_.size() ? units_[id].energy : Fixed();
    }

    Fixed ability_cooldown_left(uint32_t caster, int32_t slot) const {
        if (caster >= units_.size()) return Fixed();
        for (const auto& e : units_[caster].ability_cd)
            if (e.first == slot) return e.second;
        return Fixed();
    }
    void arm_ability_cooldown(uint32_t caster, int32_t slot, Fixed dur) {
        if (caster >= units_.size() || dur.raw() <= 0) return;
        auto& cds = units_[caster].ability_cd;
        for (auto& e : cds)
            if (e.first == slot) { e.second = dur; return; }
        auto pos = cds.begin();
        while (pos != cds.end() && pos->first < slot) ++pos;
        cds.insert(pos, {slot, dur});
    }

    bool begin_salvage(uint32_t structure, Fixed delay, int32_t refund_minerals, int32_t refund_gas) {
        if (structure >= units_.size()) return false;
        Unit& u = units_[structure];
        if (!u.alive || u.player < 0 || u.salvage_timer.raw() > 0) return false;
        if (refund_minerals < 0) refund_minerals = 0;
        if (refund_gas < 0) refund_gas = 0;
        if (delay.raw() <= 0) {
            Player& p = player(u.player);
            p.minerals += refund_minerals;
            p.gas += refund_gas;
            u.alive = false;
            return true;
        }
        u.salvage_timer = delay;
        u.salvage_minerals = refund_minerals;
        u.salvage_gas = refund_gas;
        return true;
    }

    bool set_cloak(uint32_t id, bool engage) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (engage && u.energy.raw() <= 0) return false;
        u.cloaked = engage;
        return u.cloaked;
    }

    bool set_weapon_toggle(uint32_t id, bool engage) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (!u.has_toggle_wpn) return false;
        if (engage == u.toggle_wpn_on) return u.toggle_wpn_on;
        if (engage && u.energy.raw() <= 0) return false;
        u.swap_weapon_block(u.toggle_wpn);
        u.toggle_wpn_on = engage;
        return u.toggle_wpn_on;
    }

    bool set_creep_generation(uint32_t id, bool engage) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (!u.has_toggle_creep) return false;
        if (engage == u.creep_gen_on) return u.creep_gen_on;
        if (engage) {
            const int period = u.toggle_creep_period > 0 ? u.toggle_creep_period : 1;
            auto mit = creep_masks_.find(u.catalog_id);
            u.creep_source_idx = (mit != creep_masks_.end() && !mit->second.empty())
                                     ? creep_.add_source(u.x, u.y, u.toggle_creep_radius, period,
                                                         mit->second)
                                     : creep_.add_source(u.x, u.y, u.toggle_creep_radius, period);
        } else if (u.creep_source_idx >= 0) {
            creep_.kill_source(u.creep_source_idx);
            u.creep_source_idx = -1;
        }
        u.creep_gen_on = engage;
        return u.creep_gen_on;
    }

    int32_t add_persistent(PersistentInstance p);
    size_t persistent_count() const { return persistents_.size(); }

    bool start_heal(std::uint32_t caster, std::uint32_t target, Fixed heal_rate, Fixed drain_factor,
                    Fixed range);
    size_t healer_count() const { return healers_.size(); }

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
                        nav_.mark_ff(cx, cy, true);
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

    uint32_t nearest_mineral_node(Fixed x, Fixed y) const {
        uint32_t node = kNoTarget;
        Fixed best;
        for (uint32_t i = 0; i < units_.size(); ++i) {
            const Unit& n = units_[i];
            if (!n.alive || n.resource_left <= 0 || n.is_gas) continue;
            const Fixed dx = n.x - x, dy = n.y - y;
            const Fixed d2 = dx * dx + dy * dy;
            if (node == kNoTarget || d2 < best) { best = d2; node = i; }
        }
        return node;
    }

    void begin_morph(uint32_t id, int32_t proto, Fixed time) {
        if (id >= units_.size() || proto < 0 || proto >= int32_t(spawn_protos_.size())) return;
        units_[id].morph_proto = proto;
        units_[id].morph_timer = time;
    }
    bool is_morphing(uint32_t id) const {
        return id < units_.size() && units_[id].morph_proto >= 0;
    }

    void set_zone(uint32_t id, Fixed cx, Fixed cy, Fixed radius) {
        if (id >= units_.size()) return;
        units_[id].zone_x = cx;
        units_[id].zone_y = cy;
        units_[id].zone_radius = radius;
    }
    void clear_zone(uint32_t id) {
        if (id < units_.size()) units_[id].zone_radius = Fixed();
    }
    bool has_zone(uint32_t id) const {
        return id < units_.size() && units_[id].zone_radius.raw() > 0;
    }

    bool merge_units(uint32_t keep, uint32_t absorb, int32_t proto, Fixed time) {
        if (keep >= units_.size() || absorb >= units_.size() || keep == absorb) return false;
        if (proto < 0 || proto >= int32_t(spawn_protos_.size())) return false;
        Unit& a = units_[keep];
        Unit& b = units_[absorb];
        if (!a.alive || !b.alive || a.player != b.player) return false;
        b.alive = false;
        a.morph_proto = proto;
        a.morph_timer = time;
        return true;
    }

    bool phase_cancel(uint32_t adept) {
        if (adept >= units_.size()) return false;
        Unit& a = units_[adept];
        if (a.phase_timer.raw() <= 0) return false;
        if (a.phase_shade < units_.size()) units_[a.phase_shade].alive = false;
        a.phase_timer = Fixed();
        a.phase_shade = kNoTarget;
        return true;
    }
    bool is_phasing(uint32_t id) const { return id < units_.size() && units_[id].phase_timer.raw() > 0; }

    void begin_spawner(uint32_t id, int32_t proto, int32_t cap, Fixed period, int32_t hard_cap = 0) {
        if (id >= units_.size() || proto < 0 || proto >= int32_t(spawn_protos_.size())) return;
        units_[id].spawner_proto = proto;
        units_[id].spawner_cap = cap;
        units_[id].spawner_hard_cap = hard_cap;
        units_[id].spawner_period = period;
        units_[id].spawner_timer = period;
        for (int32_t i = 0; i < cap; ++i)
            pending_spawns_.push_back(
                {proto, 1, units_[id].x, units_[id].y, units_[id].player, id, false});
    }
    void set_larva_egg_proto(int32_t proto) {
        egg_proto_ = (proto >= 0 && proto < int32_t(spawn_protos_.size())) ? proto : -1;
        larva_egg_morph_ = egg_proto_ >= 0;
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
        if (reporting()) {
            Command cmd;
            cmd.kind = CommandKind::TrainLarva;
            cmd.proto = proto;
            cmd.build_time = build_time;
            cmd.mineral_cost = mineral_cost;
            cmd.gas_cost = gas_cost;
            report_command(larva, cmd);
        }
        const bool was_internal = in_step_;
        in_step_ = true;
        const bool queued = queue_train_at(hall, proto, mineral_cost, gas_cost, build_time, hx, hy);
        in_step_ = was_internal;
        return queued;
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
            if (!s.alive || s.constructing || s.power_radius.raw() <= 0 ||
                !are_allied(s.player, player))
                continue;
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
    int unload_all_at(uint32_t transport, Fixed atx, Fixed aty) {
        if (transport >= units_.size()) return 0;
        static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        int n = 0;
        for (Unit& u : units_) {
            if (!u.alive || !u.loaded || u.transport_id != transport) continue;
            u.loaded = false;
            u.transport_id = kNoTarget;
            u.x = atx + Fixed::from_int(DX[n & 7]);
            u.y = aty + Fixed::from_int(DY[n & 7]);
            ++n;
        }
        return n;
    }
    int unload_all(uint32_t transport) {
        if (transport >= units_.size()) return 0;
        return unload_all_at(transport, units_[transport].x, units_[transport].y);
    }
    int nydus_unload(uint32_t entrance, uint32_t exit_worm) {
        if (entrance >= units_.size() || exit_worm >= units_.size()) return 0;
        const Unit& ex = units_[exit_worm];
        if (!ex.alive || !are_allied(units_[entrance].player, ex.player)) return 0;
        return unload_all_at(entrance, ex.x, ex.y);
    }

    uint64_t state_hash() const;

    const std::vector<Unit>& units() const { return units_; }
    int tick() const { return tick_; }
    int alive_count(int player) const;

    CreepSystem& creep() { return creep_; }
    const CreepSystem& creep() const { return creep_; }
    void register_creep_mask(std::string catalog_id, std::vector<std::pair<int32_t, int32_t>> mask) {
        creep_masks_[std::move(catalog_id)] = std::move(mask);
    }

    void register_attack_spawn(std::string catalog_id, int32_t proto_idx, int32_t count) {
        attack_spawn_reg_[std::move(catalog_id)] = {proto_idx, count};
    }

    void register_interceptor_arm(std::string catalog_id, int32_t proto_idx, int32_t count,
                                  int32_t count_max, Fixed leash, Fixed build_time) {
        interceptor_arm_reg_[std::move(catalog_id)] =
            InterceptorArm{proto_idx, count, count_max, leash, build_time};
    }
    void step_interceptors(Fixed dt);
    int interceptors_deployed(uint32_t carrier) const {
        for (const Carrier& c : carriers_)
            if (c.carrier == carrier) {
                int n = 0;
                for (uint32_t id : c.wing)
                    if (id < units_.size() && units_[id].alive && !units_[id].loaded) ++n;
                return n;
            }
        return 0;
    }
    std::size_t carrier_count() const { return carriers_.size(); }

    void register_nova_launch(std::string catalog_id, Fixed speed, Fixed radius, Fixed damage,
                              Fixed shield_bonus, Fixed duration, Fixed cooldown = Fixed()) {
        nova_launch_reg_[std::move(catalog_id)] =
            NovaArm{speed, radius, damage, shield_bonus, duration, cooldown};
    }
    bool launch_nova(uint32_t caster, Fixed tx, Fixed ty);
    void step_nova(Fixed dt);
    int novas_active() const {
        int k = 0;
        for (const Nova& n : novas_)
            if (n.active) ++k;
        return k;
    }

    void set_death_reveal(bool on) { death_reveal_enabled_ = on; }
    bool death_reveal_enabled() const { return death_reveal_enabled_; }

    void enable_building_burndown(bool on = true) { building_burndown_enabled_ = on; }
    bool building_burndown_enabled() const { return building_burndown_enabled_; }

    void enable_multi_hit_armor(bool on = true) { multi_hit_armor_ = on; }
    bool multi_hit_armor() const { return multi_hit_armor_; }

    void enable_target_priority(bool on = true) { target_priority_ = on; }
    bool target_priority() const { return target_priority_; }

    void enable_flee_response(bool on = true) { flee_response_enabled_ = on; }
    bool flee_response_enabled() const { return flee_response_enabled_; }
    const DeathReveal& death_reveal() const { return death_reveal_; }
    bool death_revealed(int32_t player, Fixed x, Fixed y) const { return death_reveal_.covers(player, x, y); }

    const ScanSweep& scan_sweep() const { return scan_sweep_; }
    int drop_scan(Fixed x, Fixed y, Fixed radius, Fixed duration) {
        return scan_sweep_.drop(x, y, radius, duration);
    }

    NavGrid& nav() { return nav_; }
    const NavGrid& nav() const { return nav_; }

    TerrainMap& placement() { return placement_; }
    const TerrainMap& placement() const { return placement_; }

    void set_separation(bool on) { separation_ = on; }
    bool separation() const { return separation_; }

    void set_fog_acquire(bool on) { fog_acquire_ = on; }
    bool fog_acquire() const { return fog_acquire_; }
    void set_target_sort(bool on) { target_sort_ = on; }
    bool target_sort() const { return target_sort_; }

    void set_attack_windup(bool on) { attack_windup_ = on; }
    bool attack_windup() const { return attack_windup_; }
    void set_projectile_travel(bool on) { projectile_travel_ = on; }
    bool projectile_travel() const { return projectile_travel_; }
    void enable_point_defense(bool on = true) { point_defense_enabled_ = on; }
    bool point_defense_enabled() const { return point_defense_enabled_; }
    void set_reserve_supply(bool on) { reserve_supply_ = on; }
    bool reserve_supply() const { return reserve_supply_; }
    void set_turn_limit(bool on) { turn_limit_ = on; }
    bool turn_limit() const { return turn_limit_; }
    void set_acceleration(bool on) { acceleration_ = on; }
    bool acceleration() const { return acceleration_; }
    void set_arc_movement(bool on) { arc_movement_ = on; }
    bool arc_movement() const { return arc_movement_; }

    void set_leash(bool on) { leash_ = on; }
    bool leash() const { return leash_; }
    void set_backswing(bool on) { backswing_ = on; }
    bool backswing() const { return backswing_; }
    void set_fire_while_moving(bool on) { fwm_ = on; }
    bool fire_while_moving_enabled() const { return fwm_; }
    bool visible_to(int32_t player, const Unit& target) const;

    void register_watchtower(uint32_t tower_id, Fixed capture_radius) {
        watchtowers_.push_back(Watchtower{tower_id, capture_radius * capture_radius, -1});
    }
    void update_watchtowers();

    void register_destructible(uint32_t unit_id, int cx0, int cy0, int cx1, int cy1) {
        Destructible d;
        d.id = unit_id;
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x)
                if (nav_.in_bounds(x, y) && !nav_.is_blocked_cell(x, y)) {
                    nav_.set_blocked(x, y, true);
                    d.cells.emplace_back(x, y);
                }
        destructibles_.push_back(std::move(d));
    }
    void register_destructible_cells(uint32_t unit_id, const std::vector<std::pair<int, int>>& cells) {
        Destructible d;
        d.id = unit_id;
        for (const auto& [x, y] : cells)
            if (nav_.in_bounds(x, y) && !nav_.is_blocked_cell(x, y)) {
                nav_.set_blocked(x, y, true);
                d.cells.emplace_back(x, y);
            }
        destructibles_.push_back(std::move(d));
    }
    void register_footprint_block(uint32_t id) {
        if (id >= units_.size() || !nav_.configured()) return;
        const Unit& u = units_[id];
        if (!u.footprint_offsets.empty()) {
            int ocx, ocy;
            nav_.to_cell(u.x, u.y, ocx, ocy);
            std::vector<std::pair<int, int>> cells;
            cells.reserve(u.footprint_offsets.size());
            for (const auto& [dx, dy] : u.footprint_offsets)
                cells.emplace_back(ocx + int(dx), ocy + int(dy));
            register_destructible_cells(id, cells);
        } else if (u.radius.raw() > 0) {
            int cx0, cy0, cx1, cy1;
            nav_.to_cell(u.x - u.radius, u.y - u.radius, cx0, cy0);
            nav_.to_cell(u.x + u.radius, u.y + u.radius, cx1, cy1);
            register_destructible(id, cx0, cy0, cx1, cy1);
        }
    }
    void update_destructibles();

    void register_conjoined_group(std::vector<uint32_t> members, int32_t debris_proto = -1) {
        if (members.size() >= 2) conjoined_.push_back(Conjoined{std::move(members), false, debris_proto});
    }
    void update_conjoined();

    std::size_t set_conjoined_debris_proto(int32_t proto) {
        for (Conjoined& g : conjoined_) g.debris_proto = proto;
        return conjoined_.size();
    }
    std::size_t conjoined_count() const { return conjoined_.size(); }

    void return_dead_carry();

    void release_dead_creep_sources();

    int winner() const;

private:
    void emit_death_reveal(const Unit& victim, uint32_t killer);

    struct Watchtower {
        uint32_t tower_id;
        Fixed capture_r2;
        int32_t owner = -1;
    };
    std::vector<Watchtower> watchtowers_;

    struct Destructible {
        uint32_t id;
        std::vector<std::pair<int, int>> cells;
        bool cleared = false;
    };
    std::vector<Destructible> destructibles_;

    struct Conjoined {
        std::vector<uint32_t> members;
        bool collapsed = false;
        int32_t debris_proto = -1;
    };
    std::vector<Conjoined> conjoined_;

    struct InterceptorArm {
        int32_t proto_idx = -1;
        int32_t count = 0;
        int32_t count_max = 0;
        Fixed leash;
        Fixed build_time;
    };
    struct Carrier {
        uint32_t carrier = kNoTarget;
        int32_t count = 0;
        int32_t count_max = 0;
        Fixed leash;
        Fixed build_time;
        Fixed build_timer;
        int32_t proto_idx = -1;
        std::vector<uint32_t> wing;
    };
    std::unordered_map<std::string, InterceptorArm> interceptor_arm_reg_;
    std::vector<Carrier> carriers_;

    struct NovaArm {
        Fixed speed, radius, damage, shield_bonus, duration;
        Fixed cooldown;
    };
    static constexpr int32_t kNovaCooldownSlot = -1000;
    struct Nova {
        Fixed x, y, tx, ty;
        Fixed speed, radius, damage, shield_bonus;
        Fixed remaining;
        int32_t owner = -1;
        uint32_t caster = kNoTarget;
        bool active = false;
    };
    std::unordered_map<std::string, NovaArm> nova_launch_reg_;
    std::vector<Nova> novas_;

    struct Pending {
        uint32_t producer;
        Unit proto;
        Fixed timer;
        bool has_pos = false;
        Fixed px{}, py{};
        int32_t count = 1;
        int32_t mineral_cost = 0, gas_cost = 0;
        uint32_t consumed_larva = kNoTarget;
        uint32_t egg = kNoTarget;
    };

    struct WorkerBuild {
        uint32_t builder;
        Fixed x, y;
        Unit proto;
        Fixed build_time;
        Fixed arrive;
        BuildVerb verb = BuildVerb::Committed;
    };

    std::vector<Unit> units_;
    std::vector<Player> players_;
    std::vector<Pending> pending_;
    std::vector<WorkerBuild> worker_builds_;
    std::unordered_map<uint32_t, Unit> morph_founder_;
    std::vector<PersistentInstance> persistents_;
    std::vector<HealerInstance> healers_;
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
    static Fixed chrono_dt(const Unit& u, Fixed dt) {
        return u.chrono_timer.raw() > 0 ? dt + dt / Fixed::from_int(2) : dt;
    }
    std::vector<Unit> spawn_protos_;
    int32_t egg_proto_ = -1;
    bool larva_egg_morph_ = false;
    struct PendingSpawn { int32_t proto; int32_t count; Fixed x, y; int32_t owner;
                          uint32_t parent = kNoTarget; bool injected = false;
                          uint32_t phase_link = kNoTarget; Fixed move_x{}, move_y{};
                          uint32_t gather_node = kNoTarget, depot = kNoTarget; };
    std::vector<PendingSpawn> pending_spawns_;
    struct PendingHit {
        std::uint32_t attacker, target;
        Fixed px, py;
        Fixed speed;
        Fixed dmg;
        std::uint32_t battr;
        Fixed bamt;
        Fixed life;
        Fixed ar{Fixed::from_int(1)};
        int hits = 1;
    };
    std::vector<PendingHit> pending_hits_;
    std::vector<VeterancyDef> vet_defs_;
    std::vector<AbilityDef> abilities_;
    std::vector<EffectNode> effects_;
    bool weapon_executor_ = false;
    EffectRef weapon_effect_ = kNoEffect;
    EffectRef ensure_weapon_effect() {
        if (weapon_effect_ == kNoEffect) {
            EffectNode n;
            n.kind = EffectKind::WeaponDamage;
            weapon_effect_ = add_effect(std::move(n));
        }
        return weapon_effect_;
    }
    CreepSystem creep_;
    std::unordered_map<std::string, std::vector<std::pair<int32_t, int32_t>>> creep_masks_;
    std::unordered_map<std::string, std::pair<int32_t, int32_t>> attack_spawn_reg_;
    DeathReveal death_reveal_;
    bool death_reveal_enabled_ = false;
    ScanSweep scan_sweep_;
    bool building_burndown_enabled_ = false;
    bool flee_response_enabled_ = false;
    bool multi_hit_armor_ = false;
    bool target_priority_ = false;
    NavGrid nav_;
    TerrainMap placement_;
    Rng rng_;
    int tick_ = 0;
    OrderSink* order_sink_ = nullptr;
    bool in_step_ = false;
    bool reporting() const { return order_sink_ != nullptr && !in_step_; }
    void report_command(uint32_t id, const Command& c) {
        if (reporting()) order_sink_->on_command(tick_, id, c);
    }
    bool separation_ = false;
    bool fog_acquire_ = false;
    bool target_sort_ = false;
    bool attack_windup_ = false;
    bool projectile_travel_ = false;
    bool point_defense_enabled_ = false;
    bool reserve_supply_ = false;
    std::unordered_map<std::string, std::vector<std::string>> tech_reqs_;
    std::unordered_set<std::string> techlab_gated_;
    bool turn_limit_ = false;
    bool acceleration_ = false;
    bool leash_ = false;
    bool arc_movement_ = false;
    bool backswing_ = false;
    bool fwm_ = false;

    uint32_t acquire_target(const Unit& u, Fixed max_range = Fixed()) const;
    bool detected_by(int32_t player, const Unit& target) const {
        for (const Unit& d : units_) {
            if (!d.alive || d.detect_range.raw() <= 0 || !are_allied(d.player, player)) continue;
            const Fixed dx = d.x - target.x, dy = d.y - target.y;
            if (dx * dx + dy * dy <= d.detect_range * d.detect_range) return true;
        }
        return false;
    }
    Fixed move_step(Unit& u, Fixed dt);
    bool move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step);
    bool advance_turn(Unit& u, Fixed tx, Fixed ty);
    void apply_separation();
    void step_gather(Unit& u, Fixed dt);
    void step_cast(Unit& u, Fixed dt);
    void execute_effect(EffectRef ref, const EffectContext& ctx, int depth = 0);
    static constexpr int kMaxEffectDepth = 32;
    static bool is_point_self(EffectKind k) {
        return k == EffectKind::Teleport || k == EffectKind::CreatePersistent ||
               k == EffectKind::CreateUnit || k == EffectKind::Morph ||
               k == EffectKind::Reveal ||
               k == EffectKind::EnumArea ||
               k == EffectKind::PhaseShift ||
               k == EffectKind::IssueOrder;
    }
    bool root_is_point_cast(EffectRef root, int depth = 0) const {
        if (depth > kMaxEffectDepth || root < 0 || root >= EffectRef(effects_.size())) return false;
        const EffectNode& n = effects_[root];
        if (is_point_self(n.kind)) return true;
        if (n.kind == EffectKind::Set && !n.children.empty())
            return root_is_point_cast(n.children[0], depth + 1);
        return false;
    }
    void step_production(Fixed dt);
    void step_worker_builds();
    void step_construction(Unit& u, Fixed dt);
    void step_persistents(Fixed dt);
    void step_healers(Fixed dt);
    void step_bunker_fire(Fixed dt);
    void step_auras();
    void step_power();
    void step_cloak_field();
    static Fixed behavior_speed_mult(const Unit& u);
    static Fixed behavior_weapon_bonus(const Unit& u, std::uint32_t target_attrs);
    static bool behavior_stunned(const Unit& u);
    static bool behavior_rooted(const Unit& u);
    void award_kill_xp(Unit& killer, int64_t victim_xp);
    static Fixed dist2(const Unit& a, const Unit& b);
};

}
