#pragma once

#include <cstdint>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class WarpInSystem {
public:
    static constexpr std::uint16_t kWarpBehaviorKind = 0x7A9F;

    std::uint32_t warp(World& world, Unit proto, Fixed x, Fixed y, Fixed warp_time,
                       bool powered) {
        if (!powered) return kNoTarget;
        proto.x = x;
        proto.y = y;
        proto.alive = true;
        const std::uint32_t id = world.add_unit(proto);

        if (warp_time.raw() > 0) {
            BehaviorInstance b;
            b.kind = kWarpBehaviorKind;
            b.timer = warp_time;
            b.stun = true;
            world.add_behavior(id, b);
        }
        return id;
    }

    static bool is_warping(const World& world, std::uint32_t id) {
        if (id >= world.units().size()) return false;
        for (const BehaviorInstance& b : world.units()[id].behaviors)
            if (b.kind == kWarpBehaviorKind && b.timer.raw() > 0) return true;
        return false;
    }
};

}
