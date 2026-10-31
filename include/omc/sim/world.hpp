#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "omc/sim/creep.hpp"
#include "omc/sim/death_reveal.hpp"
#include "omc/sim/fixed.hpp"
#include "omc/sim/nav.hpp"
#include "omc/sim/rng.hpp"
#include "omc/sim/scan_sweep.hpp"
#include "omc/sim/terrain_map.hpp"
#include "omc/sim/weapon.hpp"

namespace omc::sim {

constexpr uint32_t kNoTarget = 0xFFFFFFFF;

enum class CmdError : uint16_t {
    None = 0,
    NotEnoughMinerals,
    NotEnoughVespene,
    NotEnoughFood,
    TechRequirementsNotMet,
    CantBuildLocationBlocked,
    CantBuildTooCloseToResources,
    CantBuildTooFarFromCreepSource,
    CantBuildTooFarFromBuildPowerSource,
    CantBuildOnThat,
    MustBuildRefineryFirst,
    CantBuildTooCloseToCreepSource,
};

inline const char* cmd_error_key(CmdError e) {
    switch (e) {
        case CmdError::NotEnoughMinerals: return "e_cmdNotEnoughMinerals";
        case CmdError::NotEnoughVespene: return "e_cmdNotEnoughVespene";
        case CmdError::NotEnoughFood: return "e_cmdNotEnoughFood";
        case CmdError::TechRequirementsNotMet: return "e_cmdBuildTechRequirementsNotMet";
        case CmdError::CantBuildLocationBlocked: return "e_cmdCantBuildLocationBlocked";
        case CmdError::CantBuildTooCloseToResources: return "e_cmdCantBuildTooCloseToResources";
        case CmdError::CantBuildTooFarFromCreepSource: return "e_cmdCantBuildTooFarFromCreepSource";
        case CmdError::CantBuildTooFarFromBuildPowerSource: return "e_cmdCantBuildTooFarFromBuildPowerSource";
        case CmdError::CantBuildOnThat: return "e_cmdCantBuildOnThat";
        case CmdError::MustBuildRefineryFirst: return "e_cmdMustBuildRefineryFirst";
        case CmdError::CantBuildTooCloseToCreepSource: return "e_cmdCantBuildTooCloseToCreepSource";
        case CmdError::None: break;
    }
    return "";
}

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
    Repair,
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
    ModifyVitals,
    DestroyPersistent,
    CancelOrder,
    TransferBehavior,
    UseCalldown,
    WeaponCooldown,
    LaunchMissile,
    RedirectPassengers,
};

inline constexpr std::uint16_t kCastBehaviorKind = 0xCA5;
inline constexpr std::uint16_t kCastBehaviorKindControl = 0xCA7;

using EffectRef = int32_t;
inline constexpr EffectRef kNoEffect = -1;
inline constexpr std::int16_t kIssueOrderStop = 100;
inline constexpr std::uint8_t kTargetSortDefault = 0;
inline constexpr std::uint8_t kTargetSortLifeFractionLow = 1;

inline constexpr std::uint8_t kAcquireSortDefault = 0;
inline constexpr std::uint8_t kAcquireSortByAngle = 1;
inline constexpr std::uint8_t kAcquireSortByDistanceFromTarget = 2;

inline constexpr std::uint8_t kDamageKindSpell = 1, kDamageKindMelee = 2, kDamageKindRanged = 4, kDamageKindSplash = 8;
inline constexpr const char* kDeathKindNames[] = {"Normal",     "Blast",      "Disintegrate", "Fire",    "Eviscerate",
                                                  "Silentkill", "Squish",     "Electrocute",  "Eat",     "Remove",
                                                  "Timeout",    "Salvage"};
inline std::uint8_t death_code_of(const std::string& name) {
    for (std::uint8_t i = 0; i < sizeof(kDeathKindNames) / sizeof(kDeathKindNames[0]); ++i)
        if (name == kDeathKindNames[i]) return i;
    return 0;
}
inline const char* death_name_of(std::uint8_t code) {
    return code < sizeof(kDeathKindNames) / sizeof(kDeathKindNames[0]) ? kDeathKindNames[code] : "Normal";
}

struct AbilityDef {
    EffectKind kind = EffectKind::Damage;
    Fixed amount;
    Fixed duration;
    Fixed range;
    Fixed energy_fraction{};
    Fixed energy_change{}, energy_change_frac{};
    Fixed move_speed_mult = Fixed::from_int(1);
    Fixed attack_speed_mult = Fixed::from_int(1);
    bool beh_root = false;
    bool beh_stun = false;
    bool beh_suppress_attack = false;
    bool beh_invulnerable = false;
    bool beh_uncommandable = false, beh_untargetable = false;
    bool beh_suppress_cloak = false, beh_benign = false, beh_passive = false;
    bool beh_disable_abils = false;
    bool beh_disable_production = false, beh_disable_spawner = false;
    bool beh_disable_morph = false, beh_disable_transport = false;
    std::vector<std::uint32_t> beh_disabled_abils{};
    Fixed beh_weapon_range{};
    Fixed beh_speed_cap{Fixed::from_int(-1)};
    std::vector<std::uint32_t> beh_disabled_weapons{};
    Fixed beh_decel_mult{Fixed::from_int(1)};
    Fixed beh_accel_mult{Fixed::from_int(1)};
    std::uint32_t beh_wpn_bonus_attr = 0;
    Fixed beh_wpn_bonus_amount{};
    bool beh_suppress_collision = false;
    bool beh_suppress_passenger = false;
    Fixed beh_life_armor{}, beh_shield_armor{};
    bool beh_lift = false;
    bool beh_control_transfer = false;
    Fixed beh_dmg_per_tick{};
    Fixed beh_rate_mult{Fixed::from_int(1)};
    Fixed beh_dmg_period{};
    Fixed radius{};
    Fixed period{};
    int32_t period_count = 0;
    std::uint8_t persist_affiliation = 0;
    bool persist_excludes_structure = false;
    int32_t spawn_proto = -1;
    int32_t spawn_count = 0;
    Fixed spawn_range{};
    bool spawn_snap_resource = false;
    bool spawn_neutral = false;
    bool spawn_blocks_ground = false;
    std::uint32_t spawn_unit_tag = 0;
    std::uint32_t fx_tag = 0;
    std::vector<std::pair<std::uint32_t, Fixed>> fx_spawn_tags{};
    int32_t mineral_cost = 0, gas_cost = 0;
    Fixed morph_heal_life_frac{}, morph_heal_shield_frac{};
    std::uint32_t behavior_tag = 0;
    std::uint32_t phase_cancel_tag = 0;
    EffectRef root = kNoEffect;
    Fixed prep_time{};
    Fixed range_slop{Fixed::from_int(1)};
    std::uint8_t uninterruptible = 0;
    EffectRef cancel_effect_prep = kNoEffect;
    Fixed cooldown{};
    std::uint32_t abil_tag = 0;
    std::uint32_t redirect_tag = 0;
    std::uint8_t redirect_order = 0xFF;
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

struct ValidatorOp {
    enum Kind : std::uint8_t { And, Or, Vital, Filters, UnitType, NoBehavior, OrderQueue, CompareRange, BehaviorCount, PlayerRequirement,
                               Compare, Mover, ConstFalse, Pathable, EnumArea } kind = And;
    Fixed area_radius;
    std::int32_t area_count = 1;
    std::uint8_t area_cmp = 5;
    bool area_extend = false, loc_source = false, exc_worker = false, exc_constructing = false;
    bool negate = false;
    bool on_caster = false;
    std::vector<std::uint16_t> kids;
    VitalGate vital;
    std::uint32_t req_attr = 0, exc_attr = 0;
    std::uint8_t req_plane = 0, exc_plane = 0;
    std::uint8_t exc_rel = 0;
    bool exc_invulnerable = false;
    std::string unit_type;
    std::uint32_t behavior = 0;
    std::uint16_t order_mask = 0;
    bool find = true;
    std::uint8_t cmp = 0;
    Fixed range;
    bool loc_a_target = false, loc_b_target = false;
    std::string requirement;
    std::int32_t count_value = 0;
    std::uint8_t field = 0;
    Fixed value;
    std::uint32_t abil_tag = 0;
    bool want_fly = false;
};

enum class HealVital : uint8_t { Life, Shields };

struct EffectNode {
    EffectKind kind = EffectKind::Damage;
    Fixed amount, duration, energy_fraction, radius, period;
    Fixed heal_drain_factor;
    HealVital heal_vital = HealVital::Life;
    Fixed shield_bonus;
    Fixed armor_reduction{Fixed::from_int(1)};
    std::uint8_t damage_kind = kDamageKindSpell;
    bool damage_hidden = false;
    Fixed move_speed_mult{Fixed::from_int(1)}, attack_speed_mult{Fixed::from_int(1)};
    bool beh_root = false, beh_stun = false;
    bool beh_suppress_attack = false, beh_invulnerable = false;
    bool beh_uncommandable = false, beh_untargetable = false;
    bool beh_suppress_cloak = false, beh_benign = false, beh_passive = false;
    bool beh_disable_abils = false;
    bool beh_disable_production = false, beh_disable_spawner = false;
    bool beh_disable_morph = false, beh_disable_transport = false;
    std::vector<std::uint32_t> beh_disabled_abils;
    Fixed beh_weapon_range;
    std::uint32_t redirect_tag = 0;
    std::uint8_t redirect_order = 0xFF;
    Fixed beh_speed_cap{Fixed::from_int(-1)};
    std::vector<std::uint32_t> beh_disabled_weapons;
    Fixed beh_decel_mult{Fixed::from_int(1)};
    Fixed beh_accel_mult{Fixed::from_int(1)};
    std::uint32_t beh_wpn_bonus_attr = 0;
    Fixed beh_wpn_bonus_amount{};
    bool beh_suppress_collision = false;
    bool beh_suppress_passenger = false;
    Fixed beh_life_armor{}, beh_shield_armor{};
    std::int32_t beh_max_stack = 1, beh_apply_count = 1;
    std::int32_t beh_max_stack_caster = 0;
    bool beh_refresh_stack = false;
    bool beh_lift = false;
    bool beh_control_transfer = false;
    Fixed beh_dmg_per_tick, beh_dmg_period;
    Fixed beh_rate_mult{Fixed::from_int(1)};
    Fixed beh_resp_amount{};
    std::uint8_t beh_resp_kinds = 15;
    bool persist_allies = false;
    std::uint8_t persist_affiliation = 0;
    bool persist_excludes_structure = false;
    bool persist_follow_source = false;
    bool persist_channeled = false;
    Fixed persist_initial_delay{};
    Fixed area_ring_r[3] = {Fixed(), Fixed(), Fixed()};
    Fixed area_ring_frac[3] = {Fixed(), Fixed(), Fixed()};
    std::uint8_t area_ring_count = 0;
    std::uint8_t area_affiliation = 0;
    bool damage_kill = false;
    std::uint8_t death_code = 0;
    std::string calldown_unit;
    std::int32_t calldown_count = 1;
    std::string wpn_cd_weapon;
    Fixed wpn_cd_fraction = Fixed::from_int(1);
    int32_t period_count = 0, spawn_proto = -1, spawn_count = 0;
    Fixed spawn_range{};
    bool spawn_snap_resource = false;
    bool spawn_neutral = false;
    bool spawn_blocks_ground = false;
    std::uint32_t spawn_unit_tag = 0;
    int32_t mineral_cost = 0, gas_cost = 0;
    Fixed morph_heal_life_frac, morph_heal_shield_frac;
    std::uint32_t wpn_bonus_attr = 0;
    Fixed wpn_bonus_amount;
    std::uint32_t behavior_tag = 0;
    bool beh_on_caster = false;
    std::uint32_t phase_cancel_tag = 0;
    std::int32_t remove_count = 0;
    std::uint32_t persist_src = 0, destroy_src = 0;
    std::int32_t destroy_count = 0;
    bool destroy_at_source = false, destroy_own_only = false;
    std::uint8_t cancel_kind = 0;
    bool issue_queued = false;
    bool cancel_active = true, cancel_queued = false;
    bool transfer_from_caster = false, transfer_to_caster = false, transfer_copy = false;
    Fixed vit_life, vit_life_frac, vit_shields, vit_shields_frac, vit_energy, vit_energy_frac;
    std::uint32_t cd_abil_tag = 0;
    std::uint8_t cd_op = 0;
    Fixed cd_time;
    Fixed missile_speed;
    Fixed missile_launch_z, missile_impact_z;
    std::uint8_t missile_launch_loc = 0, missile_impact_loc = 2;
    Fixed persist_expire_delay;
    std::vector<EffectRef> children;
    EffectRef initial = kNoEffect, periodic = kNoEffect, expire = kNoEffect, impact = kNoEffect;
    EffectRef resp_handled = kNoEffect;
    std::vector<std::uint16_t> periodic_validator_roots;
    bool beh_permanent = false;
    std::vector<ValidatorOp> beh_remove_ops;
    std::vector<std::uint16_t> beh_remove_roots;
    EffectRef final_effect = kNoEffect;
    std::uint8_t search_affiliation = kAffilNotOwn;
    std::int32_t search_max_count = 0;
    bool search_exclude_structure = false;
    bool search_exclude_target = false;
    std::vector<VitalGate> gates;
    std::vector<ValidatorOp> validators;
    std::vector<std::uint16_t> validator_roots;
    std::vector<ValidatorOp> switch_ops;
    std::vector<std::uint16_t> switch_case_roots;
    std::int16_t issue_order = -1;
    bool teleport_to_source = false;
    bool teleport_to_cast_point = false;
    Fixed dmg_frac_cur_life, dmg_frac_cur_shields, dmg_frac_cur_energy;
    bool area_at_source = false;
    std::uint32_t search_req_attr = 0, search_exc_attr = 0;
    std::uint8_t search_req_plane = 0, search_exc_plane = 0;
    bool persist_follow_target = false;
    Fixed salvage_mineral_frac, salvage_gas_frac;
    Fixed beh_food, beh_life_max;
    bool spawn_exact = false;
    bool missile_never_instant = false;
    std::uint32_t missile_tag = 0;
    std::uint32_t fx_tag = 0;
    std::uint32_t fx_beh_tag = 0;
    std::vector<std::pair<std::uint32_t, Fixed>> fx_spawn_tags{};
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
    Fixed wpn_armor_reduction{Fixed::from_int(1)};
    int32_t recall_index = -1;
    Fixed cast_x, cast_y;
    bool has_cast_point = false;
};

struct CastCost {
    Fixed energy{}, life{}, cooldown{};
    int32_t charge = -1;
    int32_t charge_use = 0;
    bool wait = false;
};

struct CastChargeSpec {
    std::string link;
    int32_t start = 0, max = 0;
    int start_ticks = 0, use_ticks = 0;
};

struct Order {
    OrderType type = OrderType::Idle;
    Fixed tx{}, ty{};
    uint32_t target = kNoTarget;
    uint32_t ability = kNoAbility;
    Fixed px{}, py{};
    bool exact_arrival = false;
    bool contact = false;
    bool walk_in = false;
    bool evict = false;
    bool pay_due = false;
    CastCost pay{};
};

struct BehaviorInstance {
    uint16_t kind = 0;
    uint32_t src = 0;
    uint32_t present_src = 0;
    uint32_t caster = 0xFFFFFFFFu;
    Fixed timer;
    bool permanent = false;
    Fixed speed_mult = Fixed::from_int(1);
    Fixed atk_speed_mult = Fixed::from_int(1);
    bool root = false;
    bool stun = false;
    bool suppress_attack = false;
    bool invulnerable = false;
    bool uncommandable = false;
    bool untargetable = false;
    bool suppress_cloak = false, benign = false, passive = false;
    bool disable_abils = false;
    bool disable_production = false, disable_spawner = false;
    bool disable_morph = false, disable_transport = false;
    std::vector<std::uint32_t> disabled_abils;
    Fixed weapon_range;
    Fixed speed_cap{Fixed::from_int(-1)};
    std::vector<std::uint32_t> disabled_weapons;
    Fixed decel_mult{Fixed::from_int(1)};
    Fixed accel_mult{Fixed::from_int(1)};
    bool suppress_collision = false;
    bool suppress_passenger = false;
    bool remove_when_firing = false;
    Fixed life_armor_bonus{}, shield_armor_bonus{};
    std::int32_t max_stack = 1;
    std::int32_t max_stack_caster = 0;
    bool refresh_stack = false;
    bool lift = false;
    Fixed dmg_per_tick;
    Fixed resp_amount;
    std::uint8_t resp_kinds = 15;
    Fixed dmg_period;
    Fixed dmg_timer;
    std::uint32_t wpn_bonus_attr = 0;
    Fixed wpn_bonus_amount;
    int32_t ctrl_orig_owner = -2;
    std::uint32_t ctrl_caster = kNoTarget;
    EffectRef fx_periodic = kNoEffect, fx_final = kNoEffect;
    EffectRef fx_resp_handled = kNoEffect;
    EffectRef fx_expire = kNoEffect;
    bool stripped = false;
    Fixed fx_px, fx_py;
    bool fx_has_point = false;
    std::vector<ValidatorOp> rv_ops;
    std::vector<std::uint16_t> rv_roots;
    Fixed fx_period{}, fx_timer{};
    std::uint32_t fx_caster = kNoTarget;
    int32_t fx_origin = -1;
    Fixed food, life_max;
};

struct PersistentInstance {
    Fixed x, y;
    Fixed radius;
    Fixed period;
    Fixed timer;
    Fixed amount;
    int32_t count = 0;
    int32_t owner = -1;
    bool allies = false;
    std::uint8_t affiliation = 0;
    bool excludes_structure = false;
    std::uint32_t follow = 0xFFFFFFFFu;
    BehaviorInstance on_hit;
    EffectRef periodic = kNoEffect;
    std::vector<std::pair<Fixed, Fixed>> offsets;
    Fixed face_x = Fixed::from_int(1), face_y;
    int32_t applied = 0;
    EffectRef expire = kNoEffect, final_effect = kNoEffect;
    std::uint32_t src = 0;
    std::uint64_t present_id = 0;
    std::uint32_t present_tag = 0;
    EffectRef initial_deferred = kNoEffect;
    Fixed expire_delay;
    bool expire_done = false;
    std::uint32_t source = kNoTarget, target = kNoTarget;
    bool channeled = false;
    std::vector<ValidatorOp> pval;
    std::vector<std::uint16_t> pval_roots;
};

struct PendingEffect {
    EffectRef impact = kNoEffect;
    EffectContext ctx;
    Fixed remaining;
    bool home = false;
    std::uint32_t missile_tag = 0;
    Fixed lx, ly, flight;
};

struct HealerInstance {
    std::uint32_t caster = kNoTarget;
    std::uint32_t target = kNoTarget;
    Fixed heal_rate;
    Fixed drain_factor;
    Fixed range;
    HealVital vital = HealVital::Life;
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
    std::uint32_t unit_tag = 0;
};

struct PresentationEvent {
    std::uint64_t seq = 0;
    std::uint64_t instance = 0;
    std::uint32_t fx_tag = 0;
    bool stop = false;
    bool abil = false;
    std::uint32_t source = 0xFFFFFFFFu, target = 0xFFFFFFFFu;
    Fixed src_x, src_y;
    Fixed tx, ty;
    std::int32_t player = -1;
};

inline uint32_t type_identity(const std::string& s) {
    uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= uint8_t(c);
        h *= 16777619u;
    }
    return h;
}

struct MorphStep {
    Fixed at;
    int32_t proto = -1;
    bool type = true;
    bool vitals = false;
};

struct MorphExtras {
    Fixed delay_min{}, delay_max{};
    std::vector<BehaviorInstance> behaviors_on{};
    std::vector<uint32_t> behaviors_off{};
};

struct Unit {
    uint32_t id = 0;
    int32_t player = 0;
    int32_t type_id = 0;
    std::string catalog_id;
    std::string wpn_id, wpn2_id;
    bool commandable = true;
    Fixed x, y;
    Fixed hp, hp_max, shields, armor;
    Fixed life_paid;
    Fixed shields_max;
    Fixed shield_regen_rate, shield_regen_delay;
    Fixed shield_since_hit, shield_last_total;
    Fixed shield_armor;
    Fixed armor_level;
    Fixed wpn_damage, wpn_period, wpn_range;
    Fixed wpn_min_range;
    Fixed wpn_damage_point;
    Fixed wpn_fire_arc{};
    Fixed wpn_armor_reduction{Fixed::from_int(1)};
    Fixed wpn_projectile_speed;
    struct WeaponMover { Fixed range_lte, speed, accel, max_speed; };
    WeaponMover wpn_movers[2];
    std::uint8_t wpn_mover_count = 0;
    WeaponMover wpn2_movers[2];
    std::uint8_t wpn2_mover_count = 0;
    Fixed wpn_backswing;
    Fixed wpn_random_delay_min{}, wpn_random_delay_max{};
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
    Fixed minimap_radius{Fixed::from_double(0.5)};
    Fixed vision_height;
    int32_t vision_player = -1;
    Fixed speed_mult_creep = Fixed::from_int(1);
    bool can_jump = false;
    Fixed jump_range;
    bool walks_cliffs = false;
    bool crushes_ff = false;
    int32_t creep_radius = 0;
    bool has_toggle_creep = false;
    bool creep_gen_on = false;
    int32_t toggle_creep_radius = 0;
    int32_t toggle_creep_period = 1;
    int32_t creep_source_idx = -1;
    int32_t creep_grow_period = 0;
    int32_t creep_birth_radius = 1;
    bool creep_starts_grown = false;
    std::vector<std::pair<int16_t, int16_t>> footprint_offsets;
    bool requires_creep = false;
    std::vector<std::pair<int16_t, int16_t>> creep_check_offsets;
    std::vector<std::pair<int16_t, int16_t>> near_resources_offsets;
    std::vector<std::pair<int16_t, int16_t>> near_resources_check_offsets;
    std::vector<std::pair<int16_t, int16_t>> nobuild_offsets;
    std::vector<std::pair<int16_t, int16_t>> nospawn_offsets;
    bool mover_requires_creep = false;
    bool mover_avoids_nospawn = false;
    bool train_morphs_self = false;
    std::uint8_t fog_visibility = 0;
    bool requires_vespene = false;
    Fixed atk_speed_mult = Fixed::from_int(1);
    Fixed damage_reduction;
    Fixed aura_dmg_reduction, aura_radius;
    Fixed aura_reduction;
    Fixed dmg_fraction = Fixed::from_int(1);
    bool deals_no_damage = false;
    Fixed dmg_clamp_max;
    Fixed dmg_minimum;
    Fixed barrier;
    Fixed barrier_max;
    Fixed barrier_cd;
    Fixed barrier_cd_max;
    Fixed cooldown;
    Fixed cd_carry{};
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
    bool resource_raw = false;
    bool gathers_gas = false;
    bool in_gas_structure = false;
    BuildVerb build_verb = BuildVerb::Committed;
    bool build_range_to_centre = false;
    bool is_worker = false;
    Fixed mine_time;
    Fixed acquire_radius;
    Fixed mine_cd;
    Fixed return_delay;
    Fixed return_cd;
    int gather_stall_ticks = 0;
    bool wall_replanned = false;
    int block_ticks = 0;
    uint32_t gather_target = kNoTarget;
    uint32_t home_depot = kNoTarget;

    int32_t train_slots = 0;
    int32_t queue_size = 0;
    int32_t queue_bonus_reactor = 0;
    int32_t addon = 0;
    bool addon_slot_host = false;
    uint32_t addon_body = 0xFFFFFFFFu;
    uint32_t addon_host = 0xFFFFFFFFu;
    int32_t addon_body_kind = 0;
    int32_t addon_building = 0;
    Fixed addon_build_left;

    bool has_rally = false;
    Fixed rally_x, rally_y;
    bool dual_rally = false, has_worker_rally = false, rally_exempt = false;
    Fixed worker_rally_x, worker_rally_y;
    uint32_t rally_abil_tag = 0;

    bool hold_fire = false;
    bool fire_while_moving = false;
    bool fire_slowing = false;

    Fixed chrono_timer;
    Fixed chrono_mult{Fixed::from_ratio(3, 2)};

    Fixed life_regen;
    Fixed life_regen_delay, life_since_hit, life_last_hp;
    Fixed time_life;
    Fixed time_life_max;
    Fixed burrow_regen;
    bool burns_down = false;
    Fixed burn_timer;
    Fixed burn_period = Fixed::parse("0.3332");
    Fixed burn_amount = Fixed::from_int(1);
    bool flees_when_attacked = false;
    uint32_t flee_from = kNoTarget;
    Fixed phase_timer;
    uint32_t phase_shade = kNoTarget;
    uint32_t phase_caster_tag = 0;
    uint32_t phase_cancel_tag = 0;

    Fixed windup_timer;
    Fixed windup_total;
    uint32_t windup_target = kNoTarget;
    Fixed windup_dmg;
    uint32_t windup_battr = 0;
    Fixed windup_bamt;
    std::uint8_t windup_slot = 0;
    Fixed wpn_range_slop{Fixed::from_int(-1)}, wpn2_range_slop{Fixed::from_int(-1)};
    std::uint32_t wpn_tag = 0, wpn2_tag = 0;
    Fixed windup_reach;
    Fixed windup_ps;
    Fixed windup_ar{Fixed::from_int(1)};
    int windup_hits = 1;

    Fixed crush_damage;
    Fixed crush_radius;

    Fixed buff_timer;
    Fixed buff_damage;

    Fixed morph_timer;
    Fixed morph_total;
    Fixed lift_lock{}, lift_rise_delay{}, lift_rise{}, lift_t{};
    uint32_t morph_finish_beh = 0;
    int32_t morph_proto = -1;
    Fixed burrow_timer{}, burrow_flag_in{}, burrow_total{};
    bool burrow_to = false;
    Fixed depot_timer{}, depot_flag_in{};
    bool depot_to = false;

    Fixed salvage_timer;
    Fixed salvage_total;
    int32_t salvage_minerals = 0;
    int32_t salvage_gas = 0;

    int32_t spawner_proto = -1;
    int32_t spawner_cap = 0;
    int32_t spawner_hard_cap = 0;
    Fixed spawner_period;
    Fixed spawner_timer;
    uint32_t spawn_parent = kNoTarget;
    int32_t spawn_slot = -1;

    Fixed inject_timer;
    int32_t inject_proto = -1;
    int32_t inject_count = 0;
    bool larva_injected = false;

    Fixed warp_cd;

    int32_t cargo_space = 0;
    Fixed load_range{};
    int32_t cargo_size = 0;
    Fixed unload_period, unload_initial_delay;
    int32_t max_cargo_size = 0;
    Fixed unload_timer;
    int32_t unload_ring = -1;
    bool cargo_death = false;
    uint32_t transport_id = kNoTarget;
    bool player_hold = false;
    Fixed repair_time{};
    int32_t cost_minerals = 0, cost_gas = 0;
    Fixed repair_life{};
    int32_t repair_paid_m = 0, repair_paid_g = 0;
    bool attack_relay = false;
    Fixed relay_range_bonus;

    Fixed power_radius;
    int8_t power_level = 0;
    int8_t power_boost_level = 0;
    int8_t power_boost_min = 0;
    int8_t enabler_level = 0;
    Fixed enabler_radius;
    bool power_reaches_higher = true;
    bool needs_power = false;
    bool powered = false;
    bool power_managed = false;
    Fixed morph_heal_life_frac, morph_heal_shield_frac;
    bool morph_pair = false;
    bool morph_staged = false;
    std::vector<MorphStep> morph_steps;
    Fixed morph_delay{};
    std::vector<uint32_t> morph_beh_off{};
    uint32_t upgrades_folded = 0;

    Fixed zone_x, zone_y, zone_radius;

    bool constructing = false;
    bool armor_disabled_constructing = false;
    bool turn_before_move = false;
    bool town_alert = false;
    std::int8_t load_require_race = -1;
    std::vector<std::string> load_require_types;
    std::vector<std::string> load_exclude_types;
    std::vector<std::uint32_t> load_block_behaviors;
    std::int8_t race_index = -1;
    int32_t build_charges = -1;
    bool charge_augment = false;
    Fixed charge_range;
    Fixed charge_min;
    Fixed charge_speed_mult;
    Fixed charge_duration;
    Fixed charge_cd;
    std::string charge_requirement;
    Fixed offcreep_amount;
    Fixed offcreep_period;
    Fixed offcreep_timer;
    Fixed death_off_creep = Fixed::from_int(-1);
    Fixed wander_range{};
    Fixed wander_delay{};
    Fixed wander_timer{};
    Fixed wander_ox{}, wander_oy{};
    bool wander_leash = false;
    bool wander_home_set = false;
    bool hall_trained = false;
    Fixed build_cd;
    Fixed build_cd_use;
    Fixed build_range;
    Fixed build_time;
    Fixed build_progress;
    Fixed build_elapsed;
    Fixed build_hp0;
    bool warp_forming = false;
    bool needs_builder = false;
    uint32_t builder = kNoTarget;
    uint32_t construct_target = kNoTarget;

    Fixed energy, energy_max, energy_regen;

    std::vector<BehaviorInstance> behaviors;

    bool cloaked = false;
    bool detector = false;
    bool prevent_defeat = false;
    bool unselectable = false;
    bool invulnerable_flag = false;
    bool magazine_ammo = false;
    Fixed detect_range;
    Fixed cloak_drain;
    Fixed cloak_field_radius;
    bool field_cloaked = false;
    bool cloak_field_self = true;
    bool cloak_suppressed() const {
        for (const BehaviorInstance& b : behaviors)
            if (b.suppress_cloak && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    bool cloaked_effective() const { return (cloaked || field_cloaked) && !cloak_suppressed(); }

    bool loaded = false;
    bool burrowed = false;
    bool can_burrow_move = false;
    Fixed burrow_speed{};
    bool can_burrow_attack = false;
    struct BurrowWeapon {
        Fixed damage, period, range, min_range, damage_point, projectile_speed, backswing, bonus_amount, acquire;
        Fixed fire_arc{};
        Fixed random_delay_min{}, random_delay_max{};
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
        std::swap(wpn_fire_arc, other.fire_arc);
        std::swap(wpn_random_delay_min, other.random_delay_min);
        std::swap(wpn_random_delay_max, other.random_delay_max);
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
    bool drop_off_ground = true;
    bool drop_off_air = false;
    Fixed speed_air;
    bool lowered = false;
    Fixed land_time;
    Fixed land_desc_delay{}, land_desc{};
    bool land_pending = false;
    Fixed land_x, land_y;
    Fixed land_timer;
    int32_t land_addon = 0;
    Fixed land_addon_time;
    int32_t land_addon_minerals = 0, land_addon_gas = 0;
    uint32_t attributes = 0;
    uint32_t wpn_target_filter = 3;
    uint8_t target_sort = 0;
    uint8_t acquire_sort = kAcquireSortDefault;
    bool wpn_acquire_no_structure = false;
    bool wpn2_acquire_no_structure = false;
    Fixed wpn_teleport_reset = Fixed::from_int(-1);
    Fixed wpn2_teleport_reset = Fixed::from_int(-1);
    int32_t target_priority = 0;

    Fixed wpn2_damage, wpn2_period, wpn2_range;
    std::uint8_t wpn_kind = 4, wpn2_kind = 4;
    std::uint8_t wpn_death = 0, wpn2_death = 0;
    std::uint8_t death_code = 0;
    Fixed wpn2_min_range;
    Fixed wpn2_close_range;
    uint32_t wpn2_target_filter = 3;
    uint32_t wpn2_bonus_attr = 0;
    Fixed wpn2_bonus_amount;
    Fixed wpn2_damage_point;
    Fixed wpn2_fire_arc{};
    Fixed wpn2_backswing;
    Fixed wpn2_random_delay_min{}, wpn2_random_delay_max{};
    int32_t wpn2_hits = 1;
    Fixed wpn2_armor_reduction{Fixed::from_int(1)};
    Fixed wpn2_projectile_speed;

    Fixed splash_inner, splash_mid, splash_outer;
    Fixed no_splash_radius;
    Fixed splash_inner_frac = Fixed::from_int(1);
    Fixed splash_mid_frac = Fixed::from_ratio(1, 2);
    Fixed splash_outer_frac = Fixed::from_ratio(1, 4);
    std::uint8_t splash_affiliation = 8 | 16;
    std::uint8_t splash_planes = 1 | 2;
    bool splash_excludes_structure = false;
    bool splash_offset_by_radius = false;

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
    Fixed turret_rate;
    Fixed turret_facing;
    bool turret_set = false;
    Fixed accel, decel;
    Fixed lateral_accel;
    Fixed cur_speed;
    bool moving_now = false;
    bool from_rest = false;
    bool moved_this_tick = false;
    Fixed stall_gx, stall_gy, stall_dist;
    std::uint16_t stall_ticks = 0;
    bool path_partial = false;
    Fixed move_target_x, move_target_y;
    Fixed loco_moved = Fixed();
    Fixed loco_x0 = Fixed(), loco_y0 = Fixed();
    bool arrived = false;
    Fixed arrive_x = Fixed(), arrive_y = Fixed();
    bool pushed_this_tick = false;
    std::uint8_t push_release = 0;
    Fixed push_dir_x, push_dir_y;
    bool has_anchor = false;
    Fixed anchor_x, anchor_y;
    std::uint8_t air_expand = 0;
    std::int32_t air_onset_tick = 0;
    bool ever_ordered = false;
    std::uint32_t cast_count = 0;
    Fixed cast_prep;
    int32_t cast_prep_ability = -1;
    Fixed facing;
    Fixed guard_x{}, guard_y{};
    bool guarding = false;
    bool guard_set = false;
    int32_t reveal_to = -1;
    Fixed reveal_timer{};
    Fixed help_cd{};

    int32_t vet_index = -1;
    int64_t xp = 0;
    int32_t level = 0;
    int64_t kill_xp = 0;
    int32_t kills = 0;
    bool heroic = false;
    bool hero_ui = false;

    std::vector<std::pair<int32_t, Fixed>> ability_cd;

    std::vector<Waypoint> path;
    size_t path_next = 0;
    int32_t path_gx = INT32_MIN;
    int32_t path_gy = INT32_MIN;
    uint32_t path_rev = 0;
};

inline bool power_disabled(const Unit& u) {
    return u.needs_power && u.power_managed && !u.powered;
}

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
    int last_combat_tick = 0;
    int32_t minerals = 50;
    int32_t gas = 0;
    int32_t team = -1;
    bool defeated = false;
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
    int32_t player = 0;
};

struct TrainSink {
    virtual ~TrainSink() = default;
    virtual void on_train_finish(int tick, uint32_t producer, uint32_t egg,
                                 const std::string& produced) = 0;
    virtual void on_train_cancel(int tick, uint32_t producer, uint32_t egg,
                                 const std::string& produced) = 0;
};

struct OrderSink {
    virtual ~OrderSink() = default;
    virtual void on_order(int tick, uint32_t id, const Order& o, bool queued) = 0;
    virtual void on_stop(int tick, uint32_t id) = 0;
    virtual void on_command(int tick, uint32_t id, const Command& c) = 0;
};

class World {
public:
    struct ShotView {
        std::uint32_t attacker = 0, target = 0;
        Fixed x, y;
        std::uint8_t slot = 0;
        Fixed progress;
        Fixed launch_x, launch_y, speed;
    };
    std::vector<ShotView> shots_in_flight() const;
    struct HitEvent {
        std::uint32_t attacker = 0, target = 0;
        std::uint8_t slot = 0;
        Fixed x, y;
        Fixed amount;
        bool on_shields = false;
    };
    std::vector<HitEvent> take_hit_events() {
        std::vector<HitEvent> out;
        out.swap(hit_log_);
        return out;
    }
    struct LaunchEvent {
        std::uint32_t attacker = 0, target = 0;
        std::uint8_t slot = 0;
    };
    std::vector<LaunchEvent> take_launch_events() {
        std::vector<LaunchEvent> out;
        out.swap(launch_log_);
        return out;
    }
    struct ResourceEmptyEvent {
        std::int32_t player = -1;
        std::uint32_t node = 0;
        Fixed x, y;
        bool is_gas = false;
    };
    std::vector<ResourceEmptyEvent> take_resource_empty_events() {
        std::vector<ResourceEmptyEvent> out;
        out.swap(resource_empty_log_);
        return out;
    }
    static constexpr int kTicksPerSecond = 16;
    static Fixed dt() { return Fixed::from_ratio(1, kTicksPerSecond); }

    explicit World(uint64_t seed = 1) : rng_(seed), seed_(seed) {}
    uint64_t seed() const { return seed_; }

    uint32_t add_unit(Unit u) {
        u.id = uint32_t(units_.size());
        if (unit_refold_) unit_refold_(u);
        if (u.time_life.raw() > 0 && u.time_life_max.raw() <= 0) u.time_life_max = u.time_life;
        if (u.prevent_defeat) defeat_rule_authored_ = true;
        last_progress_tick_ = tick_;
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
                c.queue_size = a.queue_size;
                c.minerals = a.minerals;
                c.gas = a.gas;
                c.auto_build = a.auto_build && a.auto_build_on;
                c.show_status = a.show_status;
                carriers_.push_back(std::move(c));
            }
        }
        units_.push_back(u);
        if (u.creep_radius > 0) {
            const int period = u.creep_grow_period > 0 ? u.creep_grow_period : 1;
            auto mit = creep_masks_.find(u.catalog_id);
            const int start = (tick_ == 0 && u.creep_starts_grown) ? INT32_MAX : u.creep_birth_radius;
            units_.back().creep_source_idx =
                (mit != creep_masks_.end() && !mit->second.empty())
                    ? creep_.add_source(u.x, u.y, u.creep_radius, period, mit->second, start)
                    : creep_.add_source(u.x, u.y, u.creep_radius, period, {}, start);
        }
        return u.id;
    }

    void set_order_sink(OrderSink* s) { order_sink_ = s; }
    void set_train_sink(TrainSink* s) { train_sink_ = s; }
    OrderSink* order_sink() const { return order_sink_; }

    static bool is_movement_order(OrderType t) {
        return t == OrderType::Move || t == OrderType::AttackMove || t == OrderType::Attack ||
               t == OrderType::HoldPosition || t == OrderType::Patrol;
    }

    bool attack_suppressed(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.suppress_attack) return true;
        return false;
    }
    bool validator_passes(const std::vector<ValidatorOp>& ops, std::uint16_t i, const EffectContext& ctx,
                          bool no_unit_passes = true) const;
    static Fixed behavior_armor(const Unit& u, bool shield) {
        Fixed a;
        for (const BehaviorInstance& b : u.behaviors) a = a + (shield ? b.shield_armor_bonus : b.life_armor_bonus);
        return a;
    }
    bool abils_disabled(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.disable_abils && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    static bool boarding_suppressed(const Unit& u) {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.suppress_passenger && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    static bool collision_suppressed(const Unit& u) {
        if (u.in_gas_structure) return true;
        if (u.loaded && default_cargo_hidden()) return true;
        for (const BehaviorInstance& b : u.behaviors)
            if (b.suppress_collision && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    static Fixed behavior_decel_mult(const Unit& u) {
        Fixed m = Fixed::from_int(1);
        for (const BehaviorInstance& b : u.behaviors)
            if (b.permanent || b.timer.raw() > 0) m = m * b.decel_mult;
        return m;
    }
    static Fixed behavior_accel_mult(const Unit& u) {
        Fixed m = Fixed::from_int(1);
        for (const BehaviorInstance& b : u.behaviors)
            if (b.permanent || b.timer.raw() > 0) m = m * b.accel_mult;
        return m;
    }
    struct BehaviorUpgradeMod {
        Fixed move_speed_mult{Fixed::from_int(-1)};
        Fixed accel_mult{Fixed::from_int(-1)};
    };
    void put_behavior_upgrade_mod(int32_t player, std::uint32_t beh_tag, const BehaviorUpgradeMod& m) {
        behavior_upgrade_mods_[{player, beh_tag}] = m;
    }
    const BehaviorUpgradeMod* behavior_upgrade_mod(int32_t player, std::uint32_t beh_tag) const {
        auto it = behavior_upgrade_mods_.find({player, beh_tag});
        return it == behavior_upgrade_mods_.end() ? nullptr : &it->second;
    }
    static bool weapon_disabled(const Unit& u, std::uint32_t slot_tag) {
        if (slot_tag == 0) return false;
        for (const BehaviorInstance& b : u.behaviors) {
            if (b.disabled_weapons.empty() || !(b.permanent || b.timer.raw() > 0)) continue;
            for (std::uint32_t t : b.disabled_weapons)
                if (t == slot_tag) return true;
        }
        return false;
    }
    static Fixed behavior_speed_cap(const Unit& u) {
        Fixed best = Fixed::from_int(-1);
        for (const BehaviorInstance& b : u.behaviors) {
            if (b.speed_cap.raw() < 0 || !(b.permanent || b.timer.raw() > 0)) continue;
            if (best.raw() < 0 || b.speed_cap < best) best = b.speed_cap;
        }
        return best;
    }
    Fixed weapon_range_bonus(const Unit& u) const {
        Fixed a;
        for (const BehaviorInstance& b : u.behaviors)
            if (b.permanent || b.timer.raw() > 0) a = a + b.weapon_range;
        return a;
    }
    bool production_disabled(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.disable_production && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    bool spawner_disabled(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.disable_spawner && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    bool morph_disabled(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.disable_morph && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    bool transport_disabled(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.disable_transport && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    bool abil_link_disabled(const Unit& u, std::uint32_t abil_tag) const {
        if (abil_tag == 0) return false;
        for (const BehaviorInstance& b : u.behaviors) {
            if (b.disabled_abils.empty() || !(b.permanent || b.timer.raw() > 0)) continue;
            for (std::uint32_t t : b.disabled_abils)
                if (t == abil_tag) return true;
        }
        return false;
    }
    bool uncommandable(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.uncommandable) return true;
        return false;
    }
    bool untargetable(const Unit& u) const {
        if (u.in_gas_structure) return true;
        for (const BehaviorInstance& b : u.behaviors)
            if (b.untargetable) return true;
        return false;
    }
    bool benign(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.benign && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    bool passive(const Unit& u) const {
        for (const BehaviorInstance& b : u.behaviors)
            if (b.passive && (b.permanent || b.timer.raw() > 0)) return true;
        return false;
    }
    bool is_invulnerable(const Unit& u) const {
        if (u.invulnerable_flag) return true;
        for (const BehaviorInstance& b : u.behaviors)
            if (b.invulnerable) return true;
        return false;
    }

    bool commandable(uint32_t id) const { return id < units_.size() && units_[id].commandable; }
    bool revealed_to(int32_t player, uint32_t id) const {
        return id < units_.size() && detected_by(player, units_[id]);
    }

    bool creep_placement_ok(const Unit& proto, Fixed x, Fixed y) const {
        if (!proto.requires_creep) {
            if (proto.creep_check_offsets.empty()) return true;
            int fx = 0, fy = 0;
            creep_.grid().to_cell(x, y, fx, fy);
            for (const std::pair<int16_t, int16_t>& c : proto.creep_check_offsets)
                if (creep_.grid().is_creep_cell(fx + c.first, fy + c.second)) return false;
            return true;
        }
        if (proto.creep_check_offsets.empty()) return creep_.is_creep(x, y);
        int ox = 0, oy = 0;
        creep_.grid().to_cell(x, y, ox, oy);
        for (const std::pair<int16_t, int16_t>& c : proto.creep_check_offsets)
            if (!creep_.grid().is_creep_cell(ox + c.first, oy + c.second)) return false;
        return true;
    }

    bool resource_halo_ok(const Unit& proto, Fixed x, Fixed y) const {
        if (proto.near_resources_check_offsets.empty()) return true;
        const int ox = x.floor(), oy = y.floor();
        for (const Unit& n : units_) {
            if (!n.alive || n.near_resources_offsets.empty()) continue;
            const int nx = n.x.floor(), ny = n.y.floor();
            for (const std::pair<int16_t, int16_t>& c : proto.near_resources_check_offsets) {
                const int cx = ox + c.first - nx, cy = oy + c.second - ny;
                for (const std::pair<int16_t, int16_t>& h : n.near_resources_offsets)
                    if (h.first == cx && h.second == cy) return false;
            }
        }
        return true;
    }

    static bool harvesting(const Unit& u) {
        return u.is_worker && !u.orders.empty() && u.orders.front().type == OrderType::Gather;
    }

    CmdError last_error() const { return last_error_; }
    int32_t last_error_player() const { return last_error_player_; }
    void clear_last_error() { last_error_ = CmdError::None; last_error_player_ = -1; }
    void report_error(int32_t player, CmdError e) { refuse(player, e); }

    bool splash_hits(const Unit& attacker, const Unit& victim) const {
        std::uint8_t rel = 0;
        if (victim.id == attacker.id) rel = 1;
        else if (victim.player == attacker.player) rel = 2;
        else if (victim.player < 0) rel = 16;
        else if (are_allied(victim.player, attacker.player)) rel = 4;
        else rel = 8;
        if ((attacker.splash_affiliation & rel) == 0) return false;
        if ((attacker.splash_planes & (victim.flying ? 2 : 1)) == 0) return false;
        if (attacker.splash_excludes_structure && (victim.attributes & kAttrStructure)) return false;
        return true;
    }

    bool issue(uint32_t id, const Order& o, bool queued = false, bool player_order = true) {
        if (id >= units_.size()) return false;
        if (player_order && units_[id].lift_lock.raw() > 0) return false;
        if (player_order && units_[id].burrow_timer.raw() > 0) return false;
        if (!in_step_ && !units_[id].commandable && !units_[id].flying &&
            is_movement_order(o.type))
            return false;
        if (!in_step_ && uncommandable(units_[id])) return false;
        if (!in_step_ && o.type == OrderType::Cast && abils_disabled(units_[id])) return false;
        if (order_sink_ && !in_step_) order_sink_->on_order(tick_, id, o, queued);
        detach_builder(id);
        Unit& u = units_[id];
        if (!queued && player_order && o.type == OrderType::Move && u.windup_timer.raw() > 0) {
            const Fixed elapsed = u.windup_total - u.windup_timer;
            if (u.windup_total.raw() <= 0 || elapsed < Fixed::from_double(0.0893)) cancel_windup(u);
        }
        const bool prep_protected =
            !queued && player_order && u.cast_prep.raw() > 0 && cast_prep_uninterruptible(u);
        if (!queued && player_order && u.cast_prep.raw() > 0 && !cast_prep_uninterruptible(u))
            cancel_cast_prep(u);
        if (!queued && !prep_protected) u.orders.clear();
        u.orders.push_back(o);
        if (player_order) u.ever_ordered = true;
        return true;
    }
    void stop(uint32_t id) {
        if (id >= units_.size()) return;
        if (!in_step_ && !units_[id].commandable && !units_[id].flying) return;
        if (!in_step_ && uncommandable(units_[id])) return;
        if (order_sink_ && !in_step_) order_sink_->on_stop(tick_, id);
        detach_builder(id);
        cancel_windup(units_[id]);
        units_[id].orders.clear();
    }

    bool cast_prep_uninterruptible(const Unit& u) const {
        if (u.cast_prep.raw() <= 0) return false;
        const int32_t slot = u.cast_prep_ability;
        if (slot < 0 || std::size_t(slot) >= abilities_.size()) return false;
        return (abilities_[std::size_t(slot)].uninterruptible & 1u) != 0;
    }
    bool is_channelling(uint32_t id) const;
    bool cancel_channel(uint32_t id);
    bool cancel_cast(uint32_t id) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (u.cast_prep.raw() <= 0) return false;
        const int32_t slot = u.cast_prep_ability;
        cancel_cast_prep(u);
        if (!u.orders.empty() && u.orders.front().type == OrderType::Cast && int32_t(u.orders.front().ability) == slot)
            u.orders.erase(u.orders.begin());
        return true;
    }
    void cancel_cast_prep(Unit& u) {
        if (u.cast_prep.raw() <= 0) return;
        const int32_t slot = u.cast_prep_ability;
        u.cast_prep = Fixed();
        u.cast_prep_ability = -1;
        if (slot < 0 || std::size_t(slot) >= abilities_.size()) return;
        const EffectRef cancel = abilities_[std::size_t(slot)].cancel_effect_prep;
        if (cancel < 0) return;
        EffectContext ctx;
        ctx.source = ctx.target = u.id;
        ctx.origin = u.player;
        ctx.sx = ctx.tx = u.x;
        ctx.sy = ctx.ty = u.y;
        execute_effect(cancel, ctx);
    }
    static void cancel_windup(Unit& u) {
        if (u.windup_timer.raw() <= 0) return;
        u.windup_timer = Fixed();
        u.windup_target = kNoTarget;
        u.windup_ps = Fixed();
        u.windup_reach = Fixed();
        u.windup_total = Fixed();
        u.cooldown = Fixed();
        u.backswing_timer = Fixed();
    }

    void detach_builder(uint32_t id) {
        if (id >= units_.size()) return;
        const uint32_t bld = units_[id].construct_target;
        if (bld == kNoTarget) return;
        units_[id].construct_target = kNoTarget;
        if (bld < units_.size() && units_[bld].builder == id) units_[bld].builder = kNoTarget;
    }

    bool rally_disabled(uint32_t id) const {
        return id < units_.size() && abil_link_disabled(units_[id], units_[id].rally_abil_tag);
    }
    void set_rally(uint32_t id, Fixed x, Fixed y) {
        if (id >= units_.size()) return;
        if (abil_link_disabled(units_[id], units_[id].rally_abil_tag)) return;
        if (reporting()) {
            Command c;
            c.kind = CommandKind::RallySet;
            c.x = x;
            c.y = y;
            report_command(id, c);
        }
        if (units_[id].dual_rally) {
            for (const Unit& o : units_) {
                if (!o.alive || o.resource_left <= 0) continue;
                const Fixed dx = o.x - x, dy = o.y - y;
                if (dx * dx + dy * dy <= o.radius * o.radius) {
                    units_[id].has_worker_rally = true;
                    units_[id].worker_rally_x = o.x;
                    units_[id].worker_rally_y = o.y;
                    return;
                }
            }
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
        units_[id].has_worker_rally = false;
    }

    Order smart_order(uint32_t actor, uint32_t target, Fixed tx, Fixed ty) const;

    void issue_smart(uint32_t actor, uint32_t target, Fixed tx, Fixed ty,
                     bool queued = false) {
        issue(actor, smart_order(actor, target, tx, ty), queued);
    }

    bool teleport_unit(uint32_t id, Fixed tx, Fixed ty);

    void apply_spell_damage(uint32_t target, Fixed dmg, uint32_t caster,
                            Fixed armor_reduction = Fixed::from_int(1),
                            bool reveals_attacker = true,
                            std::uint8_t damage_kind = kDamageKindSpell,
                            std::uint8_t death_code = 0);

    void apply_weapon_hit(std::uint32_t target, Fixed base, std::uint32_t bonus_attr,
                          Fixed bonus_amount, std::uint32_t attacker,
                          Fixed armor_reduction = Fixed::from_int(1),
                          bool reveals_attacker = true);

    void record_flee_trigger(Unit& target, std::uint32_t attacker, Fixed vitals_before,
                             bool reveals_attacker = true);

    void apply_weapon_fire(std::uint32_t attacker, std::uint32_t target, Fixed w_dmg,
                           std::uint32_t w_battr, Fixed w_bamt, int hits = 1,
                           Fixed w_ar = Fixed::from_int(1), std::uint8_t w_slot = 0);
    void step_windup(Fixed dt);
    Fixed reload_after_shot(const Unit& u, Fixed period, Fixed rd_min, Fixed rd_max);
    void step_landing(Fixed dt);
    void step_projectiles(Fixed dt);
    void step_effect_missiles(Fixed dt);
    void step_point_defense(Fixed dt);
    void queue_projectile(std::uint32_t attacker, Fixed px, Fixed py, std::uint32_t target, Fixed speed,
                          Fixed dmg, std::uint32_t battr, Fixed bamt, Fixed ar = Fixed::from_int(1),
                          int hits = 1, std::uint8_t slot = 0);

    Fixed heal_unit(uint32_t id, Fixed amount);

    Unit& unit(uint32_t id) { return units_[id]; }

    void step();
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) step();
    }

    void begin_research(int32_t player, const std::string& id, Fixed delay, uint32_t producer = kNoTarget,
                        int32_t minerals = 0, int32_t gas = 0);
    void grant_upgrade(int32_t player, const std::string& id);
    const std::vector<std::string>& researched(int32_t player) const;
    bool has_upgrade(int32_t player, const std::string& id) const;
    static constexpr int32_t kChargeCooldownSlot = -1001;
    static constexpr uint16_t kChargeBehaviorKind = 0xC0DE;
    bool is_charging(uint32_t id) const {
        if (id >= units_.size()) return false;
        for (const BehaviorInstance& b : units_[id].behaviors)
            if (b.kind == kChargeBehaviorKind && b.timer.raw() > 0) return true;
        return false;
    }
    bool research_pending(int32_t player, const std::string& id) const;
    int research_in_progress() const { return int(research_pending_.size()); }
    std::vector<std::pair<int32_t, std::string>> take_completed_research();

    void note_combat(int32_t p) {
        if (p >= 0) player(p).last_combat_tick = tick_;
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

    bool cast(uint32_t caster, int32_t ability, uint32_t target, bool queued = false, const CastCost* pay = nullptr) {
        if (ability >= 0 && ability_cooldown_left(caster, ability).raw() > 0) return false;
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
        if (pay) {
            o.pay = *pay;
            o.pay_due = true;
        }
        return issue(caster, o, queued);
    }

    bool cast(uint32_t caster, int32_t ability, Fixed tx, Fixed ty, bool queued = false, const CastCost* pay = nullptr) {
        if (ability >= 0 && ability_cooldown_left(caster, ability).raw() > 0) return false;
        Order o;
        o.type = OrderType::Cast;
        o.ability = uint32_t(ability);
        o.target = kNoTarget;
        o.tx = tx;
        o.ty = ty;
        if (pay) {
            o.pay = *pay;
            o.pay_due = true;
        }
        return issue(caster, o, queued);
    }

    int32_t cast_charge_spec(const std::string& link, int32_t start, int32_t max, int start_ticks, int use_ticks) {
        for (std::size_t i = 0; i < cast_charges_.size(); ++i) {
            const CastChargeSpec& c = cast_charges_[i];
            if (c.link == link && c.start == start && c.max == max && c.start_ticks == start_ticks &&
                c.use_ticks == use_ticks)
                return int32_t(i);
        }
        cast_charges_.push_back(CastChargeSpec{link, start, max, start_ticks, use_ticks});
        return int32_t(cast_charges_.size()) - 1;
    }
    bool cast_cost_payable(const Unit& u, const Order& o) const;
    bool pay_cast_cost(Unit& u, Order& o);

    int32_t add_veterancy(VeterancyDef def);
    Fixed supply_used(int32_t p) const;
    Fixed supply_max(int32_t p) const;
    bool supply_room(int32_t p, Fixed uses, int32_t n = 1) const {
        if (uses.raw() <= 0) return true;
        const Fixed cap = supply_max(p);
        return cap.raw() <= 0 || supply_used(p) + uses * Fixed::from_int(n) <= cap;
    }
    void set_supply_ceiling(int32_t p, Fixed ceiling) {
        if (p < 0) return;
        if (supply_ceiling_.size() <= std::size_t(p)) supply_ceiling_.resize(std::size_t(p) + 1, Fixed::from_int(200));
        supply_ceiling_[std::size_t(p)] = ceiling;
    }

    static bool is_neutral(int32_t p) { return p < 0; }
    int32_t team_of(int32_t p) const;
    bool are_allied(int32_t a, int32_t b) const;

    bool queue_train(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                     int32_t gas_cost, Fixed build_time, int32_t count = 1);
    bool queue_train_at(uint32_t producer, const Unit& proto, int32_t mineral_cost,
                        int32_t gas_cost, Fixed build_time, Fixed x, Fixed y, int32_t count = 1);
    size_t pending_count() const { return pending_.size(); }
    size_t pending_count(int32_t player) const {
        size_t n = 0;
        for (const Pending& p : pending_)
            if (p.producer < units_.size() && units_[p.producer].player == player) ++n;
        return n;
    }
    bool pending_progress(uint32_t producer, Fixed& frac) const {
        for (int i = int(pending_.size()) - 1; i >= 0; --i) {
            const Pending& p = pending_[size_t(i)];
            if (p.producer != producer) continue;
            if (p.total.raw() <= 0) return false;
            Fixed f = Fixed::from_int(1) - p.timer / p.total;
            if (f.raw() < 0) f = Fixed();
            if (f > Fixed::from_int(1)) f = Fixed::from_int(1);
            frac = f;
            return true;
        }
        return false;
    }
    void pending_research_by_type(int32_t player, std::unordered_map<std::string, int>& out) const {
        for (const PendingResearch& p : research_pending_)
            if (p.player == player && !p.id.empty()) ++out[p.id];
    }
    void pending_by_type(int32_t player, std::unordered_map<std::string, int>& out) const {
        for (const Pending& p : pending_) {
            if (p.producer >= units_.size() || units_[p.producer].player != player) continue;
            if (p.proto.catalog_id.empty()) continue;
            out[p.proto.catalog_id] += p.count > 1 ? p.count : 1;
        }
    }
    void pending_at_producer(uint32_t producer, std::unordered_map<std::string, int>& queued,
                             std::unordered_map<std::string, int>& held) const {
        for (const Pending& p : pending_)
            if (p.producer == producer && !p.proto.catalog_id.empty()) queued[p.proto.catalog_id] += p.count > 1 ? p.count : 1;
        for (const Unit& u : units_)
            if (u.alive && u.magazine_ammo && u.loaded && u.transport_id == producer && !u.catalog_id.empty())
                ++held[u.catalog_id];
    }
    bool research_progress(int32_t player, Fixed& frac, std::string& id) const {
        for (const PendingResearch& p : research_pending_) {
            if (p.player != player) continue;
            if (p.total.raw() <= 0) return false;
            Fixed f = Fixed::from_int(1) - p.timer / p.total;
            if (f.raw() < 0) f = Fixed();
            if (f > Fixed::from_int(1)) f = Fixed::from_int(1);
            frac = f;
            id = p.id;
            return true;
        }
        return false;
    }

    struct ResearchSlotView {
        std::string id;
        float progress = 0.0f;
    };
    std::vector<ResearchSlotView> research_at(uint32_t producer) const {
        std::vector<ResearchSlotView> out;
        if (producer == kNoTarget) return out;
        for (const PendingResearch& p : research_pending_) {
            if (p.producer != producer) continue;
            ResearchSlotView v;
            v.id = p.id;
            if (p.total.raw() > 0) {
                const double f = 1.0 - p.timer.to_double() / p.total.to_double();
                v.progress = float(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
            }
            out.push_back(std::move(v));
        }
        return out;
    }

    size_t pending_for(uint32_t producer) const {
        size_t n = 0;
        for (const Pending& p : pending_)
            if (p.producer == producer) ++n;
        return n;
    }
    int32_t queue_count(uint32_t producer) const {
        int32_t n = 0;
        for (const Pending& p : pending_)
            if (p.producer == producer && queue_entry(p)) ++n;
        return n + hangar_queued(producer);
    }
    template <class P> static bool queue_entry(const P& p) { return p.consumed_larva == kNoTarget && p.egg == kNoTarget; }
    int pending_of_type(int32_t player, const std::string& catalog_id) const {
        int n = 0;
        for (const Pending& p : pending_)
            if (p.producer < units_.size() && units_[p.producer].player == player && p.proto.catalog_id == catalog_id)
                n += p.count > 1 ? p.count : 1;
        return n;
    }
    bool cancel_train(uint32_t producer);
    bool cancel_train_slot(uint32_t producer, int slot);
    int queued_count(uint32_t producer) const;
    struct QueueSlotView {
        std::string catalog_id;
        float progress = 0.0f;
    };
    std::vector<QueueSlotView> queue_view(uint32_t producer) const {
        std::vector<QueueSlotView> out;
        for (const Pending& pd : pending_) {
            if (pd.producer != producer) continue;
            QueueSlotView v;
            v.catalog_id = pd.proto.catalog_id;
            if (pd.total.raw() > 0) {
                const double f = 1.0 - pd.timer.to_double() / pd.total.to_double();
                v.progress = float(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
            }
            out.push_back(std::move(v));
        }
        hangar_view(producer, out);
        return out;
    }

    bool cancel_construction(uint32_t building, int32_t mineral_cost, int32_t gas_cost,
                             Fixed refund = Fixed::from_ratio(3, 4));
    bool cancel_worker_build(uint32_t builder, int32_t mineral_cost, int32_t gas_cost);

    void set_tech_requirement(std::string unit_id, std::vector<std::string> prereqs);
    using TrainGate = std::function<bool(uint32_t producer, const std::string& unit_id, bool at_site)>;
    void set_train_gate(TrainGate gate) { train_gate_ = std::move(gate); }
    using RequirementGate = std::function<bool(int32_t player, const std::string& req_id)>;
    void set_requirement_gate(RequirementGate gate) { requirement_gate_ = std::move(gate); }
    bool tech_requirements_met(int32_t player, const std::string& unit_id) const;

    static constexpr int32_t kAddonNone = 0, kAddonTechLab = 1, kAddonReactor = 2;
    using AddonBodyFactory = std::function<bool(const Unit& host, int32_t kind, Unit& body)>;
    void set_addon_body_factory(AddonBodyFactory f) { addon_body_factory_ = std::move(f); }
    using UnitRefold = std::function<void(Unit& u)>;
    void set_unit_refold(UnitRefold f) { unit_refold_ = std::move(f); }
    void set_morph_keep_behaviors(bool on) { morph_keep_behaviors_ = on; }
    using CastCooldownFold = std::function<Fixed(const Unit& caster, int32_t ability, Fixed base)>;
    void set_cast_cooldown_fold(CastCooldownFold f) { cast_cd_fold_ = std::move(f); }
    Fixed cast_cooldown(const Unit& caster, int32_t ability, Fixed base) const {
        return cast_cd_fold_ && base.raw() > 0 ? cast_cd_fold_(caster, ability, base) : base;
    }
    void attach_addon(uint32_t building, int32_t kind) {
        if (building >= units_.size()) return;
        units_[building].addon = kind;
        units_[building].train_slots = (kind == kAddonReactor) ? 2 : 1;
        if (addon_body_factory_ && kind != kAddonNone && units_[building].addon_body == kNoTarget) {
            if (in_step_) pending_addon_bodies_.push_back(building);
            else spawn_addon_body(building);
        }
    }
    static void addon_slot(Fixed x, Fixed y, Fixed& ax, Fixed& ay) {
        ax = x + Fixed::from_ratio(5, 2);
        ay = y - Fixed::from_ratio(1, 2);
    }
    void spawn_addon_body(uint32_t building) {
        if (building >= units_.size() || !addon_body_factory_) return;
        const Unit host = units_[building];
        if (!host.alive || host.addon == kAddonNone || host.addon_body != kNoTarget) return;
        Unit body;
        if (!addon_body_factory_(host, host.addon, body)) return;
        body.player = host.player;
        addon_slot(host.x, host.y, body.x, body.y);
        body.alive = true;
        body.constructing = false;
        body.speed = Fixed();
        body.addon_host = building;
        body.addon_body_kind = host.addon;
        const uint32_t id = add_unit(body);
        units_[building].addon_body = id;
        evict_from_site(units_[id], building);
    }
    int32_t queue_depth(uint32_t building) const {
        if (building >= units_.size()) return 0;
        const Unit& u = units_[building];
        if (u.queue_size <= 0) return u.train_slots;
        return u.queue_size + (u.addon == kAddonReactor ? u.queue_bonus_reactor : 0);
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
        if (addon_body_factory_) {
            Unit probe;
            if (addon_body_factory_(b, kind, probe)) {
                Fixed ax, ay;
                addon_slot(b.x, b.y, ax, ay);
                if (site_blocked(probe, ax, ay, building)) {
                    refuse(b.player, CmdError::CantBuildLocationBlocked);
                    return false;
                }
            }
        }
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

    bool cast_chrono(uint32_t producer, uint32_t caster, Fixed energy_cost, Fixed duration) {
        if (producer >= units_.size() || caster >= units_.size()) return false;
        Unit& c = units_[caster];
        if (c.energy < energy_cost) return false;
        c.energy = c.energy - energy_cost;
        units_[producer].chrono_timer = duration;
        return true;
    }

    bool spend_build_charge(uint32_t id) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (!u.alive || u.build_charges <= 0 || u.build_cd.raw() > 0) return false;
        --u.build_charges;
        u.build_cd = u.build_cd_use;
        return true;
    }
    bool build_charge_ready(uint32_t id) const {
        return id < units_.size() && units_[id].build_charges > 0 && units_[id].build_cd.raw() <= 0;
    }
    uint32_t found_charged_build(uint32_t caster, const Unit& proto, Fixed x, Fixed y, Fixed build_time) {
        if (caster >= units_.size()) return kNoTarget;
        const bool uncharged = units_[caster].build_charges < 0;
        if (!uncharged && !build_charge_ready(caster)) return kNoTarget;
        const Unit& c = units_[caster];
        const Fixed dx = x - c.x, dy = y - c.y, reach = c.radius + c.build_range;
        if ((!uncharged || c.build_range.raw() > 0) && dx * dx + dy * dy > reach * reach) return kNoTarget;
        const int32_t owner = c.player;
        if (!uncharged) spend_build_charge(caster);
        Unit u = proto;
        u.player = owner;
        u.x = x;
        u.y = y;
        u.speed = Fixed();
        const uint32_t id = add_unit(u);
        Fixed start_hp = Fixed::from_raw(u.hp_max.raw() / 10);
        if (start_hp.raw() <= 0) start_hp = Fixed::from_int(1);
        start_construction(id, build_time.raw() > 0 ? build_time : Fixed::from_int(1), start_hp);
        return id;
    }

    void start_construction(uint32_t id, Fixed build_time, Fixed start_hp) {
        if (id >= units_.size() || build_time.raw() <= 0) return;
        Unit& u = units_[id];
        u.constructing = true;
        u.build_time = build_time;
        u.build_progress = Fixed();
        u.build_elapsed = Fixed();
        u.build_hp0 = start_hp;
        u.hp = start_hp;
        register_footprint_block(id);
    }
    bool lift_off(uint32_t id, Fixed lock = Fixed(), Fixed rise_delay = Fixed(), Fixed rise = Fixed()) {
        if (id >= units_.size()) return false;
        Unit& u = units_[id];
        if (!u.alive || u.flying) return false;
        u.flying = true;
        u.lift_lock = lock;
        u.lift_rise_delay = rise_delay;
        u.lift_rise = rise;
        u.lift_t = Fixed();
        u.orders.clear();
        if (u.addon_body != kNoTarget) {
            if (u.addon_body < units_.size()) units_[u.addon_body].addon_host = kNoTarget;
            u.addon_body = kNoTarget;
            u.addon = kAddonNone;
            u.train_slots = 1;
        }
        return true;
    }
    uint32_t free_addon_at_slot(uint32_t id, Fixed x, Fixed y) const {
        if (!addon_body_factory_ || id >= units_.size() || units_[id].addon_body != kNoTarget) return kNoTarget;
        Fixed ax, ay;
        addon_slot(x, y, ax, ay);
        const Fixed tol = Fixed::from_ratio(1, 4);
        for (const Unit& o : units_) {
            if (!o.alive || o.addon_body_kind == 0 || o.addon_host != kNoTarget || o.player != units_[id].player) continue;
            if (o.x < ax - tol || o.x > ax + tol || o.y < ay - tol || o.y > ay + tol) continue;
            Unit probe;
            if (!addon_body_factory_(units_[id], o.addon_body_kind, probe)) continue;
            return o.id;
        }
        return kNoTarget;
    }
    bool can_land(uint32_t id, Fixed x, Fixed y) const {
        if (id >= units_.size()) return false;
        const Unit& u = units_[id];
        if (!u.alive || !u.flying) return false;
        if (!creep_placement_ok(u, x, y)) return false;
        return !site_blocked(u, x, y, id);
    }
    bool order_land(uint32_t id, Fixed x, Fixed y) {
        if (!can_land(id, x, y)) return false;
        if (units_[id].lift_lock.raw() > 0) return false;
        issue(id, Order{OrderType::Move, x, y});
        Unit& u = units_[id];
        u.land_pending = true;
        u.land_x = x;
        u.land_y = y;
        u.land_timer = Fixed::from_int(-1);
        if (u.land_addon != 0) {
            player(u.player).minerals += u.land_addon_minerals;
            player(u.player).gas += u.land_addon_gas;
        }
        u.land_addon = 0;
        return true;
    }
    bool addon_slot_clear(uint32_t id, Fixed x, Fixed y, int32_t kind) const {
        if (id >= units_.size() || !addon_body_factory_) return true;
        Unit probe;
        if (!addon_body_factory_(units_[id], kind, probe)) return true;
        Fixed ax, ay;
        addon_slot(x, y, ax, ay);
        return !site_blocked(probe, ax, ay, id);
    }
    bool order_land_addon(uint32_t id, Fixed x, Fixed y, int32_t kind, Fixed build_time, int32_t minerals,
                          int32_t gas) {
        if (kind != kAddonTechLab && kind != kAddonReactor) return false;
        if (!addon_slot_clear(id, x, y, kind)) return false;
        if (id >= units_.size()) return false;
        Player& pl = player(units_[id].player);
        if (pl.minerals < minerals || pl.gas < gas) return false;
        if (!order_land(id, x, y)) return false;
        pl.minerals -= minerals;
        pl.gas -= gas;
        Unit& u = units_[id];
        u.land_addon = kind;
        u.land_addon_time = build_time;
        u.land_addon_minerals = minerals;
        u.land_addon_gas = gas;
        return true;
    }
    bool land(uint32_t id, Fixed x, Fixed y) {
        if (!can_land(id, x, y)) return false;
        Unit& u = units_[id];
        const uint32_t take = free_addon_at_slot(id, x, y);
        u.x = x;
        u.y = y;
        u.flying = false;
        register_footprint_block(id);
        evict_from_site(units_[id], kNoTarget);
        if (take != kNoTarget) {
            Unit body;
            const int32_t kind = units_[take].addon_body_kind;
            if (addon_body_factory_(units_[id], kind, body)) {
                units_[take].catalog_id = body.catalog_id;
                units_[take].addon_host = id;
                units_[id].addon_body = take;
                attach_addon(id, kind);
            }
        }
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
        if (!raise_evicts_)
            for (const Unit& o : units_) {
                if (!o.alive || o.id == id || o.flying) continue;
                const Fixed dx = o.x - u.x, dy = o.y - u.y;
                const Fixed rr = u.radius + o.radius;
                if (dx * dx + dy * dy < rr * rr) return false;
            }
        u.lowered = false;
        register_footprint_block(id);
        if (raise_evicts_) evict_from_site(units_[id], kNoTarget);
        return true;
    }
    bool is_constructing(uint32_t id) const {
        return id < units_.size() && units_[id].constructing;
    }
    Fixed build_progress(uint32_t id) const {
        return id < units_.size() ? units_[id].build_progress : Fixed();
    }

    bool add_behavior(uint32_t id, const BehaviorInstance& b);

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
        u.life_paid = u.life_paid + amount;
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
        u.salvage_total = delay;
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
    const std::vector<PersistentInstance>& persistents() const { return persistents_; }
    const std::vector<ForceFieldInstance>& force_fields() const { return force_fields_; }
    static constexpr std::size_t kPresentationLogCap = 2048;
    void set_presentation_log(bool on) {
        presentation_log_ = on;
        if (!on) {
            presentation_events_.clear();
            presentation_delayed_.clear();
        }
    }
    bool presentation_log() const { return presentation_log_; }
    const std::vector<PresentationEvent>& presentation_events() const { return presentation_events_; }

    bool start_heal(std::uint32_t caster, std::uint32_t target, Fixed heal_rate, Fixed drain_factor,
                    Fixed range, HealVital vital = HealVital::Life);
    bool stop_heal(std::uint32_t caster);
    bool is_healing(std::uint32_t caster) const;
    size_t healer_count() const { return healers_.size(); }
    const std::vector<HealerInstance>& healers() const { return healers_; }
    const std::vector<PendingEffect>& pending_effects() const { return pending_effects_; }

    int32_t add_force_field(Fixed x, Fixed y, Fixed radius, Fixed duration, std::uint32_t unit_tag = 0) {
        ForceFieldInstance ff;
        ff.unit_tag = unit_tag;
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
                    if (dx * dx + dy * dy <= r2 && claim_nav_cell(cx, cy, true)) {
                        nav_.mark_ff(cx, cy, true);
                        ff.cells.emplace_back(cx, cy);
                    }
                }
        }
        displace_from_force_field(ff);
        force_fields_.push_back(std::move(ff));
        return int32_t(force_fields_.size()) - 1;
    }
    size_t force_field_count() const { return force_fields_.size(); }
    void displace_from_force_field(const ForceFieldInstance& ff);

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

    void begin_morph(uint32_t id, int32_t proto, Fixed time, uint32_t finish_behavior = 0,
                     std::vector<MorphStep> steps = {}) {
        begin_morph(id, proto, time, finish_behavior, std::move(steps), MorphExtras{});
    }
    void begin_morph(uint32_t id, int32_t proto, Fixed time, uint32_t finish_behavior,
                     std::vector<MorphStep> steps, MorphExtras extras) {
        if (id >= units_.size() || proto < 0 || proto >= int32_t(spawn_protos_.size())) return;
        if (morph_disabled(units_[id])) return;
        for (const MorphStep& s : steps)
            if (s.proto < 0 || s.proto >= int32_t(spawn_protos_.size())) return;
        std::stable_sort(steps.begin(), steps.end(), [](const MorphStep& a, const MorphStep& b) { return a.at < b.at; });
        Unit& u = units_[id];
        u.morph_proto = proto;
        u.morph_timer = time;
        u.morph_total = time;
        u.morph_finish_beh = finish_behavior;
        u.morph_staged = !steps.empty();
        u.morph_steps = std::move(steps);
        u.morph_delay = Fixed();
        if (extras.delay_max > extras.delay_min) {
            const Fixed u01 = Fixed::from_ratio(int(rng_.below(4096)), 4096);
            u.morph_delay = extras.delay_min + (extras.delay_max - extras.delay_min) * u01;
        } else if (extras.delay_min.raw() > 0) {
            u.morph_delay = extras.delay_min;
        }
        if (u.morph_delay.raw() < 0) u.morph_delay = Fixed();
        for (BehaviorInstance& b : extras.behaviors_on) {
            bool have = false;
            for (const BehaviorInstance& o : u.behaviors)
                if (o.permanent && o.src == b.src) have = true;
            if (have) continue;
            b.permanent = true;
            u.behaviors.push_back(std::move(b));
        }
        u.morph_beh_off = std::move(extras.behaviors_off);
        morph_origin_.erase(id);
    }
    void drop_morph_behaviors(Unit& u) {
        if (u.morph_beh_off.empty()) return;
        const std::vector<uint32_t> off = std::move(u.morph_beh_off);
        u.morph_beh_off.clear();
        u.behaviors.erase(std::remove_if(u.behaviors.begin(), u.behaviors.end(),
                                         [&](const BehaviorInstance& b) {
                                             return b.permanent && std::find(off.begin(), off.end(), b.src) != off.end();
                                         }),
                          u.behaviors.end());
    }
    bool is_morph_delayed(uint32_t id) const { return id < units_.size() && units_[id].morph_proto >= 0 && units_[id].morph_delay.raw() > 0; }
    const Unit* morph_origin(uint32_t id) const {
        if (id >= units_.size() || units_[id].morph_proto < 0) return nullptr;
        auto it = morph_origin_.find(id);
        return it != morph_origin_.end() ? &it->second : nullptr;
    }
    const std::string& morph_shown_id(uint32_t id) const {
        static const std::string kNone;
        if (id >= units_.size()) return kNone;
        if (const Unit* o = morph_origin(id)) return o->catalog_id;
        return units_[id].catalog_id;
    }
    bool begin_burrow(uint32_t id, bool down, Fixed flag_at, Fixed total);
    bool is_burrowing(uint32_t id) const { return id < units_.size() && units_[id].burrow_timer.raw() > 0; }
    bool begin_depot_morph(uint32_t id, bool down, Fixed flag_at, Fixed total);
    bool is_depot_morphing(uint32_t id) const { return id < units_.size() && units_[id].depot_timer.raw() > 0; }
    bool cancel_morph(uint32_t id);
    std::string morph_target_id(uint32_t id) const {
        if (id >= units_.size()) return std::string();
        const int32_t p = units_[id].morph_proto;
        return (p >= 0 && p < int32_t(spawn_protos_.size())) ? spawn_protos_[size_t(p)].catalog_id : std::string();
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
        end_phase(a);
        return true;
    }
    void end_phase(Unit& a) {
        a.phase_timer = Fixed();
        a.phase_shade = kNoTarget;
        if (a.phase_caster_tag != 0) {
            const uint32_t tag = a.phase_caster_tag;
            a.behaviors.erase(std::remove_if(a.behaviors.begin(), a.behaviors.end(),
                                             [tag](const BehaviorInstance& b) { return b.src == tag; }),
                              a.behaviors.end());
        }
        a.phase_caster_tag = a.phase_cancel_tag = 0;
    }
    bool is_phasing(uint32_t id) const { return id < units_.size() && units_[id].phase_timer.raw() > 0; }

    void register_spawner_type(const std::string& catalog_id, int32_t proto, int32_t cap, Fixed period, int32_t hard_cap = 0) {
        spawner_type_reg_[catalog_id] = SpawnerType{proto, cap, period, hard_cap};
    }
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
    void set_spawn_offsets(int32_t proto, std::vector<std::pair<Fixed, Fixed>> offsets) {
        if (proto < 0) return;
        if (int32_t(spawn_offsets_.size()) <= proto) spawn_offsets_.resize(std::size_t(proto) + 1);
        spawn_offsets_[std::size_t(proto)] = std::move(offsets);
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
    void brood_counts(uint32_t id, int& natural, int& total) const {
        natural = total = 0;
        for (const Unit& u : units_)
            if (u.alive && u.spawn_parent == id) {
                ++total;
                if (!u.larva_injected) ++natural;
            }
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
        units_[id].warp_forming = true;
        return true;
    }
    bool warp_ready(uint32_t gate) const {
        return gate < units_.size() && units_[gate].alive && units_[gate].warp_cd.raw() <= 0;
    }

    bool field_covers(const Unit& s, Fixed x, Fixed y, int32_t player) const {
        if (!s.alive || s.constructing || s.power_radius.raw() <= 0 || !are_allied(s.player, player))
            return false;
        const Fixed dx = x - s.x, dy = y - s.y;
        if (dx * dx + dy * dy > s.power_radius * s.power_radius) return false;
        if (!s.power_reaches_higher && nav_.configured()) {
            int sx = 0, sy = 0, tx = 0, ty = 0;
            nav_.to_cell(s.x, s.y, sx, sy);
            nav_.to_cell(x, y, tx, ty);
            if (nav_.level_cell(tx, ty) > nav_.level_cell(sx, sy)) return false;
        }
        return true;
    }

    bool powered_at(Fixed x, Fixed y, int32_t player) const {
        for (const Unit& s : units_)
            if (field_covers(s, x, y, player)) return true;
        return false;
    }

    int enabler_level_at(Fixed x, Fixed y, int32_t player) const {
        int lvl = 0;
        for (const Unit& s : units_) {
            if (!s.alive || s.constructing || s.enabler_radius.raw() <= 0 || s.enabler_level <= lvl ||
                !are_allied(s.player, player))
                continue;
            const Fixed dx = x - s.x, dy = y - s.y;
            if (dx * dx + dy * dy <= s.enabler_radius * s.enabler_radius) lvl = s.enabler_level;
        }
        return lvl;
    }

    int field_level(const Unit& s) const {
        const int base = s.power_level > 0 ? s.power_level : 1;
        if (s.power_boost_min <= 0 || s.power_boost_level <= base) return base;
        return enabler_level_at(s.x, s.y, s.player) >= s.power_boost_min ? s.power_boost_level : base;
    }

    int power_level_at(Fixed x, Fixed y, int32_t player) const {
        int lvl = 0;
        for (const Unit& s : units_)
            if (field_covers(s, x, y, player)) lvl = std::max(lvl, field_level(s));
        return lvl;
    }

    int cargo_used(uint32_t transport) const {
        int n = 0;
        for (const Unit& u : units_)
            if (u.alive && u.loaded && holds(transport, u)) n += u.cargo_size;
        return n;
    }
    bool holds(uint32_t transport, const Unit& u) const {
        if (u.transport_id == transport) return true;
        if (transport >= units_.size() || u.transport_id >= units_.size()) return false;
        const Unit& t = units_[transport];
        const Unit& c = units_[u.transport_id];
        return t.player_hold && c.player_hold && c.alive && t.alive && t.player == c.player;
    }
    bool load_unit(uint32_t transport, uint32_t unit) {
        if (transport >= units_.size() || unit >= units_.size() || transport == unit) return false;
        Unit& t = units_[transport];
        Unit& u = units_[unit];
        if (!t.alive || !u.alive || t.cargo_space <= 0 || u.loaded || u.cargo_size <= 0) return false;
        if (t.max_cargo_size > 0 && u.cargo_size > t.max_cargo_size) return false;
        if (boarding_suppressed(u)) return false;
        if (t.load_require_race >= 0 && u.race_index != t.load_require_race) return false;
        if (!t.load_require_types.empty()) {
            bool ok = false;
            for (const std::string& id : t.load_require_types)
                if (id == u.catalog_id) { ok = true; break; }
            if (!ok) return false;
        }
        for (const std::string& id : t.load_exclude_types)
            if (id == u.catalog_id) return false;
        for (std::uint32_t beh : t.load_block_behaviors)
            for (const BehaviorInstance& b : u.behaviors)
                if (b.src == beh && (b.permanent || b.timer.raw() > 0)) return false;
        if (cargo_used(transport) + u.cargo_size > t.cargo_space) return false;
        u.loaded = true;
        u.transport_id = transport;
        return true;
    }
    int unload_all_at(uint32_t transport, Fixed atx, Fixed aty) {
        if (transport >= units_.size() || !units_[transport].alive) return 0;
        if (transport_disabled(units_[transport])) return 0;
        static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        int n = 0;
        for (Unit& u : units_) {
            if (!u.alive || !u.loaded || !holds(transport, u)) continue;
            u.loaded = false;
            u.transport_id = kNoTarget;
            u.x = atx + Fixed::from_int(DX[n & 7]);
            u.y = aty + Fixed::from_int(DY[n & 7]);
            ++n;
        }
        return n;
    }
    std::vector<uint32_t> passengers(uint32_t transport) const {
        std::vector<uint32_t> out;
        if (transport >= units_.size() || !units_[transport].alive) return out;
        for (const Unit& u : units_)
            if (u.alive && u.loaded && holds(transport, u)) out.push_back(u.id);
        return out;
    }
    uint32_t unload_one(uint32_t transport, int slot) {
        if (transport < units_.size() && transport_disabled(units_[transport])) return kNoTarget;
        const std::vector<uint32_t> hold = passengers(transport);
        if (slot < 0 || std::size_t(slot) >= hold.size()) return kNoTarget;
        static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        Unit& p = units_[hold[std::size_t(slot)]];
        p.loaded = false;
        p.transport_id = kNoTarget;
        p.x = units_[transport].x + Fixed::from_int(DX[slot & 7]);
        p.y = units_[transport].y + Fixed::from_int(DY[slot & 7]);
        return p.id;
    }
    uint32_t unload_next(uint32_t transport, int ring) {
        if (transport >= units_.size() || !units_[transport].alive) return kNoTarget;
        static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        for (Unit& u : units_) {
            if (!u.alive || !u.loaded || !holds(transport, u)) continue;
            u.loaded = false;
            u.transport_id = kNoTarget;
            u.x = units_[transport].x + Fixed::from_int(DX[ring & 7]);
            u.y = units_[transport].y + Fixed::from_int(DY[ring & 7]);
            return u.id;
        }
        return kNoTarget;
    }
    int unload_all(uint32_t transport) {
        if (transport >= units_.size()) return 0;
        return unload_all_at(transport, units_[transport].x, units_[transport].y);
    }

    uint64_t state_hash() const;

    const std::vector<Unit>& units() const { return units_; }
    int tick() const { return tick_; }

    int32_t charge_pool_count(int32_t player, const std::string& link, int32_t start, int32_t max, int start_ticks,
                              int use_ticks) const {
        ChargePool p = find_charge_pool(player, link) ? *find_charge_pool(player, link)
                                                      : fresh_charge_pool(player, link, start, max, start_ticks, use_ticks);
        advance_charge_pool(p, tick_);
        return p.count;
    }
    bool charge_pool_take(int32_t player, const std::string& link, int32_t start, int32_t max, int start_ticks,
                          int use_ticks, int32_t n) {
        ChargePool* p = find_charge_pool_mut(player, link);
        if (!p) {
            charge_pools_.push_back(fresh_charge_pool(player, link, start, max, start_ticks, use_ticks));
            p = &charge_pools_.back();
        }
        advance_charge_pool(*p, tick_);
        if (p->count < n) return false;
        const bool was_full = p->next_tick < 0;
        p->count -= n;
        if (was_full && p->count < p->max && p->use_ticks > 0) p->next_tick = tick_ + p->use_ticks;
        return true;
    }
    int alive_count(int player) const;
    int defeat_holders(int player) const;
    bool defeat_rule_authored() const { return defeat_rule_authored_; }

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
    void register_interceptor_hangar(const std::string& catalog_id, int32_t queue_size, int32_t minerals,
                                     int32_t gas, bool auto_build, bool auto_build_on, bool show_status) {
        auto it = interceptor_arm_reg_.find(catalog_id);
        if (it == interceptor_arm_reg_.end()) return;
        it->second.queue_size = queue_size;
        it->second.minerals = minerals;
        it->second.gas = gas;
        it->second.auto_build = auto_build;
        it->second.auto_build_on = auto_build_on;
        it->second.show_status = show_status;
    }
    bool hangar_enqueue(uint32_t carrier);
    bool hangar_cancel_slot(uint32_t carrier, int slot);
    int32_t hangar_queued(uint32_t carrier) const;
    int32_t hangar_stock(uint32_t carrier) const;
    bool hangar_auto_build(uint32_t carrier) const;
    bool set_hangar_auto_build(uint32_t carrier, bool on);
    void hangar_view(uint32_t carrier, std::vector<QueueSlotView>& out) const;
    void step_interceptors(Fixed dt);
    bool hosts_interceptors(uint32_t id) const {
        for (const auto& c : carriers_)
            if (c.carrier == id) return true;
        return false;
    }
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

    void set_gather_dock_footprint(bool on) { gather_dock_footprint_ = on; }
    void set_separation_respects_nav(bool on) { sep_respects_nav_ = on; }
    void set_pull_with_radius(bool on) { pull_with_radius_ = on; }
    void set_acquire_skips_neutral(bool on) { acquire_skips_neutral_ = on; }
    void set_raise_evicts(bool on) { raise_evicts_ = on; }
    void set_exit_open_spot(bool on) { exit_open_spot_ = on; }
    void set_spawn_open_spot(bool on) { spawn_open_spot_ = on; }
    void set_return_dock_footprint(bool on) { return_dock_footprint_ = on; }
    void set_gather_bounce(bool on) { gather_bounce_ = on; }
    void set_cargo_idle_without_dropoff(bool on) { cargo_idle_without_dropoff_ = on; }
    void set_unstick_bodies(bool on) { unstick_bodies_ = on; }
    void set_disc_slide(bool on) { disc_slide_ = on; }
    void set_rest_push_collide(bool on) { rest_push_collide_ = on; }
    void set_sep_push_rules(bool on) { sep_push_rules_ = on; }
    void set_level_guard(bool on) { level_guard_ = on; }
    bool separation_respects_nav() const { return sep_respects_nav_; }
    static bool default_gas_worker_inside();
    void set_research_queue(bool on) { research_queue_ = on; }
    bool research_queue() const { return research_queue_; }
    void set_research_dies_with_host(bool on) { research_dies_with_host_ = on; }
    bool research_dies_with_host() const { return research_dies_with_host_; }
    struct LostResearch {
        int32_t player = 0;
        std::string id;
        uint32_t producer = kNoTarget;
    };
    std::vector<LostResearch> take_lost_research() {
        std::vector<LostResearch> out;
        out.swap(research_lost_);
        return out;
    }
    void set_gas_worker_inside(bool on) { gas_worker_inside_ = on; }
    bool gas_worker_inside() const { return gas_worker_inside_; }
    static bool default_gas_harvest_ordered();
    static bool default_cargo_hidden();
    static bool default_cliff_leap();
    static bool default_morph_vital_fraction();
    static bool default_path_nearest_fallback();
    void set_path_nearest_fallback(bool on) { path_nearest_fallback_ = on; }
    void set_stall_unreachable(bool on) { stall_unreachable_ = on; }
    void set_smart_structure_contact(bool on) { smart_structure_contact_ = on; }
    void set_smart_lowered_open(bool on) { smart_lowered_open_ = on; }
    Fixed footprint_gap(const Unit& t, Fixed x, Fixed y) const;
    void set_morph_vital_fraction(bool on) { morph_vital_fraction_ = on; }
    void set_cliff_leap(bool on) { cliff_leap_ = on; }
    bool cliff_leap() const { return cliff_leap_; }
    void set_cargo_hidden(bool on) { cargo_hidden_ = on; }
    bool cargo_hidden() const { return cargo_hidden_; }
    void set_gas_harvest_ordered(bool on) { gas_harvest_ordered_ = on; }
    bool gas_harvest_ordered() const { return gas_harvest_ordered_; }
    bool gas_node_harvestable(const Unit& n, int32_t player) const {
        return n.alive && n.is_gas && !n.resource_raw && n.resource_left > 0 && (n.player < 0 || n.player == player);
    }
    void set_exact_build_progress(bool on) { exact_build_progress_ = on; }
    void set_accelerating_movers(bool on) { accelerating_movers_ = on; }
    void set_turret_aim(bool on) { turret_aim_ = on; }
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
    void set_train_affordability(bool on) { train_affordability_ = on; }
    bool train_affordability() const { return train_affordability_; }
    void set_reserve_supply(bool on) { reserve_supply_ = on; }
    bool reserve_supply() const { return reserve_supply_; }
    void set_turn_limit(bool on) { turn_limit_ = on; }
    bool turn_limit() const { return turn_limit_; }
    void set_acceleration(bool on) { acceleration_ = on; }
    bool acceleration() const { return acceleration_; }
    long long nav_plans() const { return nav_plans_; }
    long long nav_plan_failures() const { return nav_plan_failures_; }
    void set_arc_movement(bool on) { arc_movement_ = on; }
    bool arc_movement() const { return arc_movement_; }

    void set_leash(bool on, Fixed radius = Fixed(), Fixed reset = Fixed()) {
        leash_ = on;
        leash_radius_ = radius;
        leash_reset_ = reset;
    }
    Fixed leash_radius() const { return leash_radius_; }
    Fixed leash_reset_radius() const { return leash_reset_; }
    void set_call_for_help(Fixed radius, Fixed period) {
        help_radius_ = radius;
        help_period_ = period;
    }
    Fixed call_for_help_radius() const { return help_radius_; }
    Fixed call_for_help_period() const { return help_period_; }
    void set_attack_reveal(Fixed seconds) { attack_reveal_time_ = seconds; }
    Fixed attack_reveal_time() const { return attack_reveal_time_; }
    int last_progress_tick() const { return last_progress_tick_; }
    void note_progress() { last_progress_tick_ = tick_; }
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

    static bool block_claims_old() {
        static const bool kOld = std::getenv("OMC_NAV_BLOCK_CLAIMS_OLD") != nullptr;
        return kOld;
    }
    std::uint16_t* nav_claim_slot(int x, int y) {
        if (!nav_.in_bounds(x, y)) return nullptr;
        const std::size_t n = std::size_t(nav_.width()) * std::size_t(nav_.height());
        if (nav_claims_.size() != n) nav_claims_.assign(n, 0);
        return &nav_claims_[std::size_t(y) * std::size_t(nav_.width()) + std::size_t(x)];
    }
    bool claim_nav_cell(int x, int y, bool over_ff = false) {
        if (!nav_.in_bounds(x, y)) return false;
        if (block_claims_old()) {
            if (nav_.is_blocked_cell(x, y)) return false;
            nav_.set_blocked(x, y, true);
            return true;
        }
        std::uint16_t* c = nav_claim_slot(x, y);
        if (*c == 0) {
            if (nav_.is_blocked_cell(x, y)) return false;
            nav_.set_blocked(x, y, true);
        } else if (over_ff && !(nav_.has_ff() && nav_.is_ff_cell(x, y))) {
            return false;
        } else if (!over_ff && nav_.has_ff() && nav_.is_ff_cell(x, y)) {
            nav_.mark_ff(x, y, false);
        }
        ++*c;
        return true;
    }
    bool release_nav_cell(int x, int y) {
        if (!nav_.in_bounds(x, y)) return false;
        if (!block_claims_old()) {
            std::uint16_t* c = nav_claim_slot(x, y);
            if (*c > 1) {
                --*c;
                return false;
            }
            *c = 0;
        }
        nav_.set_blocked(x, y, false);
        return true;
    }
    void register_destructible(uint32_t unit_id, int cx0, int cy0, int cx1, int cy1) {
        Destructible d;
        d.id = unit_id;
        for (int y = cy0; y <= cy1; ++y)
            for (int x = cx0; x <= cx1; ++x)
                if (claim_nav_cell(x, y)) d.cells.emplace_back(x, y);
        destructibles_.push_back(std::move(d));
    }
    void register_destructible_cells(uint32_t unit_id, const std::vector<std::pair<int, int>>& cells) {
        Destructible d;
        d.id = unit_id;
        for (const auto& [x, y] : cells)
            if (claim_nav_cell(x, y)) d.cells.emplace_back(x, y);
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

    int refresh_defeats();
    int32_t alliance_key(int32_t p) const {
        const int32_t t = team_of(p);
        return t >= 0 ? t : -(p + 2);
    }
    int32_t winning_team() const;

private:
    std::uint8_t hit_kind_ = kDamageKindSpell;
    std::uint8_t firing_death_ = 0;
    std::vector<HitEvent> hit_log_;
    std::vector<ResourceEmptyEvent> resource_empty_log_;
    std::vector<LaunchEvent> launch_log_;
    struct ChargePool {
        int32_t player = -1;
        std::string link;
        int32_t count = 0, max = 0;
        int next_tick = -1;
        int use_ticks = 0;
    };
    std::vector<ChargePool> charge_pools_;
    std::vector<CastChargeSpec> cast_charges_;
    const ChargePool* find_charge_pool(int32_t player, const std::string& link) const {
        for (const ChargePool& p : charge_pools_)
            if (p.player == player && p.link == link) return &p;
        return nullptr;
    }
    ChargePool* find_charge_pool_mut(int32_t player, const std::string& link) {
        for (ChargePool& p : charge_pools_)
            if (p.player == player && p.link == link) return &p;
        return nullptr;
    }
    static ChargePool fresh_charge_pool(int32_t player, const std::string& link, int32_t start, int32_t max,
                                        int start_ticks, int use_ticks) {
        ChargePool p;
        p.player = player;
        p.link = link;
        p.max = max;
        p.count = start < max ? start : max;
        p.use_ticks = use_ticks;
        p.next_tick = (p.count < max && use_ticks > 0) ? start_ticks : -1;
        return p;
    }
    static void advance_charge_pool(ChargePool& p, int now) {
        while (p.next_tick >= 0 && p.next_tick <= now) {
            ++p.count;
            p.next_tick = (p.count < p.max && p.use_ticks > 0) ? p.next_tick + p.use_ticks : -1;
        }
    }
    bool defeat_rule_authored_ = false;
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
    std::vector<std::uint16_t> nav_claims_;

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
        int32_t queue_size = 0;
        int32_t minerals = 0;
        int32_t gas = 0;
        bool auto_build = false;
        bool auto_build_on = false;
        bool show_status = false;
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
        int32_t queue_size = 0;
        int32_t queued = 0;
        int32_t minerals = 0;
        int32_t gas = 0;
        bool auto_build = false;
        bool show_status = false;
    };
    void step_hangar(Carrier& c, Fixed dt);
    std::unordered_map<std::string, InterceptorArm> interceptor_arm_reg_;
    struct SpawnerType { int32_t proto; int32_t cap; Fixed period; int32_t hard_cap; };
    std::unordered_map<std::string, SpawnerType> spawner_type_reg_;
    void arm_spawner_on_completion(Unit& u) {
        if (u.spawner_proto >= 0 || u.catalog_id.empty()) return;
        auto it = spawner_type_reg_.find(u.catalog_id);
        if (it == spawner_type_reg_.end()) return;
        const SpawnerType& t = it->second;
        if (t.proto < 0 || t.proto >= int32_t(spawn_protos_.size())) return;
        u.spawner_proto = t.proto;
        u.spawner_cap = t.cap;
        u.spawner_hard_cap = t.hard_cap;
        u.spawner_period = t.period;
        u.spawner_timer = t.period;
    }
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
        Fixed total;
        bool has_pos = false;
        Fixed px{}, py{};
        int32_t count = 1;
        int32_t mineral_cost = 0, gas_cost = 0;
        uint32_t consumed_larva = kNoTarget;
        uint32_t rally_from = kNoTarget;
        uint32_t egg = kNoTarget;
    };

    struct WorkerBuild {
        uint32_t builder;
        Fixed x, y;
        Unit proto;
        Fixed build_time;
        Fixed arrive;
        BuildVerb verb = BuildVerb::Committed;
        int32_t mineral_cost = 0, gas_cost = 0;
    };
 public:
    Fixed site_half_extent(const Unit& proto) const;
    bool site_blocked(const Unit& proto, Fixed x, Fixed y, uint32_t skip) const;
    bool site_reserved(const Unit& proto, Fixed x, Fixed y, int32_t player, uint32_t skip_builder) const;
    void evict_from_site(const Unit& structure, uint32_t builder);
    void train_exit_point(const Unit& producer, Fixed unit_radius, Fixed& ox, Fixed& oy) const;
    bool body_overlaps_blocked(Fixed cx, Fixed cy, Fixed r) const;
    bool nearest_open_spot(Fixed& x, Fixed& y, Fixed r, bool avoid_nospawn = false) const;
    bool ground_nospawn_at(Fixed x, Fixed y) const;
 private:

    std::vector<Unit> units_;
    std::vector<Player> players_;
    std::vector<Fixed> supply_ceiling_;
    std::vector<Pending> pending_;
    std::vector<WorkerBuild> worker_builds_;
    std::unordered_map<uint32_t, Unit> morph_founder_;
    std::unordered_map<uint32_t, Unit> morph_origin_;
    void morph_become(Unit& u, int32_t proto, bool vitals);
    void morph_vitals(Unit& u, int32_t proto);
    void morph_finish(Unit& u);
    std::vector<PersistentInstance> persistents_;
    std::vector<HealerInstance> healers_;
    std::vector<StatModInstance> stat_mods_;
    std::vector<KnockImpulse> knockbacks_;
    std::vector<ForceFieldInstance> force_fields_;
    bool presentation_log_ = false;
    std::vector<PresentationEvent> presentation_events_;
    std::uint64_t presentation_seq_ = 0;
    struct DelayedPresentation {
        Fixed left;
        PresentationEvent e;
    };
    std::vector<DelayedPresentation> presentation_delayed_;
    std::uint64_t log_presentation(std::uint32_t tag, bool stop, std::uint64_t instance, std::uint32_t source,
                                   std::uint32_t target, Fixed sx, Fixed sy, Fixed tx, Fixed ty, std::int32_t player);
    std::uint64_t log_effect_edge(std::uint32_t tag, bool stop, std::uint64_t instance, const EffectContext& ctx);
    void log_cast_start(const Unit& u, const AbilityDef& ab, const EffectContext& ctx);

    struct PendingResearch {
        int32_t player = 0;
        std::string id;
        Fixed timer;
        Fixed total;
        uint32_t producer = kNoTarget;
        int32_t minerals = 0, gas = 0;
    };
    std::unordered_map<int32_t, std::vector<std::string>> researched_;
    std::vector<PendingResearch> research_pending_;
    std::vector<std::pair<int32_t, std::string>> research_done_;
    void step_research(Fixed dt);
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
        return u.chrono_timer.raw() > 0 ? dt * u.chrono_mult : dt;
    }
    std::vector<Unit> spawn_protos_;
    int32_t egg_proto_ = -1;
    bool larva_egg_morph_ = false;
    struct PendingSpawn { int32_t proto; int32_t count; Fixed x, y; int32_t owner;
                          uint32_t parent = kNoTarget; bool injected = false;
                          Fixed range{};
                          uint32_t phase_link = kNoTarget; Fixed move_x{}, move_y{};
                          uint32_t gather_node = kNoTarget, depot = kNoTarget;
                          bool exact_point = false; EffectRef fx = kNoEffect; uint32_t fx_source = kNoTarget;
                          Fixed cast_x{}, cast_y{}; bool has_cast_point = false; };
    std::vector<PendingSpawn> pending_spawns_;
    std::vector<Fixed> sep_budget_, sep_allowance_, sep_x0_, sep_y0_;
    std::vector<Fixed> sep_in_x_, sep_in_y_;
    bool sep_respects_nav_ = false;
    bool pull_with_radius_ = false;
    bool acquire_skips_neutral_ = false;
    bool raise_evicts_ = false;
    bool exit_open_spot_ = false;
    bool spawn_open_spot_ = false;
    bool return_dock_footprint_ = false;
    bool gather_bounce_ = false;
    bool cargo_idle_without_dropoff_ = false;
    bool unstick_bodies_ = false;
    bool disc_slide_ = false;
    bool rest_push_collide_ = false;
    bool sep_push_rules_ = false;
    bool level_guard_ = false;
    std::vector<std::uint8_t> sep_held_;
    Fixed crowd_max_radius_;
    int crowd_max_tick_ = -1;
    std::vector<uint32_t> dropoffs_;
    int dropoffs_tick_ = -1;
    size_t dropoffs_n_ = 0;
    std::vector<uint32_t> power_sources_;
    std::vector<int32_t> prod_rank_, prod_done_;
    struct BehaviorFx {
        EffectRef ref;
        std::uint32_t holder, caster;
        int32_t origin;
        Fixed px{}, py{};
        bool has_point = false;
    };
    std::vector<BehaviorFx> behavior_fx_;
    void on_unit_damaged(const Unit& u, Fixed vitals_before) {
        if (u.hp + u.shields >= vitals_before) return;
        for (const BehaviorInstance& b : u.behaviors)
            if (b.fx_resp_handled != kNoEffect && (b.permanent || b.timer.raw() > 0))
                behavior_fx_.push_back({b.fx_resp_handled, u.id, b.fx_caster, b.fx_origin, b.fx_px, b.fx_py, b.fx_has_point});
    }
    static void behavior_stats_end(Unit& u, BehaviorInstance& b) {
        if (b.food.raw() == 0 && b.life_max.raw() == 0) return;
        u.supply_provides = u.supply_provides - b.food;
        u.hp_max = u.hp_max - b.life_max;
        if (u.hp > u.hp_max) u.hp = u.hp_max;
        b.food = Fixed();
        b.life_max = Fixed();
    }
    void queue_behavior_final(const Unit& u, const BehaviorInstance& b) {
        if (b.fx_final != kNoEffect)
            behavior_fx_.push_back({b.fx_final, u.id, b.fx_caster, b.fx_origin, b.fx_px, b.fx_py, b.fx_has_point});
    }
    CmdError last_error_ = CmdError::None;
    int32_t last_error_player_ = -1;
    void refuse(int32_t player, CmdError e) { last_error_ = e; last_error_player_ = player; }
    AddonBodyFactory addon_body_factory_;
    UnitRefold unit_refold_;
    CastCooldownFold cast_cd_fold_;
    bool morph_keep_behaviors_ = false;
    std::vector<uint32_t> pending_addon_bodies_;
    std::vector<std::vector<std::pair<Fixed, Fixed>>> spawn_offsets_;
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
        std::uint8_t slot = 0;
        Fixed launch_x, launch_y;
        Fixed accel, vmax;
    };
    std::vector<PendingHit> pending_hits_;
    std::vector<PendingEffect> pending_effects_;
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
    bool turret_aim_ = false;
    bool accelerating_movers_ = false;
    bool exact_build_progress_ = false;
    bool gather_dock_footprint_ = false;
    bool gas_worker_inside_ = default_gas_worker_inside();
    bool gas_harvest_ordered_ = default_gas_harvest_ordered();
    bool cargo_hidden_ = default_cargo_hidden();
    bool cliff_leap_ = default_cliff_leap();
    bool morph_vital_fraction_ = default_morph_vital_fraction();
    bool path_nearest_fallback_ = default_path_nearest_fallback();
    bool stall_unreachable_ = false;
    bool smart_structure_contact_ = false;
    bool smart_lowered_open_ = false;
    bool research_queue_ = false;
    bool research_dies_with_host_ = false;
    std::vector<LostResearch> research_lost_;
    bool death_reveal_enabled_ = false;
    ScanSweep scan_sweep_;
    bool building_burndown_enabled_ = false;
    bool flee_response_enabled_ = false;
    bool multi_hit_armor_ = false;
    bool target_priority_ = false;
    NavGrid nav_;
    TerrainMap placement_;
    Rng rng_;
    uint64_t seed_ = 1;
    int tick_ = 0;
    OrderSink* order_sink_ = nullptr;
    TrainSink* train_sink_ = nullptr;
    bool in_step_ = false;
    bool reporting() const { return order_sink_ != nullptr && !in_step_; }
    void report_command(uint32_t id, const Command& c) {
        if (reporting()) order_sink_->on_command(tick_, id, c);
    }
    long long nav_plans_ = 0;
    long long nav_plan_failures_ = 0;
    bool separation_ = false;
    bool fog_acquire_ = false;
    bool target_sort_ = false;
    bool attack_windup_ = false;
    bool projectile_travel_ = false;
    bool point_defense_enabled_ = false;
    bool reserve_supply_ = false;
    bool train_affordability_ = false;
    std::unordered_map<std::string, std::vector<std::string>> tech_reqs_;
    std::unordered_set<std::string> techlab_gated_;
    bool turn_limit_ = false;
    bool acceleration_ = false;
    std::map<std::pair<int32_t, std::uint32_t>, BehaviorUpgradeMod> behavior_upgrade_mods_;
    TrainGate train_gate_;
    RequirementGate requirement_gate_;
    bool leash_ = false;
    Fixed leash_radius_{}, leash_reset_{};
    Fixed help_radius_{}, help_period_{};
    Fixed attack_reveal_time_{};
    int last_progress_tick_ = 0;
    bool arc_movement_ = false;
    bool backswing_ = false;
    bool fwm_ = false;

    uint32_t acquire_target(const Unit& u, Fixed max_range = Fixed()) const;
    bool detected_by(int32_t player, const Unit& target) const {
        for (const Unit& d : units_) {
            if (!d.alive || d.detect_range.raw() <= 0 || !are_allied(d.player, player)) continue;
            if (power_disabled(d)) continue;
            const Fixed dx = d.x - target.x, dy = d.y - target.y;
            if (dx * dx + dy * dy <= d.detect_range * d.detect_range) return true;
        }
        return false;
    }
    Fixed move_step(Unit& u, Fixed dt, Fixed brake_dist = Fixed::from_raw(-1));
    static bool harvest_brake(bool drop);
    bool move_unit_toward(Unit& u, Fixed gx, Fixed gy, Fixed step);
    bool move_unit_toward_impl(Unit& u, Fixed gx, Fixed gy, Fixed step);
    bool advance_turn(Unit& u, Fixed tx, Fixed ty, Fixed fire_arc_deg = Fixed());
    bool advance_turret(Unit& u, Fixed tx, Fixed ty, Fixed fire_arc_deg);
    void apply_separation();
    void step_gather(Unit& u, Fixed dt);
    std::uint32_t free_patch_near(const Unit& u, const Unit& m) const;
    void unstick_bodies();
    void step_cast(Unit& u, Fixed dt);
    void execute_effect(EffectRef ref, const EffectContext& ctx, int depth = 0);
    static constexpr int kMaxEffectDepth = 32;
    static bool is_point_self(EffectKind k) {
        return k == EffectKind::Teleport || k == EffectKind::CreatePersistent ||
               k == EffectKind::CreateUnit || k == EffectKind::Morph ||
               k == EffectKind::Reveal ||
               k == EffectKind::EnumArea ||
               k == EffectKind::PhaseShift ||
               k == EffectKind::DestroyPersistent ||
               k == EffectKind::UseCalldown ||
               k == EffectKind::IssueOrder;
    }
    bool root_is_point_cast(EffectRef root, int depth = 0) const {
        if (depth > kMaxEffectDepth || root < 0 || root >= EffectRef(effects_.size())) return false;
        const EffectNode& n = effects_[root];
        if (is_point_self(n.kind)) return true;
        if (n.kind == EffectKind::LaunchMissile)
            return n.missile_impact_loc == 1 || n.missile_impact_loc == 3;
        if (n.kind == EffectKind::Set && !n.children.empty())
            return root_is_point_cast(n.children[0], depth + 1);
        return false;
    }
    void step_production(Fixed dt);
    void step_worker_builds();
    void step_construction(Unit& u, Fixed dt);
    static constexpr std::size_t kAllPersistents = std::size_t(-1);
    void step_persistents(Fixed dt, std::size_t only = kAllPersistents);
    bool apply_persistent_now_ = false;
    void step_healers(Fixed dt);
    void step_charge();
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
