#pragma once

#include <cstdint>
#include <utility>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::ai {

inline int balance_workers(sim::World& world, const std::vector<std::uint32_t>& workers,
                           const std::vector<std::uint32_t>& nodes, int ideal_per_node = 2) {
    const std::size_t U = world.units().size();

    std::vector<int> count(nodes.size(), 0);
    std::vector<std::uint8_t> live(nodes.size(), 0);
    for (std::size_t i = 0; i < nodes.size(); ++i)
        live[i] = (nodes[i] < U && world.units()[nodes[i]].alive &&
                   world.units()[nodes[i]].resource_left > 0)
                      ? 1
                      : 0;

    auto node_slot = [&](std::uint32_t nid) -> int {
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (nodes[i] == nid) return int(i);
        return -1;
    };

    for (std::uint32_t wid : workers) {
        if (wid >= U) continue;
        const int slot = node_slot(world.units()[wid].gather_target);
        if (slot >= 0 && live[slot]) count[slot]++;
    }

    auto most_under = [&]() -> int {
        int best = -1, best_c = ideal_per_node;
        for (std::size_t i = 0; i < nodes.size(); ++i)
            if (live[i] && count[i] < best_c) {
                best_c = count[i];
                best = int(i);
            }
        return best;
    };

    int moved = 0;
    for (std::uint32_t wid : workers) {
        if (wid >= U || !world.units()[wid].alive) continue;
        const int cur = node_slot(world.units()[wid].gather_target);
        const bool over = cur >= 0 && live[cur] && count[cur] > ideal_per_node;
        const bool stranded = cur < 0 || !live[cur];
        if (!over && !stranded) continue;

        const int dst = most_under();
        if (dst < 0) break;
        if (cur >= 0 && live[cur]) count[cur]--;
        count[dst]++;
        world.unit(wid).gather_target = nodes[dst];
        sim::Order o;
        o.type = sim::OrderType::Gather;
        o.target = nodes[dst];
        world.issue(wid, o);
        ++moved;
    }
    return moved;
}

inline bool should_expand(int worker_count, int patches, int bank_minerals, int base_cost,
                          int ideal_per_patch = 2, double saturation_frac = 0.85) {
    if (patches <= 0) return bank_minerals >= base_cost;
    const double ideal = double(patches) * double(ideal_per_patch);
    const bool saturated = double(worker_count) >= saturation_frac * ideal;
    return saturated && bank_minerals >= base_cost;
}

inline int nearest_expansion(const std::vector<std::pair<sim::Fixed, sim::Fixed>>& candidates,
                             const std::vector<std::pair<sim::Fixed, sim::Fixed>>& bases,
                             sim::Fixed from_x, sim::Fixed from_y, sim::Fixed occupied_radius) {
    using sim::Fixed;
    const Fixed r2 = occupied_radius * occupied_radius;
    int best = -1;
    std::int64_t best_d2 = 0;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        bool taken = false;
        for (const auto& b : bases) {
            const Fixed dx = candidates[i].first - b.first, dy = candidates[i].second - b.second;
            if (dx * dx + dy * dy <= r2) { taken = true; break; }
        }
        if (taken) continue;
        const std::int64_t dx = std::int64_t((candidates[i].first - from_x).raw());
        const std::int64_t dy = std::int64_t((candidates[i].second - from_y).raw());
        const std::int64_t d2 = dx * dx + dy * dy;
        if (best < 0 || d2 < best_d2) {
            best = int(i);
            best_d2 = d2;
        }
    }
    return best;
}

inline sim::Fixed supply_cap() { return sim::Fixed::from_int(200); }

inline sim::Fixed supply_headroom(const sim::World& world, int32_t player) {
    const sim::Fixed room = world.supply_max(player) - world.supply_used(player);
    return room.raw() > 0 ? room : sim::Fixed();
}

inline bool needs_supply(const sim::World& world, int32_t player, sim::Fixed margin) {
    if (world.supply_max(player) >= supply_cap()) return false;
    return supply_headroom(world, player) <= margin;
}

}
