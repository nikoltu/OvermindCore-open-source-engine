#pragma once

#include <cstdint>

#include "omc/sim/world.hpp"

namespace omc::sim {

struct ModeProfile {
    Fixed wpn_damage;
    Fixed wpn_period;
    Fixed wpn_range;
    Fixed speed;
    Fixed armor;
    int32_t type_id = 0;
};

inline ModeProfile capture_mode(const Unit& u) {
    ModeProfile m;
    m.wpn_damage = u.wpn_damage;
    m.wpn_period = u.wpn_period;
    m.wpn_range = u.wpn_range;
    m.speed = u.speed;
    m.armor = u.armor;
    m.type_id = u.type_id;
    return m;
}

inline void apply_mode(Unit& u, const ModeProfile& m) {
    u.wpn_damage = m.wpn_damage;
    u.wpn_period = m.wpn_period;
    u.wpn_range = m.wpn_range;
    u.speed = m.speed;
    u.armor = m.armor;
    u.type_id = m.type_id;
    if (m.speed.raw() == 0) u.orders.clear();
}

}
