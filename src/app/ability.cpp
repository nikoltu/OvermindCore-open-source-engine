#include "sc2/app/ability.hpp"

#include <algorithm>
#include <functional>
#include <utility>

#include "sc2/app/spawn.hpp"
#include "sc2/core/xml.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/sim/weapon.hpp"

namespace sc2::app {

using sim::Fixed;

static std::uint32_t filter_attr_bit(const std::string& tok) {
    if (tok == "Light") return sim::kAttrLight;
    if (tok == "Armored") return sim::kAttrArmored;
    if (tok == "Biological") return sim::kAttrBiological;
    if (tok == "Mechanical") return sim::kAttrMechanical;
    if (tok == "Massive") return sim::kAttrMassive;
    if (tok == "Psionic") return sim::kAttrPsionic;
    if (tok == "Structure") return sim::kAttrStructure;
    return 0;
}

static void parse_ability_filters(const data::Catalog& cat, const std::string& abil_id,
                                  AbilityBuild& b) {
    std::string filters;
    for (const core::XmlNode& c : cat.resolved("CAbil", abil_id))
        if (c.name == "TargetFilters")
            if (const std::string* v = c.attr("value"); v && !v->empty()) filters = *v;
    if (filters.empty()) return;
    const std::size_t semi = filters.find(';');
    const std::string req = filters.substr(0, semi);
    const std::string exc = semi == std::string::npos ? "" : filters.substr(semi + 1);
    auto each = [](const std::string& list, const std::function<void(const std::string&)>& fn) {
        std::size_t s = 0;
        while (s <= list.size()) {
            std::size_t c = list.find(',', s);
            if (c == std::string::npos) c = list.size();
            if (c > s) fn(list.substr(s, c - s));
            s = c + 1;
        }
    };
    each(req, [&](const std::string& t) {
        if (t == "HasEnergy") b.require_energy = true;
        else b.req_attr_mask |= filter_attr_bit(t);
    });
    each(exc, [&](const std::string& t) { b.exc_attr_mask |= filter_attr_bit(t); });
}

static Fixed cabil_energy(const data::Catalog& cat, const std::string& id) {
    for (const core::XmlNode& c : cat.resolved("CAbil", id)) {
        if (c.name != "Cost") continue;
        std::string v = c.attr_or("Energy", "");
        if (v.empty())
            for (const core::XmlNode& g : c.children)
                if (g.name == "Vital" && g.attr_or("index", "") == "Energy")
                    v = g.attr_or("value", "");
        if (!v.empty()) return Fixed::parse(v);
    }
    return Fixed();
}

static std::string cabil_effect(const data::Catalog& cat, const std::string& id) {
    std::string eff;
    for (const core::XmlNode& c : cat.resolved("CAbil", id)) {
        if (c.name == "Effect") {
            std::string v = c.attr_or("value", "");
            if (!v.empty()) eff = v;
        }
    }
    if (auto s = cat.scalar("CAbil", id, "Effect")) {
        if (!s->empty()) eff = *s;
    }
    return eff;
}

double weapon_upgrade_bonus(const data::Catalog& cat, const data::UpgradeCatalog& up,
                            const std::string& weapon_id,
                            const std::vector<std::string>& researched) {
    const auto eff = cat.weapon_damage_effect_id(weapon_id);
    if (!eff) return 0.0;
    double bonus = 0.0;
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const data::UpgradeEffect& e : up.resolve(upg))
            if (e.catalog == "Effect" && e.entry == *eff && e.field == "Amount")
                bonus += e.delta();
    }
    return bonus;
}

double armor_upgrade_bonus(const data::UpgradeCatalog& up, const std::string& unit_name,
                           const std::vector<std::string>& researched) {
    double bonus = 0.0;
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const data::UpgradeEffect& e : up.resolve(upg))
            if (e.catalog == "Unit" && e.entry == unit_name && e.field == "LifeArmor")
                bonus += e.delta();
    }
    return bonus;
}

void apply_unit_upgrades(const data::Catalog& cat, const data::UpgradeCatalog& up,
                         const std::string& unit_name, const std::vector<std::string>& researched,
                         sim::Unit& u) {
    if (researched.empty()) return;
    const auto wpns = cat.array_links("CUnit", unit_name, "WeaponArray");
    if (!wpns.empty()) {
        const double dmg = weapon_upgrade_bonus(cat, up, wpns[0], researched);
        u.wpn_damage = u.wpn_damage + Fixed::from_double(dmg);
    }
    const double armor = armor_upgrade_bonus(up, unit_name, researched);
    u.armor = u.armor + Fixed::from_double(armor);
    double speed = 0.0, range = 0.0, shield_armor = 0.0, life_max = 0.0, rate_mult = 0.0;
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const data::UpgradeEffect& e : up.resolve(upg)) {
            if (e.catalog == "Unit" && e.entry == unit_name && e.field == "Speed") {
                speed += e.delta();
            } else if (e.catalog == "Unit" && e.entry == unit_name && e.field == "ShieldArmor") {
                shield_armor += e.delta();
            } else if (e.catalog == "Unit" && e.entry == unit_name && e.field == "LifeMax") {
                life_max += e.delta();
            } else if (e.catalog == "Weapon" && (e.field == "Range" || e.field == "RateMultiplier")) {
                for (const std::string& wl : wpns)
                    if (!wl.empty() && wl == e.entry) {
                        if (e.field == "Range") range += e.delta();
                        else rate_mult = e.delta();
                        break;
                    }
            }
        }
    }
    if (speed != 0.0) u.speed = u.speed + Fixed::from_double(speed);
    if (range != 0.0) u.wpn_range = u.wpn_range + Fixed::from_double(range);
    if (shield_armor != 0.0) u.shield_armor = u.shield_armor + Fixed::from_double(shield_armor);
    if (life_max != 0.0) {
        u.hp_max = u.hp_max + Fixed::from_double(life_max);
        u.hp = u.hp + Fixed::from_double(life_max);
    }
    if (rate_mult != 0.0 && u.wpn_period.raw() > 0)
        u.wpn_period = u.wpn_period / Fixed::from_double(1.0 + rate_mult);
}

static void parse_ability_validators(const data::Catalog& cat, const std::string& eff,
                                     AbilityBuild& b) {
    if (eff.empty()) return;
    for (const std::string& vid : cat.array_links("CEffect", eff, "ValidatorArray")) {
        if (vid.empty()) continue;
        const data::Catalog::Entry* ve = cat.find("CValidator", vid);
        if (!ve || ve->tag != "CValidatorUnitCompareVital") continue;
        sim::VitalGate g;
        const std::string vital = cat.scalar("CValidator", vid, "Vital").value_or("Life");
        g.vital = vital == "Energy" ? sim::VitalGate::Energy
                  : vital == "Shields" ? sim::VitalGate::Shields
                                       : sim::VitalGate::Life;
        const std::string cmp = cat.scalar("CValidator", vid, "Compare").value_or("LT");
        g.cmp = cmp == "LE"   ? sim::VitalGate::LE
                : cmp == "GT" ? sim::VitalGate::GT
                : cmp == "GE" ? sim::VitalGate::GE
                : cmp == "EQ" ? sim::VitalGate::EQ
                : cmp == "NE" ? sim::VitalGate::NE
                              : sim::VitalGate::LT;
        g.value = Fixed::parse(cat.scalar("CValidator", vid, "Value").value_or("0"));
        g.fraction = cat.scalar("CValidator", vid, "Type").value_or("") == "Fraction";
        b.vital_gates.push_back(g);
    }
}

std::optional<AbilityBuild> make_ability(const data::Catalog& cat, const std::string& abil_id) {
    if (auto m = cat.morph_spec(abil_id)) {
        AbilityBuild b;
        b.def.kind = sim::EffectKind::Morph;
        b.def.duration = Fixed::from_double(m->morph_time);
        b.spawn_unit = m->target;
        if (!m->finish_effect.empty()) {
            if (auto lf = cat.effect_vital_fraction(m->finish_effect, "Life"); lf && *lf > 0)
                b.def.morph_heal_life_frac = Fixed::from_double(*lf);
            if (auto sf = cat.effect_vital_fraction(m->finish_effect, "Shields"); sf && *sf > 0)
                b.def.morph_heal_shield_frac = Fixed::from_double(*sf);
        }
        b.energy_cost = cabil_energy(cat, abil_id);
        if (auto cd = cat.scalar("CAbil", abil_id, "Cooldown"); cd && !cd->empty())
            b.cooldown = Fixed::parse(*cd);
        return b;
    }

    std::string eff = cabil_effect(cat, abil_id);
    if (eff.empty()) eff = abil_id;

    AbilityBuild b;
    if (auto per = cat.effect_persistent(eff)) {
        b.def.kind = sim::EffectKind::CreatePersistent;
        b.def.amount = Fixed::from_double(per->amount);
        b.def.radius = Fixed::from_double(per->radius);
        b.def.period = Fixed::from_double(per->period);
        b.def.period_count = per->count;
    } else if (auto dmg = cat.effect_damage(eff)) {
        b.def.kind = sim::EffectKind::Damage;
        b.def.amount = Fixed::from_int(*dmg);
    } else if (auto frac = cat.effect_energy_fraction(eff); frac && *frac > 0) {
        b.def.kind = sim::EffectKind::ModifyEnergy;
        b.def.energy_fraction = Fixed::from_double(*frac);
    } else if (auto heal = cat.effect_life_change(eff); heal && *heal > 0) {
        b.def.kind = sim::EffectKind::Heal;
        b.def.amount = Fixed::from_double(*heal);
    } else if (auto beh = cat.effect_apply_behavior(eff)) {
        auto bb = cat.behavior_buff(*beh);
        if (!bb) return std::nullopt;
        b.def.kind = sim::EffectKind::ApplyBehavior;
        b.def.duration = Fixed::from_double(bb->duration);
        b.def.move_speed_mult = Fixed::from_double(bb->move_speed_mult);
        b.def.attack_speed_mult = Fixed::from_double(bb->attack_speed_mult);
        b.def.beh_root = bb->root;
        b.def.beh_stun = bb->stun;
        b.def.beh_dmg_per_tick = Fixed::from_double(bb->dmg_per_tick);
        b.def.beh_dmg_period = Fixed::from_double(bb->dmg_period);
    } else if (auto cu = cat.effect_create_unit(eff)) {
        b.def.kind = sim::EffectKind::CreateUnit;
        b.def.spawn_count = cu->count;
        b.spawn_unit = cu->unit;
        if (!cu->spawn_effect.empty())
            if (auto beh = cat.effect_apply_behavior(cu->spawn_effect))
                if (auto bb = cat.behavior_buff(*beh))
                    b.spawn_life = Fixed::from_double(bb->duration);
    } else {
        return std::nullopt;
    }
    if (auto r = cat.scalar("CAbil", abil_id, "Range"); r && !r->empty())
        b.def.range = Fixed::parse(*r);
    b.energy_cost = cabil_energy(cat, abil_id);
    if (auto cd = cat.scalar("CAbil", abil_id, "Cooldown"); cd && !cd->empty())
        b.cooldown = Fixed::parse(*cd);
    parse_ability_filters(cat, abil_id, b);
    parse_ability_validators(cat, eff, b);
    return b;
}

std::pair<int, int> morph_cost_delta(const data::Catalog& cat, const std::string& source_unit,
                                     const std::string& target_unit) {
    const data::UnitCost s = data::unit_cost(cat, source_unit);
    const data::UnitCost t = data::unit_cost(cat, target_unit);
    return {std::max(0, t.minerals - s.minerals), std::max(0, t.gas - s.gas)};
}

sim::Unit make_unit_upgraded(const data::Catalog& cat, const data::UpgradeCatalog& up,
                             const std::string& unit_id, int player,
                             const std::vector<std::string>& researched) {
    sim::Unit u = make_unit(cat, unit_id, player);
    apply_unit_upgrades(cat, up, unit_id, researched, u);
    return u;
}

std::uint32_t calldown_mule(sim::World& world, sim::MuleSystem& mules, std::uint32_t caster,
                            sim::Fixed tx, sim::Fixed ty) {
    using sim::Fixed;
    const auto& us = world.units();
    if (caster >= us.size() || !us[caster].alive) return sim::kNoTarget;
    const Fixed cost = Fixed::from_int(50);
    if (us[caster].energy_max.raw() <= 0 || us[caster].energy < cost) return sim::kNoTarget;
    std::uint32_t node = sim::kNoTarget;
    Fixed best;
    for (std::uint32_t i = 0; i < us.size(); ++i) {
        const sim::Unit& n = us[i];
        if (!n.alive || n.resource_left <= 0 || n.is_gas) continue;
        const Fixed dx = n.x - tx, dy = n.y - ty;
        const Fixed d2 = dx * dx + dy * dy;
        if (node == sim::kNoTarget || d2 < best) { best = d2; node = i; }
    }
    if (node == sim::kNoTarget) return sim::kNoTarget;
    const std::int32_t player = us[caster].player;
    const Fixed nx = us[node].x, ny = us[node].y;
    world.unit(caster).energy = world.unit(caster).energy - cost;
    return mules.calldown(world, player, nx, ny, node, caster,
                          Fixed::from_int(sim::MuleSystem::kLifetimeSeconds));
}

bool order_repair(const data::Catalog& cat, sim::World& world, sim::RepairSystem& repairs,
                  std::uint32_t repairer, std::uint32_t target, const std::string& target_unit_id) {
    using sim::Fixed;
    const auto& us = world.units();
    if (repairer >= us.size() || target >= us.size() || repairer == target) return false;
    const sim::Unit& r = us[repairer];
    const sim::Unit& t = us[target];
    if (!r.alive || !t.alive) return false;
    if (!r.is_worker) return false;
    if (!world.are_allied(r.player, t.player)) return false;
    if (!(t.attributes & sim::kAttrMechanical)) return false;
    if (t.constructing) return false;
    if (t.hp >= t.hp_max) return false;
    const auto rt = cat.repair_time(target_unit_id);
    if (!rt || *rt <= 0.0) return false;
    const double hpmax = t.hp_max.to_double();
    if (hpmax <= 0.0) return false;
    const data::UnitCost cost = data::unit_cost(cat, target_unit_id);
    const Fixed rate = Fixed::from_double(hpmax / *rt);
    const Fixed min_per_hp = Fixed::from_double(double(cost.minerals) / hpmax);
    const Fixed gas_per_hp = Fixed::from_double(double(cost.gas) / hpmax);
    repairs.assign(repairer, target, rate, min_per_hp, gas_per_hp);
    return true;
}

int add_sensor_tower(const data::Catalog& cat, sim::SensorTowerSystem& towers, std::int32_t owner,
                     sim::Fixed x, sim::Fixed y, const std::string& unit_id) {
    const double r = cat.radar_range(unit_id);
    if (r <= 0.0) return -1;
    return towers.add(owner, x, y, sim::Fixed::from_double(r));
}

bool calldown_supply_drop(sim::World& world, sim::SupplyDropSystem& drops, std::uint32_t caster,
                          std::uint32_t depot) {
    using sim::Fixed;
    const auto& us = world.units();
    if (caster >= us.size() || depot >= us.size()) return false;
    if (!us[caster].alive || !us[depot].alive) return false;
    if (!world.are_allied(us[caster].player, us[depot].player)) return false;
    if (!(us[depot].attributes & sim::kAttrStructure)) return false;
    if (us[depot].supply_provides.raw() <= 0) return false;
    const Fixed cost = Fixed::from_int(50);
    if (us[caster].energy_max.raw() <= 0 || us[caster].energy < cost) return false;
    world.unit(caster).energy = world.unit(caster).energy - cost;
    drops.queue(depot, Fixed::from_double(sim::SupplyDropSystem::kEnRouteDelay));
    return true;
}

bool cast_abduct(sim::World& world, std::uint32_t caster, std::uint32_t target) {
    using sim::Fixed;
    const auto& us = world.units();
    if (caster >= us.size() || target >= us.size() || caster == target) return false;
    if (!us[caster].alive || !us[target].alive) return false;
    if (us[target].player < 0) return false;
    if (us[target].attributes & sim::kAttrStructure) return false;
    const Fixed cost = Fixed::from_int(75);
    if (us[caster].energy_max.raw() <= 0 || us[caster].energy < cost) return false;
    const Fixed dx = us[target].x - us[caster].x, dy = us[target].y - us[caster].y;
    const Fixed r = Fixed::from_int(9);
    if (dx * dx + dy * dy > r * r) return false;
    world.unit(caster).energy = world.unit(caster).energy - cost;
    world.unit(target).x = us[caster].x;
    world.unit(target).y = us[caster].y;
    return true;
}

}
