#pragma once

#include <cmath>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include <string>
#include <vector>

#include "sc2/app/ability.hpp"
#include "sc2/app/melee.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/data/production.hpp"
#include "sc2/data/upgrades.hpp"
#include "sc2/formats/map_archive.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

struct MapMeleeStarts {
    int s0x, s0y;
    int s1x, s1y;
    int cx, cy;
};

inline MapMeleeStarts map_melee_starts_corner_layout(const formats::LoadedMap& map) {
    const int W = int(map.width), H = int(map.height);
    int minx = W, miny = H, maxx = -1, maxy = -1;
    for (std::uint32_t y = 0; y < map.height; ++y)
        for (std::uint32_t x = 0; x < map.width; ++x)
            if (map.terrain.walkable(x, y)) {
                minx = std::min(minx, int(x));
                maxx = std::max(maxx, int(x));
                miny = std::min(miny, int(y));
                maxy = std::max(maxy, int(y));
            }
    if (maxx < 0) { minx = 0; miny = 0; maxx = W - 1; maxy = H - 1; }

    const int insetx = std::max(4, (maxx - minx) / 6);
    const int insety = std::max(4, (maxy - miny) / 6);

    auto nearest_walkable = [&](int tx, int ty) -> std::pair<int, int> {
        tx = std::clamp(tx, 0, W - 1);
        ty = std::clamp(ty, 0, H - 1);
        const int rmax = std::max(W, H);
        for (int r = 0; r <= rmax; ++r)
            for (int dy = -r; dy <= r; ++dy)
                for (int dx = -r; dx <= r; ++dx) {
                    if (std::max(std::abs(dx), std::abs(dy)) != r) continue;
                    int x = tx + dx, y = ty + dy;
                    if (x < 0 || y < 0 || x >= W || y >= H) continue;
                    if (map.terrain.walkable(std::uint32_t(x), std::uint32_t(y))) return {x, y};
                }
        return {tx, ty};
    };

    {
        const auto& sl = map.objects.start_locations;
        if (sl.size() >= 2) {
            std::size_t ai = 0, bi = 1;
            double bestD = -1.0;
            for (std::size_t i = 0; i < sl.size(); ++i)
                for (std::size_t j = i + 1; j < sl.size(); ++j) {
                    const double dx = double(sl[i].first) - sl[j].first,
                                 dy = double(sl[i].second) - sl[j].second;
                    const double d = dx * dx + dy * dy;
                    if (d > bestD) { bestD = d; ai = i; bi = j; }
                }
            auto near0 = [&](const std::pair<float, float>& p) {
                const double dx = double(p.first) - minx, dy = double(p.second) - miny;
                return dx * dx + dy * dy;
            };
            std::pair<float, float> a = sl[ai], b = sl[bi];
            if (near0(b) < near0(a)) std::swap(a, b);
            const auto A = nearest_walkable(int(std::floor(a.first)), int(std::floor(a.second)));
            const auto B = nearest_walkable(int(std::floor(b.first)), int(std::floor(b.second)));
            return {A.first, A.second, B.first, B.second, (minx + maxx) / 2, (miny + maxy) / 2};
        }
    }

    std::vector<std::array<double, 3>> cl;
    for (const auto& u : map.objects.units) {
        if (u.unit_type.find("Mineral") == std::string::npos) continue;
        int best = -1;
        for (std::size_t c = 0; c < cl.size(); ++c) {
            const double cx = cl[c][0] / cl[c][2], cy = cl[c][1] / cl[c][2];
            if (std::abs(u.x - cx) <= 10.0 && std::abs(u.y - cy) <= 10.0) { best = int(c); break; }
        }
        if (best < 0) cl.push_back({double(u.x), double(u.y), 1.0});
        else { cl[best][0] += u.x; cl[best][1] += u.y; cl[best][2] += 1.0; }
    }
    std::vector<std::pair<int, int>> mains;
    for (const auto& c : cl)
        if (c[2] >= 4.0) mains.push_back({int(c[0] / c[2] + 0.5), int(c[1] / c[2] + 0.5)});

    int s0x, s0y, s1x, s1y;
    if (mains.size() >= 2) {
        std::size_t ai = 0, bi = 1;
        double bestD = -1.0;
        for (std::size_t i = 0; i < mains.size(); ++i)
            for (std::size_t j = i + 1; j < mains.size(); ++j) {
                const double dx = mains[i].first - mains[j].first,
                             dy = mains[i].second - mains[j].second;
                const double d = dx * dx + dy * dy;
                if (d > bestD) { bestD = d; ai = i; bi = j; }
            }
        auto near0 = [&](std::pair<int, int> p) {
            const double dx = p.first - minx, dy = p.second - miny;
            return dx * dx + dy * dy;
        };
        std::pair<int, int> a = mains[ai], b = mains[bi];
        if (near0(b) < near0(a)) std::swap(a, b);
        std::tie(s0x, s0y) = nearest_walkable(a.first, a.second);
        std::tie(s1x, s1y) = nearest_walkable(b.first, b.second);
    } else {
        std::tie(s0x, s0y) = nearest_walkable(minx + insetx, miny + insety);
        std::tie(s1x, s1y) = nearest_walkable(maxx - insetx, maxy - insety);
    }
    return {s0x, s0y, s1x, s1y, (minx + maxx) / 2, (miny + maxy) / 2};
}

inline MapMeleeStarts map_melee_starts(const formats::LoadedMap& map, std::uint64_t start_seed = 0) {
    MapMeleeStarts st = map_melee_starts_corner_layout(map);
    if (start_seed == 0) return st;
    std::uint64_t z = start_seed + 0xD1B54A32D192ED03ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    if (z & 1u) {
        std::swap(st.s0x, st.s1x);
        std::swap(st.s0y, st.s1y);
    }
    return st;
}

struct MeleeProtos {
    float hall_facing = kPlacedStructureFacing;
    int start_minerals = 50;
    int food_ceiling = 200;
    int worker_count = kStartingWorkers;
    std::vector<std::pair<int, int>> worker_offsets;
    std::vector<MeleePlayerSetup::StartExtra> start_extras;
    sim::Unit hall, worker, mineral, producer, army, supply, geyser;
};

inline MeleePlayerSetup map_melee_player(const MeleeProtos& pr, int sx, int sy, int cx, int cy,
                                         int worker_count, int patches) {
    using sim::Fixed;
    auto cell = [](int c) { return Fixed::from_int(c) + Fixed::from_ratio(1, 2); };
    MeleePlayerSetup p;
    p.start = {cell(sx), cell(sy)};
    p.town_hall = pr.hall;
    p.town_hall_facing = pr.hall_facing;
    p.worker = pr.worker;
    p.worker_count = worker_count;
    p.mineral_field = pr.mineral;
    const int dirx = sx <= cx ? 1 : -1, diry = sy <= cy ? 1 : -1;
    for (int i = 0; i < patches; ++i)
        p.mineral_positions.push_back(
            {cell(sx + dirx * (3 + (i % 4))), cell(sy + diry * (i / 4))});
    p.geyser_field = pr.geyser;
    p.geyser_positions.push_back({cell(sx + dirx * 2), cell(sy - diry * 2)});
    p.geyser_positions.push_back({cell(sx + dirx * 4), cell(sy - diry * 2)});
    p.producer = pr.producer;
    p.army_unit = pr.army;
    p.supply_unit = pr.supply;
    p.start_minerals = pr.start_minerals;
    p.food_ceiling = pr.food_ceiling;
    p.worker_offsets = pr.worker_offsets;
    p.start_extras = pr.start_extras;
    return p;
}

inline MeleeSetup melee_setup_for_map(const formats::LoadedMap& map, int worker_count = kStartingWorkers,
                                      int patches = 8) {
    using sim::Fixed;
    MapMeleeStarts st = map_melee_starts(map);

    MeleeProtos pr;
    pr.hall.hp = pr.hall.hp_max = Fixed::from_int(1500);
    pr.hall.supply_provides = Fixed::from_int(15);
    pr.hall.radius = Fixed::from_int(2);

    pr.worker.hp = pr.worker.hp_max = Fixed::from_int(45);
    pr.worker.speed = Fixed::from_int(3);
    pr.worker.radius = Fixed::from_ratio(1, 2);
    pr.worker.carry_amount = 5;
    pr.worker.mine_time = Fixed::from_ratio(1, 2);
    pr.worker.supply_uses = Fixed::from_int(1);
    pr.worker.is_worker = true;

    pr.mineral.resource_left = 1600;
    pr.mineral.radius = Fixed::from_ratio(3, 4);

    pr.producer.hp = pr.producer.hp_max = Fixed::from_int(1000);
    pr.producer.radius = Fixed::from_ratio(3, 2);

    pr.supply.hp = pr.supply.hp_max = Fixed::from_int(400);
    pr.supply.supply_provides = Fixed::from_int(8);
    pr.supply.radius = Fixed::from_ratio(5, 4);

    pr.geyser.is_gas = true;
    pr.geyser.resource_left = 2000;
    pr.geyser.radius = Fixed::from_ratio(3, 2);

    pr.army.hp = pr.army.hp_max = Fixed::from_int(45);
    pr.army.wpn_damage = Fixed::from_int(6);
    pr.army.wpn_period = Fixed::from_ratio(55, 64);
    pr.army.wpn_range = Fixed::from_int(5);
    pr.army.speed = Fixed::from_int(3);
    pr.army.radius = Fixed::from_ratio(1, 2);
    pr.army.supply_uses = Fixed::from_int(1);

    MeleeSetup s;
    s.players.push_back(map_melee_player(pr, st.s0x, st.s0y, st.cx, st.cy, worker_count, patches));
    s.players.push_back(map_melee_player(pr, st.s1x, st.s1y, st.cx, st.cy, worker_count, patches));
    return s;
}

inline MeleeSetup melee_setup_for_map(const formats::LoadedMap& map, const data::Catalog& cat,
                                      int worker_count = -1, int patches = 8,
                                      const data::UpgradeCatalog* up = nullptr,
                                      const std::vector<std::string>* researched_p0 = nullptr,
                                      const std::vector<std::string>* researched_p1 = nullptr) {
    using sim::Fixed;
    MapMeleeStarts st = map_melee_starts(map);

    MeleeProtos pr;
    pr.hall = make_unit(cat, "CommandCenter", 0);
    pr.hall.speed = Fixed();
    pr.worker = make_unit(cat, "SCV", 0);
    pr.worker.is_worker = true;
    pr.mineral = make_unit(cat, "MineralField", -1);
    pr.mineral.player = -1;
    pr.mineral.speed = Fixed();
    pr.producer = make_unit(cat, "Barracks", 0);
    pr.producer.speed = Fixed();
    pr.supply = make_unit(cat, "SupplyDepot", 0);
    pr.supply.speed = Fixed();
    pr.geyser = make_unit(cat, "VespeneGeyser", -1);
    pr.geyser.player = -1;
    pr.geyser.speed = Fixed();
    pr.geyser.is_gas = true;
    pr.army = make_unit(cat, "Marine", 0);
    if (worker_count < 0) {
        const int authored = cat.race_starting_unit_count("Terr", "SCV");
        worker_count = authored > 0 ? authored : kStartingWorkers;
    }
    for (const data::Catalog::StartSlot& sl : cat.race_start_roster("Terr"))
        if (sl.unit == "SCV") pr.worker_offsets = sl.offsets;
    const data::UnitCost mcost = data::unit_cost(cat, "Marine");
    const data::UnitCost scost = data::unit_cost(cat, "SCV");
    const data::UnitCost dcost = data::unit_cost(cat, "SupplyDepot");
    const data::UnitCost bcost = data::unit_cost(cat, "Barracks");

    data::Production prod;
    prod.build(cat);
    auto secs = [&](const char* id, int fallback) {
        double t = prod.time_of(id);
        return t > 0.0 ? Fixed::from_raw(int(t * double(Fixed::kOne) + 0.5)) : Fixed::from_int(fallback);
    };

    MeleeSetup s;
    for (int side = 0; side < 2; ++side) {
        const int sx = side ? st.s1x : st.s0x, sy = side ? st.s1y : st.s0y;
        MeleePlayerSetup p = map_melee_player(pr, sx, sy, st.cx, st.cy, worker_count, patches);
        p.worker_mineral_cost = scost.minerals > 0 ? scost.minerals : 50;
        p.worker_build_time = secs("SCV", 12);
        p.supply_mineral_cost = dcost.minerals > 0 ? dcost.minerals : 100;
        p.supply_build_time = secs("SupplyDepot", 21);
        p.producer_mineral_cost = bcost.minerals > 0 ? bcost.minerals : 150;
        p.producer_gas_cost = bcost.gas;
        p.producer_build_time = secs("Barracks", 46);
        p.army_mineral_cost = mcost.minerals;
        p.army_gas_cost = mcost.gas;
        p.army_build_time = secs("Marine", 18);
        p.army_threshold = 5;
        p.wave_phases = {{10, 425}, {14, 336}, {18, 290}, {30, 279}};
        if (const std::vector<std::string>* r = side == 0 ? researched_p0 : researched_p1;
            up && r && !r->empty()) {
            apply_unit_upgrades(cat, *up, "Marine", *r, p.army_unit);
            apply_unit_upgrades(cat, *up, "SCV", *r, p.worker);
            apply_unit_upgrades(cat, *up, "Barracks", *r, p.producer);
            apply_unit_upgrades(cat, *up, "SupplyDepot", *r, p.supply_unit);
        }
        s.players.push_back(p);
    }
    return s;
}

}
