#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sc2/sim/fixed.hpp"

namespace sc2::sim {

class World;
class VisionGrid;

struct RememberedStructure {
    std::uint32_t id = 0;
    std::int32_t player = -1;
    Fixed x, y;
    std::int32_t type = 0;
    int last_seen_tick = 0;
};

class FogMemory {
public:
    template <typename IsStructure, typename TypeOf>
    void update(const World& world, int32_t viewer, const VisionGrid& vision, int tick,
                IsStructure is_structure, TypeOf type_of);

    template <typename IsStructure>
    void update(const World& world, int32_t viewer, const VisionGrid& vision, int tick,
                IsStructure is_structure) {
        update(world, viewer, vision, tick, is_structure,
               [](const auto&) { return 0; });
    }

    const std::vector<RememberedStructure>& all() const { return mem_; }
    bool remembers(std::uint32_t id) const { return index_.find(id) != index_.end(); }
    std::size_t size() const { return mem_.size(); }
    void clear() {
        mem_.clear();
        index_.clear();
    }

private:
    std::vector<RememberedStructure> mem_;
    std::unordered_map<std::uint32_t, std::size_t> index_;
};

}

#include "sc2/sim/vision.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::sim {

template <typename IsStructure, typename TypeOf>
void FogMemory::update(const World& world, int32_t viewer, const VisionGrid& vision, int tick,
                       IsStructure is_structure, TypeOf type_of) {
    for (const Unit& u : world.units()) {
        if (!u.alive) continue;
        if (u.player == viewer) continue;
        if (!is_structure(u)) continue;
        if (!vision.is_visible(u.x, u.y)) continue;
        auto it = index_.find(u.id);
        RememberedStructure rs;
        rs.id = u.id;
        rs.player = u.player;
        rs.x = u.x;
        rs.y = u.y;
        rs.type = int32_t(type_of(u));
        rs.last_seen_tick = tick;
        if (it == index_.end()) {
            index_[u.id] = mem_.size();
            mem_.push_back(rs);
        } else {
            mem_[it->second] = rs;
        }
    }

    std::vector<RememberedStructure> kept;
    kept.reserve(mem_.size());
    for (const RememberedStructure& rs : mem_) {
        bool drop = false;
        if (vision.is_visible(rs.x, rs.y)) {
            const std::vector<Unit>& us = world.units();
            const bool still = rs.id < us.size() && us[rs.id].alive &&
                               us[rs.id].player == rs.player && is_structure(us[rs.id]) &&
                               vision.is_visible(us[rs.id].x, us[rs.id].y);
            if (!still) drop = true;
        }
        if (!drop) kept.push_back(rs);
    }
    mem_.swap(kept);
    index_.clear();
    for (std::size_t i = 0; i < mem_.size(); ++i) index_[mem_[i].id] = i;
}

}
