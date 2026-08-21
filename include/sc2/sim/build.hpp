#pragma once

#include <cstdint>

#include "sc2/sim/terrain_map.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::sim {

enum class BuildStyle { TerranStructure, ProtossWarp, ZergMorph };

inline bool can_place_structure(const World& world, const TerrainMap& terrain, int cx0,
                                int cy0, int fw, int fh, std::uint32_t builder = kNoTarget,
                                bool requires_vespene = false) {
    if (!terrain.can_place(cx0, cy0, fw, fh)) return false;
    bool found_gas = false;
    for (const Unit& u : world.units()) {
        if (!u.alive) continue;
        if (u.id == builder) continue;
        int ucx, ucy;
        terrain.to_cell(u.x, u.y, ucx, ucy);
        if (!(ucx >= cx0 && ucx < cx0 + fw && ucy >= cy0 && ucy < cy0 + fh)) continue;
        if (requires_vespene && u.is_gas && u.resource_left > 0) {
            found_gas = true;
            continue;
        }
        return false;
    }
    return !requires_vespene || found_gas;
}

struct BuildResult {
    std::uint32_t building = kNoTarget;
    bool ok() const { return building != kNoTarget; }
};

inline BuildResult begin_structure(World& world, const TerrainMap& terrain, Unit proto,
                                   BuildStyle style, int cx0, int cy0, int fw, int fh,
                                   std::uint32_t builder = kNoTarget, bool powered = true) {
    BuildResult r;
    if (!can_place_structure(world, terrain, cx0, cy0, fw, fh, builder, proto.requires_vespene))
        return r;
    if (style == BuildStyle::ProtossWarp && !powered) return r;

    Fixed lx, ly, hx, hy;
    terrain.cell_center(cx0, cy0, lx, ly);
    terrain.cell_center(cx0 + fw - 1, cy0 + fh - 1, hx, hy);
    proto.x = Fixed::from_raw((lx.raw() + hx.raw()) / 2);
    proto.y = Fixed::from_raw((ly.raw() + hy.raw()) / 2);
    proto.speed = Fixed();

    const std::uint32_t id = world.add_unit(proto);

    Fixed start_hp = Fixed::from_raw(proto.hp_max.raw() / 10);
    if (start_hp.raw() <= 0) start_hp = Fixed::from_int(1);
    Fixed build_time = proto.build_time.raw() > 0 ? proto.build_time : Fixed::from_int(1);
    world.start_construction(id, build_time, start_hp);

    if (style == BuildStyle::ZergMorph && builder != kNoTarget)
        world.unit(builder).alive = false;

    r.building = id;
    return r;
}

}
