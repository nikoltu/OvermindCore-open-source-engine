#pragma once

#include <cstddef>
#include <string>

#include "sc2/formats/map_archive.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

template <typename StampFn>
std::size_t spawn_placed_units(sim::World& world, const formats::PlacedObjects& objs, StampFn stamp,
                               int mineral_amount = 1500, int gas_amount = 2250) {
    std::size_t n = 0;
    for (const formats::PlacedUnit& pu : objs.units) {
        sim::Unit u;
        u.x = sim::Fixed::from_ratio(int(pu.x * 64.0f), 64);
        u.y = sim::Fixed::from_ratio(int(pu.y * 64.0f), 64);
        u.alive = true;
        if (formats::is_resource_type(pu.unit_type)) {
            u.player = -1;
            const bool gas = pu.unit_type.find("Geyser") != std::string::npos ||
                             pu.unit_type.find("Vespene") != std::string::npos;
            u.resource_left = gas ? gas_amount : mineral_amount;
            u.radius = sim::Fixed::from_ratio(1, 1);
        } else {
            u.player = pu.player;
            u.hp = u.hp_max = sim::Fixed::from_int(1);
            stamp(pu.unit_type, u);
        }
        world.add_unit(u);
        ++n;
    }
    return n;
}

inline std::size_t spawn_placed_units(sim::World& world, const formats::PlacedObjects& objs,
                                      int mineral_amount = 1500, int gas_amount = 2250) {
    return spawn_placed_units(
        world, objs, [](const std::string&, sim::Unit&) {}, mineral_amount, gas_amount);
}

inline sim::World build_world(const formats::LoadedMap& map) {
    sim::World world;
    world.nav() = map.nav;
    spawn_placed_units(world, map.objects);
    return world;
}

template <typename StampFn>
inline sim::World build_world(const formats::LoadedMap& map, StampFn stamp) {
    sim::World world;
    world.nav() = map.nav;
    spawn_placed_units(world, map.objects, stamp);
    return world;
}

}
