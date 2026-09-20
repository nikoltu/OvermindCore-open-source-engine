#include <cmath>
#include "sc2/app/spawn.hpp"

#include <cstdlib>

#include <algorithm>
#include <map>
#include <vector>

#include "sc2/app/ability.hpp"
#include "sc2/data/abil_charge.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/sim/weapon.hpp"

namespace sc2::app {

using sim::Fixed;

static Fixed field(const data::Catalog& cat, const char* fam, const std::string& id,
                   const char* name, Fixed fallback = Fixed()) {
    auto s = cat.scalar(fam, id, name);
    return (s && !s->empty()) ? Fixed::parse(*s) : fallback;
}

static Fixed field_or_class_default(const data::Catalog& cat, const char* fam, const std::string& id, const char* name,
                                    Fixed fallback = Fixed()) {
    if (auto s = cat.scalar(fam, id, name); s && !s->empty()) return Fixed::parse(*s);
    if (const data::Catalog::Entry* e = cat.find(fam, id))
        if (auto d = cat.tag_default_scalar(e->tag, name); d && !d->empty()) return Fixed::parse(*d);
    return fallback;
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
        if (!idx) continue;
        const bool on = truthy(n.attr_or("value", "1"));
        if (*idx == "Air") air = on;
        else if (*idx == "Ground") ground = on;
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
    u.hp = field_or_class_default(cat, "CUnit", unit_id, "LifeMax");
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
    u.commandable = cat.unit_commandable(unit_id);
    u.train_morphs_self = cat.train_morph_unit(unit_id).has_value();
    u.fog_visibility = std::uint8_t(cat.unit_fog_visibility(unit_id));
    for (const std::string& abil : cat.array_links("CUnit", unit_id, "AbilArray")) {
        const data::Catalog::Entry* e = abil.empty() ? nullptr : cat.find("CAbil", abil);
        if (e && e->tag == "CAbilHarvest") {
            u.is_worker = true;
            if (auto ar = cat.scalar_num("CAbil", abil, "AcquireRadius"))
                u.acquire_radius = Fixed::from_double(*ar);
            else if (auto d = cat.tag_default_scalar(e->tag, "AcquireRadius"))
                u.acquire_radius = Fixed::from_double(std::strtod(d->c_str(), nullptr));
            break;
        }
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
    u.hall_trained = cat.trained_by_spawner(unit_id);
    if (const double dr = cat.death_off_creep_radius(unit_id); dr >= 0) u.death_off_creep = Fixed::from_double(dr);
    if (const auto oc = cat.off_creep_decay(unit_id); oc.period > 0) {
        u.offcreep_period = u.offcreep_timer = Fixed::from_double(oc.period);
        u.offcreep_amount = Fixed::from_double(oc.amount);
    }
    for (const std::string& ab : cat.array_links("CUnit", unit_id, "AbilArray")) {
        const data::Catalog::Entry* ae = ab.empty() ? nullptr : cat.find("CAbil", ab);
        if (!ae || ae->tag != "CAbilBuild") continue;
        for (const data::SlotCharge& sc : data::ability_charges(cat, ab)) {
            if (sc.charge.count_max <= 0) continue;
            const data::CooldownSpec cd = data::ability_cooldown(cat, ab, sc.index);
            u.build_charges = int32_t(sc.charge.count_start > 0 ? sc.charge.count_start : sc.charge.count_max);
            u.build_cd = Fixed::from_double(cd.time_start);
            u.build_cd_use = Fixed::from_double(cd.time_use);
            u.build_range = field(cat, "CAbil", ab, "Range");
            break;
        }
        if (u.build_charges >= 0) break;
    }
    u.load_range = Fixed::from_double(cat.transport_load_range(unit_id));
    u.player_hold = cat.transport_player_hold(unit_id);

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
        u.energy_regen = field_or_class_default(cat, "CUnit", unit_id, "EnergyRegenRate", Fixed::from_ratio(5625, 10000));
    }

    u.supply_provides = data::supply_provided(cat, unit_id);
    u.supply_uses = data::unit_cost(cat, unit_id).supply;
    u.repair_time = Fixed::from_double(cat.repair_time(unit_id).value_or(0.0));
    u.cost_minerals = data::unit_cost(cat, unit_id).minerals;
    u.cost_gas = data::unit_cost(cat, unit_id).gas;

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
            u.wpn_id = w;
            {
                const auto dmg_id = cat.weapon_damage_effect_id(w);
                const auto kd = cat.scalar("CEffect", dmg_id ? *dmg_id : cat.weapon_effect_id(w), "Kind");
                const std::string ks = kd ? *kd : std::string();
                const std::uint8_t kb = ks == "Melee" ? sim::kDamageKindMelee : ks == "Splash" ? sim::kDamageKindSplash : ks == "Spell" ? sim::kDamageKindSpell : sim::kDamageKindRanged;
                u.wpn_kind = kb;
            }
            u.wpn_damage = damage;
            u.wpn_period = period;
            u.wpn_range = range;
            u.wpn_min_range = min_range;
            u.wpn_target_filter = filter;
            weapon_attribute_bonus(cat, w, u.wpn_bonus_attr, u.wpn_bonus_amount);
            u.wpn_damage_point = field(cat, "CWeapon", w, "DamagePoint", Fixed::from_double(0.1670));
            u.wpn_backswing = field(cat, "CWeapon", w, "Backswing", Fixed::from_double(0.5));
            u.wpn_fire_arc = Fixed::from_double(cat.weapon_fire_arc_deg(w));
            const auto rd = cat.weapon_random_delay(w);
            u.wpn_random_delay_min = Fixed::from_double(rd.first);
            u.wpn_random_delay_max = Fixed::from_double(rd.second);
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
                const data::Catalog::SplashFilters sf = cat.weapon_splash_filters(w);
                u.splash_affiliation = sf.affiliation;
                u.splash_planes = sf.planes;
                u.splash_excludes_structure = sf.excludes_structure;
                u.splash_offset_by_radius = sf.offset_by_radius;
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
            u.wpn2_id = w;
            {
                const auto dmg_id = cat.weapon_damage_effect_id(w);
                const auto kd = cat.scalar("CEffect", dmg_id ? *dmg_id : cat.weapon_effect_id(w), "Kind");
                const std::string ks = kd ? *kd : std::string();
                const std::uint8_t kb = ks == "Melee" ? sim::kDamageKindMelee : ks == "Splash" ? sim::kDamageKindSplash : ks == "Spell" ? sim::kDamageKindSpell : sim::kDamageKindRanged;
                u.wpn2_kind = kb;
            }
            u.wpn2_damage = damage;
            u.wpn2_period = period;
            u.wpn2_range = range;
            u.wpn2_min_range = min_range;
            u.wpn2_target_filter = filter;
            weapon_attribute_bonus(cat, w, u.wpn2_bonus_attr, u.wpn2_bonus_amount);
            u.wpn2_damage_point = field(cat, "CWeapon", w, "DamagePoint", Fixed::from_double(0.1670));
            u.wpn2_backswing = field(cat, "CWeapon", w, "Backswing", Fixed::from_double(0.5));
            u.wpn2_fire_arc = Fixed::from_double(cat.weapon_fire_arc_deg(w));
            const auto rd2 = cat.weapon_random_delay(w);
            u.wpn2_random_delay_min = Fixed::from_double(rd2.first);
            u.wpn2_random_delay_max = Fixed::from_double(rd2.second);
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

    if (const std::string bv = cat.burrowed_variant(unit_id); !bv.empty()) {
        u.burrow_speed = field(cat, "CUnit", bv, "Speed");
        u.can_burrow_move = u.burrow_speed.raw() > 0;
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
                u.burrowed_wpn.fire_arc = bu.wpn_fire_arc;
                u.burrowed_wpn.random_delay_min = bu.wpn_random_delay_min;
                u.burrowed_wpn.random_delay_max = bu.wpn_random_delay_max;
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
    {
        const std::string air = cat.liftoff_target(unit_id);
        auto any_drop_off = [&](const std::string& id) {
            return !id.empty() && (is_resource_drop_off(cat, id, "Minerals") ||
                                   is_resource_drop_off(cat, id, "Vespene"));
        };
        u.drop_off_ground = any_drop_off(unit_id);
        u.drop_off_air = air.empty() ? u.drop_off_ground : any_drop_off(air);
        if (!air.empty()) u.speed_air = field(cat, "CUnit", air, "Speed", Fixed());
        if (!air.empty()) {
            const std::string land_ab = cat.morph_button_ability(air, "Land");
            if (!land_ab.empty()) {
                if (auto ls = cat.liftoff_spec(land_ab)) {
                    u.land_time = Fixed::from_double(ls->lock);
                    u.land_desc_delay = Fixed::from_double(ls->rise_delay);
                    u.land_desc = Fixed::from_double(ls->rise);
                } else if (auto ms = cat.morph_spec(land_ab)) {
                    u.land_time = Fixed::from_double(ms->morph_time);
                }
            }
        }
    }
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
        if (const std::string birth = cat.creep_source_footprint(unit_id, "Birth"); !birth.empty())
            u.creep_birth_radius = std::max(1, sim::mask_growth_radius(cat.footprint_place_offsets(birth)));
        const std::string start = cat.creep_source_footprint(unit_id, "Start");
        u.creep_starts_grown = !start.empty() && start == cat.creep_source_footprint(unit_id, "Grown");
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
    if (auto ccs = cat.creep_check_offsets(unit_id); !ccs.empty()) {
        u.creep_check_offsets.reserve(ccs.size());
        for (const auto& [dx, dy] : ccs) u.creep_check_offsets.emplace_back(int16_t(dx), int16_t(dy));
    }
    for (const auto& [dx, dy] : cat.near_resources_offsets(unit_id)) u.near_resources_offsets.emplace_back(int16_t(dx), int16_t(dy));
    for (const auto& [dx, dy] : cat.near_resources_check_offsets(unit_id))
        u.near_resources_check_offsets.emplace_back(int16_t(dx), int16_t(dy));
    u.requires_vespene = cat.structure_requires_vespene(unit_id);
    if (const double pr = cat.power_source_radius(unit_id); pr > 0)
        u.power_radius = Fixed::from_double(pr);
    u.needs_power = cat.requires_power(unit_id);
    u.turn_rate = field_or_class_default(cat, "CUnit", unit_id, "TurningRate");
    u.stationary_turn_rate =
        field_or_class_default(cat, "CUnit", unit_id, "StationaryTurningRate");
    {
        Fixed sep_default;
        if (auto d = cat.tag_default_scalar("CUnit", "SeparationRadius"); d && !d->empty())
            sep_default = Fixed::parse(*d);
        u.sep_radius = field(cat, "CUnit", unit_id, "SeparationRadius", sep_default);
    }
    u.inner_radius = field(cat, "CUnit", unit_id, "InnerRadius", u.radius);
    u.push_priority = int32_t(field(cat, "CUnit", unit_id, "PushPriority", Fixed::from_int(10)).round());
    u.allied_push_priority =
        int32_t(field(cat, "CUnit", unit_id, "AlliedPushPriority", Fixed()).round());
    u.collide_flags = read_collide(cat, unit_id);
    u.prevent_defeat = cat.unit_flag(unit_id, "PreventDefeat");
    u.catalog_id = unit_id;
    u.yield_amount = cat.resource_harvest_amount(unit_id);
    if (auto rd = cat.resource_return_delay(unit_id))
        u.return_delay = sim::Fixed::from_double(*rd);
    if (const int content = cat.resource_content(unit_id); content > 0) u.resource_left = content;
    if (auto rt = cat.scalar("CUnit", unit_id, "ResourceType"); rt && *rt == "Vespene") u.is_gas = true;
    if (u.resource_left > 0) {
        if (auto ideal = cat.resource_ideal_harvesters(unit_id)) u.miners_max = *ideal;
        else u.miners_max = u.is_gas ? 3 : 2;
    }
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
    for (const std::string& ab : cat.array_links("CUnit", unit_id, "AbilArray")) {
        const auto* qe = ab.empty() ? nullptr : cat.find("CAbil", ab);
        if (!qe || qe->tag != "CAbilQueue") continue;
        if (auto qs = cat.scalar("CAbil", ab, "QueueSize")) u.queue_size = std::atoi(qs->c_str());
        break;
    }
    if (u.queue_size > 0)
        for (const core::XmlNode& n : cat.resolved("CBehavior", "ReactorQueue"))
            if (n.name == "Modification")
                if (const std::string* qs = n.attr("QueueSize")) u.queue_bonus_reactor = std::atoi(qs->c_str());
    if (auto rsp = cat.scalar("CUnit", unit_id, "Response"); rsp && *rsp == "Flee") u.flees_when_attacked = true;
    if (auto ht = cat.resource_harvest_time(unit_id)) u.harvest_time = Fixed::from_double(*ht);
    for (const std::string& b : cat.array_links("CUnit", unit_id, "BehaviorArray")) {
        if (b.empty()) continue;
        const auto dr = cat.behavior_damage_response(b);
        if (dr.modify_limit > 0) u.barrier_max = Fixed::from_double(dr.modify_limit);
        if (dr.cooldown > 0) u.barrier_cd_max = Fixed::from_double(dr.cooldown);
        if (const data::Catalog::Entry* we = cat.find("CBehavior", b); we && we->tag == "CBehaviorWander") {
            if (auto r = cat.scalar_num("CBehavior", b, "Range"); r && *r > 0) u.wander_range = Fixed::from_double(*r);
            if (auto d = cat.scalar_num("CBehavior", b, "Delay"); d && *d > 0) u.wander_delay = Fixed::from_double(*d);
            u.wander_leash = cat.scalar("CBehavior", b, "Leash").value_or("0") == "1";
            u.wander_timer = u.wander_delay;
        }
        if (b == "TerranBuildingBurnDown") {
            u.burns_down = true;
            if (auto p = cat.scalar_num("CBehavior", b, "Period"); p && *p > 0) u.burn_period = Fixed::from_double(*p);
            if (const std::string eff = cat.scalar("CBehavior", b, "PeriodicEffect").value_or(""); !eff.empty())
                if (auto a = cat.effect_amount(eff); a && *a > 0) u.burn_amount = Fixed::from_double(*a);
        }
    }
    return u;
}

sim::BuildVerb worker_build_verb(const data::Catalog& cat, const std::string& worker_unit_id) {
    const data::Catalog::PeonBuildMode m = cat.worker_build_mode(worker_unit_id);
    if (!m.found) return sim::BuildVerb::Committed;
    if (m.consumed) return sim::BuildVerb::Morph;
    return m.maintained ? sim::BuildVerb::Committed : sim::BuildVerb::Warp;
}

}
