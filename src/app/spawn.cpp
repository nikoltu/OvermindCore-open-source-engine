#include "sc2/app/spawn.hpp"

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
    u.shields = field(cat, "CUnit", unit_id, "ShieldsMax");
    u.armor = field(cat, "CUnit", unit_id, "LifeArmor");
    u.speed = field(cat, "CUnit", unit_id, "Speed");
    u.radius = field(cat, "CUnit", unit_id, "Radius", Fixed::from_ratio(1, 2));

    u.supply_provides = data::supply_provided(cat, unit_id);
    u.supply_uses = data::unit_cost(cat, unit_id).supply;

    auto weapons = cat.array_links("CUnit", unit_id, "WeaponArray");
    for (const std::string& w : weapons) {
        if (w.empty()) continue;
        if (auto dmg = cat.weapon_damage(w)) {
            u.wpn_damage = Fixed::from_int(*dmg);
            u.wpn_period = field(cat, "CWeapon", w, "Period", Fixed::from_int(1));
            u.wpn_range = field(cat, "CWeapon", w, "Range", Fixed::from_int(1));
            if (auto tf = cat.scalar("CWeapon", w, "TargetFilters"); tf && !tf->empty())
                u.wpn_target_filter = parse_target_filter(*tf);
            break;
        }
    }

    read_attributes(cat, unit_id, u);
    u.flying = read_flying(cat, unit_id);
    return u;
}

}
