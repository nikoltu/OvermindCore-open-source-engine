#pragma once

#include <cmath>

#include "omc/data/catalog.hpp"
#include "omc/sim/world.hpp"

namespace omc::data {

inline sim::Fixed to_fixed(double v) {
    return sim::Fixed::from_raw(std::int32_t(std::llround(v * double(sim::Fixed::kOne))));
}

inline sim::Unit make_unit_proto(const Catalog::UnitStats& s) {
    sim::Unit u;
    u.hp = u.hp_max = to_fixed(s.life_max);
    u.armor = to_fixed(s.life_armor);
    if (s.shields_max > 0.0) {
        u.shields_max = to_fixed(s.shields_max);
        u.shields = u.shields_max;
    }
    u.speed = to_fixed(s.speed);
    if (s.radius) u.radius = to_fixed(*s.radius);
    if (!s.weapon_damage.empty()) u.wpn_damage = sim::Fixed::from_int(s.weapon_damage.front());
    if (s.weapon_period > 0.0) u.wpn_period = to_fixed(s.weapon_period);
    if (s.weapon_range > 0.0) u.wpn_range = to_fixed(s.weapon_range);
    return u;
}

inline sim::Unit make_unit_proto(const Catalog& catalog, const std::string& unit_id) {
    return make_unit_proto(catalog.unit_stats(unit_id));
}

}
