#include <cmath>
#include "sc2/app/spawn.hpp"

#include <algorithm>
#include <map>
#include <vector>

#include "sc2/data/economy.hpp"
#include "sc2/sim/weapon.hpp"

namespace sc2::app {

using sim::Fixed;

static Fixed field(const data::Catalog& cat, const char* fam, const std::string& id,
                   const char* name, Fixed fallback = Fixed()) {
    auto s = cat.scalar(fam, id, name);
    return (s && !s->empty()) ? Fixed::parse(*s) : fallback;
}

static bool truthy(const std::string& v) {
    return !v.empty() && v != "0" && v != "false";
}

static uint32_t attribute_bit(const std::string& name) {
    using namespace sim;
    if (name == "Light") return kAttrLight;
    if (name == "Armored") return kAttrArmored;
    if (name == "Biological") return kAttrBiological;
    if (name == "Mechanical") return kAttrMechanical;
    if (name == "Massive") return kAttrMassive;
    if (name == "Psionic") return kAttrPsionic;
    if (name == "Structure") return kAttrStructure;
    return 0;
}

static double fold_attribute_bonus(const data::UpgradeCatalog& up,
                                   const std::vector<std::string>& researched,
                                   const std::string& effect, const std::string& attribute,
                                   double base) {
    double v = base;
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const data::UpgradeEffect& e : up.resolve(upg))
            if (e.catalog == "Effect" && e.entry == effect && e.field == "AttributeBonus" &&
                e.field_index == attribute)
                v = e.apply(v);
    }
    return v;
}

void weapon_attribute_bonus(const data::Catalog& cat, const std::string& weapon_id, uint32_t& bits,
                            Fixed& amount, const data::UpgradeCatalog* up,
                            const std::vector<std::string>* researched) {
    bits = 0;
    amount = Fixed();
    const auto eff = cat.weapon_damage_effect_id(weapon_id);
    double best = 0.0;
    for (const auto& [name, val] : cat.weapon_attribute_bonus(weapon_id)) {
        const uint32_t b = attribute_bit(name);
        if (b == 0) continue;
        double v = double(val);
        if (up && researched && eff) v = fold_attribute_bonus(*up, *researched, *eff, name, v);
        if (v <= best) continue;
        best = v;
        bits = b;
        amount = Fixed::from_double(v);
    }
}

static bool has_token(const std::string& list, const char* tok) {
    size_t start = 0;
    while (start <= list.size()) {
        size_t comma = list.find(',', start);
        size_t end = comma == std::string::npos ? list.size() : comma;
        if (list.compare(start, end - start, tok) == 0) return true;
        if (comma == std::string::npos) break;
        start = comma + 1;
    }
    return false;
}

static uint32_t parse_target_filter(const std::string& filters) {
    const size_t semi = filters.find(';');
    const std::string required = filters.substr(0, semi);
    const std::string excluded = semi == std::string::npos ? std::string() : filters.substr(semi + 1);

    uint32_t bits = 3;
    const bool req_ground = has_token(required, "Ground");
    const bool req_air = has_token(required, "Air");
    if (req_ground && !req_air) bits = 1;
    else if (req_air && !req_ground) bits = 2;
    if (has_token(excluded, "Ground")) bits &= ~1u;
    if (has_token(excluded, "Air")) bits &= ~2u;
    return bits == 0 ? 3u : bits;
}

static void read_attributes(const data::Catalog& cat, const std::string& unit_id,
                            sim::Unit& u) {
    for (const core::XmlNode& n : cat.resolved("CUnit", unit_id)) {
        if (n.name != "Attributes") continue;
        const std::string* idx = n.attr("index");
        if (!idx) continue;
        const std::string& val = n.attr_or("value", "1");
        const bool set = truthy(val);
        if (*idx == "Heroic") { u.heroic = set; continue; }
        if (set) u.attributes |= attribute_bit(*idx);
        else u.attributes &= ~attribute_bit(*idx);
    }
}

static bool read_flying(const data::Catalog& cat, const std::string& unit_id) {
    bool air = false, ground = false;
    for (const core::XmlNode& n : cat.resolved("CUnit", unit_id)) {
        if (n.name != "PlaneArray") continue;
        const std::string* idx = n.attr("index");
        if (!idx || !truthy(n.attr_or("value", "1"))) continue;
        if (*idx == "Air") air = true;
        else if (*idx == "Ground") ground = true;
    }
    return air && !ground;
}

static uint32_t collide_bit(const std::string& name) {
    using namespace sim;
    if (name == "Ground") return kCollideGround;
    if (name == "Small") return kCollideSmall;
    if (name == "Flying") return kCollideFlying;
    if (name == "FlyingEscorts") return kCollideFlyingEscorts;
    if (name == "Structure") return kCollideStructure;
    if (name == "ForceField") return kCollideForceField;
    if (name == "Locust") return kCollideLocust;
    if (name == "Larva") return kCollideLarva;
    if (name == "Colossus") return kCollideColossus;
    if (name == "Burrow") return kCollideBurrow;
    if (name == "RoachBurrow") return kCollideRoachBurrow;
    if (name == "Phased") return kCollidePhased;
    if (name == "CreepTumor") return kCollideCreepTumor;
    if (name == "DisruptorPhased") return kCollideDisruptorPhased;
    if (name == "LocustForceField") return kCollideLocustForceField;
    return 0;
}

static uint32_t read_collide(const data::Catalog& cat, const std::string& unit_id) {
    std::map<std::string, bool> final_val;
    for (const core::XmlNode& n : cat.resolved("CUnit", unit_id)) {
        if (n.name != "Collide") continue;
        const std::string* idx = n.attr("index");
        if (!idx) continue;
        final_val[*idx] = truthy(n.attr_or("value", "1"));
    }
    uint32_t flags = 0;
    for (const auto& [name, set] : final_val)
        if (set) flags |= collide_bit(name);
    return flags;
}

sim::Unit make_unit(const data::Catalog& cat, const std::string& unit_id,
                    int player, Fixed x, Fixed y) {
    sim::Unit u;
    u.player = player;
    u.x = x;
    u.y = y;
    u.hp = field(cat, "CUnit", unit_id, "LifeMax");
    u.hp_max = u.hp;
    u.shields_max = field(cat, "CUnit", unit_id, "ShieldsMax");
    u.shields = u.shields_max;
    if (u.shields_max.raw() > 0) {
        const auto sr = cat.shield_regen(unit_id);
        u.shield_regen_rate = Fixed::from_double(sr.rate);
        u.shield_regen_delay = Fixed::from_double(sr.delay);
    }
    for (const std::string& abil : cat.array_links("CUnit", unit_id, "AbilArray")) {
        if (abil.empty()) continue;
        if (auto d = cat.cloak_drain(abil)) { u.cloak_drain = Fixed::from_double(*d); break; }
    }
    for (const std::string& abil : cat.array_links("CUnit", unit_id, "AbilArray")) {
        const data::Catalog::Entry* e = abil.empty() ? nullptr : cat.find("CAbil", abil);
        if (e && e->tag == "CAbilHarvest") { u.is_worker = true; break; }
    }
    if (const double dr = cat.detector_range(unit_id); dr > 0) {
        u.detector = true;
        u.detect_range = Fixed::from_double(dr);
    }
    u.armor = field(cat, "CUnit", unit_id, "LifeArmor");
    u.speed = field(cat, "CUnit", unit_id, "Speed");
    u.accel = field(cat, "CUnit", unit_id, "Acceleration");
    u.decel = field(cat, "CUnit", unit_id, "Deceleration", u.accel);
    u.lateral_accel = field(cat, "CUnit", unit_id, "LateralAcceleration");
    u.radius = field(cat, "CUnit", unit_id, "Radius", Fixed::from_ratio(1, 2));
    u.radius = Fixed::from_double(cat.unit_body_radius(unit_id, u.radius.to_double()));
    u.cargo_size = int(field(cat, "CUnit", unit_id, "CargoSize").round());
    u.cargo_space = cat.transport_capacity(unit_id);

    u.sight = field(cat, "CUnit", unit_id, "Sight");
    u.death_reveal_radius = field(cat, "CUnit", unit_id, "DeathRevealRadius", Fixed::from_int(2));
    u.target_priority = int(field(cat, "CUnit", unit_id, "AttackTargetPriority", Fixed()).round());
    u.life_regen = field(cat, "CUnit", unit_id, "LifeRegenRate");
    for (const std::string& ab : cat.array_links("CUnit", unit_id, "AbilArray")) {
        if (ab.empty()) continue;
        if (auto ms = cat.morph_spec(ab); ms && !ms->target.empty()) {
            const Fixed r = field(cat, "CUnit", ms->target, "LifeRegenRate");
            if (r > u.life_regen && r > u.burrow_regen) u.burrow_regen = r;
        }
    }
    u.energy_max = field(cat, "CUnit", unit_id, "EnergyMax");
    if (u.energy_max.raw() > 0) {
        u.energy = field(cat, "CUnit", unit_id, "EnergyStart");
        u.energy_regen = field(cat, "CUnit", unit_id, "EnergyRegenRate",
                               Fixed::from_ratio(5625, 10000));
    }

    u.supply_provides = data::supply_provided(cat, unit_id);
    u.supply_uses = data::unit_cost(cat, unit_id).supply;

    u.build_progress = Fixed::from_int(1);

    int filled = 0;
    const std::string toggled_w = cat.unit_toggled_weapon(unit_id);
    for (const std::string& w : cat.equipped_weapons(unit_id,  !toggled_w.empty())) {
        auto dmg = cat.weapon_damage(w);
        if (!dmg) continue;
        const Fixed damage = Fixed::from_int(*dmg);
        const Fixed period = field(cat, "CWeapon", w, "Period", Fixed::from_double(0.8332));
        const Fixed range = field(cat, "CWeapon", w, "Range", Fixed::from_int(5));
        const Fixed min_range = field(cat, "CWeapon", w, "MinimumRange");
        uint32_t filter = 3;
        if (auto tf = cat.scalar("CWeapon", w, "TargetFilters"); tf && !tf->empty())
            filter = parse_target_filter(*tf);
        if (filled == 0) {
            u.wpn_damage = damage;
            u.wpn_period = period;
            u.wpn_range = range;
            u.wpn_min_range = min_range;
            u.wpn_target_filter = filter;
            weapon_attribute_bonus(cat, w, u.wpn_bonus_attr, u.wpn_bonus_amount);
            u.wpn_damage_point = field(cat, "CWeapon", w, "DamagePoint", Fixed::from_double(0.1670));
            u.wpn_backswing = field(cat, "CWeapon", w, "Backswing", Fixed::from_double(0.5));
            u.wpn_armor_reduction = Fixed::from_double(cat.weapon_armor_reduction(w));
            u.wpn_hits = cat.weapon_hit_count(w).value_or(1);
            if (auto ps = cat.missile_speed(w)) u.wpn_projectile_speed = Fixed::from_double(*ps);
            if (auto am = cat.scalar("CWeapon", w, "AllowedMovement"); am && *am == "Moving")
                u.fire_while_moving = true;
            const Fixed scan = field(cat, "CWeapon", w, "MinScanRange");
            u.acquire_range = scan.raw() > 0 ? scan : range;
            auto lsplash = cat.effect_persistent(cat.weapon_effect_id(w));
            auto arcs = cat.weapon_arc_splash(w);
            if (lsplash && lsplash->line_count >= 2 && lsplash->radius > 0) {
                u.splash_line_count = lsplash->line_count;
                u.splash_line_radius = Fixed::from_double(lsplash->radius);
                u.splash_line_from_source = lsplash->line_from_source;
                if (lsplash->line_from_source) {
                    const double a = std::fabs(lsplash->line_first), b = std::fabs(lsplash->line_last);
                    u.splash_line_first = Fixed::from_double(std::min(a, b));
                    u.splash_line_step = Fixed::from_double(std::fabs(lsplash->line_step));
                } else {
                    u.splash_line_first = Fixed::from_double(lsplash->line_first);
                    u.splash_line_step = Fixed::from_double(lsplash->line_step);
                }
            } else if (!arcs.empty()) {
                constexpr double kPi = 3.14159265358979323846;
                data::Catalog::ArcSplash src[2] = {};
                int band = 0;
                for (const data::Catalog::ArcSplash& a : arcs) {
                    if (band >= 2) break;
                    bool dup = false;
                    for (int b = 0; b < band; ++b)
                        if (src[b].arc_deg == a.arc_deg && src[b].radius == a.radius && src[b].frac == a.frac)
                            dup = true;
                    if (dup) continue;
                    src[band] = a;
                    u.arc_radius[band] = Fixed::from_double(a.radius);
                    u.arc_cos[band] = Fixed::from_double(std::cos(a.arc_deg * 0.5 * kPi / 180.0));
                    u.arc_frac[band] = Fixed::from_double(a.frac);
                    ++band;
                }
            } else if (auto rings = cat.weapon_splash(w); rings.size() >= 1) {
                std::sort(rings.begin(), rings.end(),
                          [](const auto& a, const auto& b) { return a.first < b.first; });
                const auto& lo = rings.front();
                const auto& hi = rings.back();
                const auto& mid = rings[rings.size() / 2];
                u.splash_inner = Fixed::from_double(lo.first);
                u.splash_outer = Fixed::from_double(hi.first);
                u.splash_mid = Fixed::from_double(mid.first);
                u.splash_inner_frac = Fixed::from_double(lo.second);
                u.splash_mid_frac = Fixed::from_double(mid.second);
                u.splash_outer_frac = Fixed::from_double(hi.second);
            }
            if (auto thr = cat.weapon_radius_split_no_splash(w))
                u.no_splash_radius = Fixed::from_double(*thr);
            if (auto bs = cat.weapon_bounce(w); bs && !bs->amounts.empty()) {
                const int nb = std::min<int>(int(bs->amounts.size()), sim::Unit::kMaxBounce);
                u.bounce_count = nb;
                u.bounce_range = Fixed::from_double(bs->range);
                for (int k = 0; k < nb; ++k) u.bounce_amount[k] = Fixed::from_double(bs->amounts[k]);
            }
        } else {
            u.wpn2_damage = damage;
            u.wpn2_period = period;
            u.wpn2_range = range;
            u.wpn2_min_range = min_range;
            u.wpn2_target_filter = filter;
            weapon_attribute_bonus(cat, w, u.wpn2_bonus_attr, u.wpn2_bonus_amount);
            u.wpn2_damage_point = field(cat, "CWeapon", w, "DamagePoint", Fixed::from_double(0.1670));
            u.wpn2_backswing = field(cat, "CWeapon", w, "Backswing", Fixed::from_double(0.5));
            u.wpn2_armor_reduction = Fixed::from_double(cat.weapon_armor_reduction(w));
            u.wpn2_hits = cat.weapon_hit_count(w).value_or(1);
            if (auto ps = cat.missile_speed(w)) u.wpn2_projectile_speed = Fixed::from_double(*ps);
            const Fixed scan2 = field(cat, "CWeapon", w, "MinScanRange");
            const Fixed acq2 = scan2.raw() > 0 ? scan2 : range;
            if (acq2 > u.acquire_range) u.acquire_range = acq2;
        }
        if (++filled == 2) break;
    }

    if (!toggled_w.empty() && filled == 1 && u.wpn2_damage.raw() == 0) {
        u.has_toggle_wpn = true;
        u.swap_weapon_block(u.toggle_wpn);
        for (const std::string& ab : cat.array_links("CUnit", unit_id, "AbilArray"))
            for (const std::string& beh : cat.array_links("CAbil", ab, "BehaviorArray"))
                if (cat.behavior_weapon_enable(beh) == toggled_w)
                    if (auto d = cat.cloak_drain(beh))
                        u.toggle_wpn_drain = Fixed::from_double(*d);
    }

    if (auto pd = cat.point_defense_of(unit_id)) {
        u.point_defense_radius = Fixed::from_double(pd->radius);
        u.point_defense_max = pd->max;
    }

    if (u.wpn_damage.raw() == 0 && u.wpn2_damage.raw() == 0) {
        const std::string bv = cat.burrowed_variant(unit_id);
        if (!bv.empty()) {
            sim::Unit bu = make_unit(cat, bv, player);
            if (bu.wpn_damage.raw() > 0) {
                u.has_burrowed_wpn = true;
                u.burrowed_wpn.damage = bu.wpn_damage;
                u.burrowed_wpn.period = bu.wpn_period;
                u.burrowed_wpn.range = bu.wpn_range;
                u.burrowed_wpn.min_range = bu.wpn_min_range;
                u.burrowed_wpn.damage_point = bu.wpn_damage_point;
                u.burrowed_wpn.projectile_speed = bu.wpn_projectile_speed;
                u.burrowed_wpn.backswing = bu.wpn_backswing;
                u.burrowed_wpn.bonus_amount = bu.wpn_bonus_amount;
                u.burrowed_wpn.acquire = bu.acquire_range;
                u.burrowed_wpn.hits = bu.wpn_hits;
                u.burrowed_wpn.target_filter = bu.wpn_target_filter;
                u.burrowed_wpn.bonus_attr = bu.wpn_bonus_attr;
                u.burrowed_wpn.splash_line_count = bu.splash_line_count;
                u.burrowed_wpn.splash_line_first = bu.splash_line_first;
                u.burrowed_wpn.splash_line_step = bu.splash_line_step;
                u.burrowed_wpn.splash_line_radius = bu.splash_line_radius;
                u.burrowed_wpn.splash_line_from_source = bu.splash_line_from_source;
            }
        }
    }

    read_attributes(cat, unit_id, u);
    u.flying = read_flying(cat, unit_id);
    u.speed_mult_creep = field(cat, "CUnit", unit_id, "SpeedMultiplierCreep", Fixed::from_int(1));
    if (auto mv = cat.scalar("CUnit", unit_id, "Mover"); mv && *mv == "CliffJumper") u.can_jump = true;
    if (auto mv = cat.scalar("CUnit", unit_id, "Mover"); mv && !mv->empty())
        if (auto pm = cat.scalar("CMover", *mv, "PathMode"); pm && *pm == "Scaler") u.walks_cliffs = true;
    if ((u.attributes & sim::kAttrMassive) && !u.flying) u.crushes_ff = true;
    if (const double cr = cat.creep_source_radius(unit_id); cr > 0) {
        u.creep_radius = int(cr);
        const double period_s = cat.creep_source_period(unit_id);
        u.creep_grow_period =
            period_s > 0 ? int(period_s * sim::World::kTicksPerSecond + 0.5) : 1;
    }
    if (const double tcr = cat.toggled_creep_radius(unit_id); tcr > 0) {
        u.has_toggle_creep = true;
        u.toggle_creep_radius = int(tcr);
        const double tp = cat.toggled_creep_period(unit_id);
        u.toggle_creep_period = tp > 0 ? int(tp * sim::World::kTicksPerSecond + 0.5) : 1;
    }

    if (auto offs = cat.unit_pathing_footprint_offsets(unit_id); !offs.empty()) {
        u.footprint_offsets.reserve(offs.size());
        for (const auto& [dx, dy] : offs)
            u.footprint_offsets.emplace_back(int16_t(dx), int16_t(dy));
    }
    u.requires_creep = cat.requires_creep(unit_id);
    u.requires_vespene = cat.structure_requires_vespene(unit_id);
    if (const double pr = cat.power_source_radius(unit_id); pr > 0)
        u.power_radius = Fixed::from_double(pr);
    u.needs_power = cat.requires_power(unit_id);
    u.turn_rate = field(cat, "CUnit", unit_id, "TurningRate");
    u.stationary_turn_rate = field(cat, "CUnit", unit_id, "StationaryTurningRate", Fixed());
    u.sep_radius = field(cat, "CUnit", unit_id, "SeparationRadius");
    u.inner_radius = field(cat, "CUnit", unit_id, "InnerRadius", u.radius);
    u.push_priority = int32_t(field(cat, "CUnit", unit_id, "PushPriority", Fixed::from_int(10)).round());
    u.allied_push_priority =
        int32_t(field(cat, "CUnit", unit_id, "AlliedPushPriority", Fixed()).round());
    u.collide_flags = read_collide(cat, unit_id);
    u.catalog_id = unit_id;
    u.yield_amount = cat.resource_harvest_amount(unit_id);
    if (const int content = cat.resource_content(unit_id); content > 0) u.resource_left = content;
    if (auto rt = cat.scalar("CUnit", unit_id, "ResourceType"); rt && *rt == "Vespene") u.is_gas = true;
    if (u.resource_left > 0) u.miners_max = u.is_gas ? 3 : 2;
    if (!cat.unit_spawn_behavior(unit_id)) {
        for (const std::string& ab : cat.array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            if (const auto* ce = cat.find("CAbil", ab);
                ce && (ce->tag == "CAbilTrain" || ce->tag == "CAbilWarpTrain")) {
                u.train_slots = 1;
                break;
            }
        }
    }
    if (auto rsp = cat.scalar("CUnit", unit_id, "Response"); rsp && *rsp == "Flee") u.flees_when_attacked = true;
    if (auto ht = cat.resource_harvest_time(unit_id)) u.harvest_time = Fixed::from_double(*ht);
    for (const std::string& b : cat.array_links("CUnit", unit_id, "BehaviorArray")) {
        if (b.empty()) continue;
        const auto dr = cat.behavior_damage_response(b);
        if (dr.modify_limit > 0) u.barrier_max = Fixed::from_double(dr.modify_limit);
        if (dr.cooldown > 0) u.barrier_cd_max = Fixed::from_double(dr.cooldown);
        if (b == "TerranBuildingBurnDown") u.burns_down = true;
    }
    return u;
}

}
