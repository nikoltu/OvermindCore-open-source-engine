#include "sc2/app/spawn.hpp"

#include "sc2/data/economy.hpp"

namespace sc2::app {

using sim::Fixed;

static Fixed field(const data::Catalog& cat, const char* fam, const std::string& id,
                   const char* name, Fixed fallback = Fixed()) {
    auto s = cat.scalar(fam, id, name);
    return (s && !s->empty()) ? Fixed::parse(*s) : fallback;
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
            break;
        }
    }
    return u;
}

}
