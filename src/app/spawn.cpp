#include "sc2/app/spawn.hpp"

#include <algorithm>
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
        if (!truthy(val)) continue;
        if (*idx == "Heroic") { u.heroic = true; continue; }
        u.attributes |= attribute_bit(*idx);
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
    u.radius = field(cat, "CUnit", unit_id, "Radius", Fixed::from_ratio(1, 2));

    u.sight = field(cat, "CUnit", unit_id, "Sight");
    u.life_regen = field(cat, "CUnit", unit_id, "LifeRegenRate");
    u.energy_max = field(cat, "CUnit", unit_id, "EnergyMax");
    if (u.energy_max.raw() > 0) {
        u.energy = field(cat, "CUnit", unit_id, "EnergyStart");
        u.energy_regen = field(cat, "CUnit", unit_id, "EnergyRegenRate",
                               Fixed::from_ratio(5625, 10000));
    }

    u.supply_provides = data::supply_provided(cat, unit_id);
    u.supply_uses = data::unit_cost(cat, unit_id).supply;

    int filled = 0;
    for (const std::string& w : cat.array_links("CUnit", unit_id, "WeaponArray")) {
        if (w.empty()) continue;
        auto dmg = cat.weapon_damage(w);
        if (!dmg) continue;
        const Fixed damage = Fixed::from_int(*dmg);
        const Fixed period = field(cat, "CWeapon", w, "Period", Fixed::from_int(1));
        const Fixed range = field(cat, "CWeapon", w, "Range", Fixed::from_int(5));
        uint32_t filter = 3;
        if (auto tf = cat.scalar("CWeapon", w, "TargetFilters"); tf && !tf->empty())
            filter = parse_target_filter(*tf);
        if (filled == 0) {
            u.wpn_damage = damage;
            u.wpn_period = period;
            u.wpn_range = range;
            u.wpn_target_filter = filter;
            u.wpn_damage_point = field(cat, "CWeapon", w, "DamagePoint");
            auto rings = cat.weapon_splash(w);
            if (rings.size() >= 2) {
                std::vector<double> radii;
                for (auto& [r, frac] : rings) { (void)frac; radii.push_back(r); }
                std::sort(radii.begin(), radii.end());
                u.splash_inner = Fixed::from_double(radii.front());
                u.splash_outer = Fixed::from_double(radii.back());
                u.splash_mid = Fixed::from_double(radii[radii.size() / 2]);
            }
        } else {
            u.wpn2_damage = damage;
            u.wpn2_period = period;
            u.wpn2_range = range;
            u.wpn2_target_filter = filter;
        }
        if (++filled == 2) break;
    }

    read_attributes(cat, unit_id, u);
    u.flying = read_flying(cat, unit_id);
    u.speed_mult_creep = field(cat, "CUnit", unit_id, "SpeedMultiplierCreep", Fixed::from_int(1));
    return u;
}

}
