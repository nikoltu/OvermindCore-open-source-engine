#pragma once

#include <cstdint>
#include <unordered_set>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

class HallucinationSystem {
public:
    std::vector<std::uint32_t> create(World& world, std::uint32_t source, std::int32_t owner,
                                      int count, Fixed lifetime, Fixed spread = Fixed::from_int(1)) {
        std::vector<std::uint32_t> ids;
        if (source >= world.units().size() || count <= 0) return ids;
        const Unit proto = world.units()[source];

        static const int DX[8] = {1, 1, 0, -1, -1, -1, 0, 1};
        static const int DY[8] = {0, 1, 1, 1, 0, -1, -1, -1};
        for (int i = 0; i < count; ++i) {
            Unit h = proto;
            h.player = owner;
            const int k = i % 8;
            h.x = proto.x + spread * Fixed::from_int(DX[k]);
            h.y = proto.y + spread * Fixed::from_int(DY[k]);
            h.wpn_damage = Fixed();
            h.time_life = lifetime.raw() > 0 ? lifetime : Fixed::from_raw(1);
            h.orders.clear();
            h.target = kNoTarget;
            const std::uint32_t id = world.add_unit(h);
            ids.push_back(id);
            illusions_.insert(id);
        }
        return ids;
    }

    bool is_hallucination(std::uint32_t id) const { return illusions_.count(id) != 0; }
    std::size_t count() const { return illusions_.size(); }

    void prune(const World& world) {
        for (auto it = illusions_.begin(); it != illusions_.end();) {
            if (*it >= world.units().size() || !world.units()[*it].alive)
                it = illusions_.erase(it);
            else
                ++it;
        }
    }

private:
    std::unordered_set<std::uint32_t> illusions_;
};

}
