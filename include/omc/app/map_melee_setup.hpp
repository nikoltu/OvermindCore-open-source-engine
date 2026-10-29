#pragma once

#include <cmath>
#include <array>
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <utility>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

#include "omc/app/ability.hpp"
#include "omc/app/melee.hpp"
#include "omc/app/spawn.hpp"
#include "omc/data/catalog.hpp"
#include "omc/data/economy.hpp"
#include "omc/data/production.hpp"
#include "omc/data/upgrades.hpp"
#include "omc/formats/map_archive.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

struct MapMeleeStarts {
    std::vector<std::pair<int, int>> cells;
    int cx = 0, cy = 0;
    int s0x() const { return cells[0].first; }
    int s0y() const { return cells[0].second; }
    int s1x() const { return cells[1].first; }
    int s1y() const { return cells[1].second; }
};

inline MapMeleeStarts map_two_starts(int s0x, int s0y, int s1x, int s1y, int cx, int cy) {
    MapMeleeStarts st;
    st.cells = {{s0x, s0y}, {s1x, s1y}};
    st.cx = cx;
    st.cy = cy;
    return st;
}

inline std::pair<int, int> map_nearest_walkable(const formats::LoadedMap& map, int tx, int ty) {
    const int W = int(map.width), H = int(map.height);
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
}

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

    auto nearest_walkable = [&](int tx, int ty) -> std::pair<int, int> { return map_nearest_walkable(map, tx, ty); };

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
            return map_two_starts(A.first, A.second, B.first, B.second, (minx + maxx) / 2, (miny + maxy) / 2);
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
    return map_two_starts(s0x, s0y, s1x, s1y, (minx + maxx) / 2, (miny + maxy) / 2);
}

inline bool map_startless_halls(const formats::LoadedMap& map) {
    return std::getenv("OMC_STARTLESS_HALLS") != nullptr && map.objects.start_locations.size() < 2 &&
           map.info.has_playable_area();
}

inline bool map_startless_retail(const formats::LoadedMap& map) {
    return map.objects.start_locations.size() < 2 && !map_startless_halls(map) && std::getenv("OMC_STARTLESS_INVENTED_BASE") == nullptr;
}

inline MapMeleeStarts map_melee_starts(const formats::LoadedMap& map, std::uint64_t start_seed = 0, bool any_pair = true) {
    MapMeleeStarts st = map_melee_starts_corner_layout(map);
    if (map_startless_halls(map)) {
        st.cells[0].first = int(map.info.playable_x0) + 6;
        st.cells[0].second = int(map.info.playable_y0) + 6;
        st.cells[1].first = int(map.info.playable_x1) - 6;
        st.cells[1].second = int(map.info.playable_y1) - 6;
        const std::string spec = std::getenv("OMC_STARTLESS_HALLS");
        const std::size_t semi = spec.find(';');
        if (spec.find(',') != std::string::npos) {
            const std::string part[2] = {spec.substr(0, semi), semi == std::string::npos ? std::string("-") : spec.substr(semi + 1)};
            for (int side = 0; side < 2; ++side) {
                float x = 0.0f, y = 0.0f;
                if (std::sscanf(part[side].c_str(), "%f,%f", &x, &y) != 2) continue;
                st.cells[std::size_t(side)].first = int(std::floor(x));
                st.cells[std::size_t(side)].second = int(std::floor(y));
            }
        }
        return st;
    }
    if (std::getenv("OMC_START_SWAP")) std::swap(st.cells[0], st.cells[1]);
    if (start_seed == 0) return st;
    std::uint64_t z = start_seed + 0xD1B54A32D192ED03ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    const auto& sl = map.objects.start_locations;
    if (sl.size() > 2 && any_pair && std::getenv("OMC_START_FURTHEST_PAIR") == nullptr) {
        const std::uint64_t S = sl.size(), k = z % (S * (S - 1));
        const std::size_t a = std::size_t(k / (S - 1));
        std::size_t b = std::size_t(k % (S - 1));
        if (b >= a) ++b;
        st.cells[0] = map_nearest_walkable(map, int(std::floor(sl[a].first)), int(std::floor(sl[a].second)));
        st.cells[1] = map_nearest_walkable(map, int(std::floor(sl[b].first)), int(std::floor(sl[b].second)));
        return st;
    }
    if (z & 1u) std::swap(st.cells[0], st.cells[1]);
    return st;
}

inline bool map_melee_any_pair_rule(const formats::LoadedMap& map) {
    return map.objects.start_locations.size() > 2 && !map_startless_halls(map) && std::getenv("OMC_START_FURTHEST_PAIR") == nullptr;
}

inline MapMeleeStarts map_melee_starts(const formats::LoadedMap& map, const std::vector<int>& teams,
                                       std::uint64_t start_seed = 0) {
    const std::size_t n = teams.size();
    if (n <= 2) return map_melee_starts(map, start_seed);
    const auto& sl = map.objects.start_locations;
    if (sl.size() < n)
        throw std::invalid_argument("map_melee_starts: " + std::to_string(n) + " players on a map authoring " +
                                    std::to_string(sl.size()) + " start locations");
    if (sl.size() > 8)
        throw std::invalid_argument("map_melee_starts: more than eight authored start locations (" +
                                    std::to_string(sl.size()) + ")");
    MapMeleeStarts st = map_melee_starts_corner_layout(map);
    const std::size_t S = sl.size();
    std::vector<std::pair<int, int>> cell(S);
    for (std::size_t i = 0; i < S; ++i)
        cell[i] = map_nearest_walkable(map, int(std::floor(sl[i].first)), int(std::floor(sl[i].second)));
    std::vector<std::int64_t> dist(S * S, 0);
    for (std::size_t i = 0; i < S; ++i)
        for (std::size_t j = 0; j < S; ++j) {
            const double dx = double(cell[i].first - cell[j].first), dy = double(cell[i].second - cell[j].second);
            dist[i * S + j] = std::int64_t(std::llround(std::sqrt(dx * dx + dy * dy) * 1024.0));
        }
    std::vector<std::size_t> pick(n, 0);
    std::vector<char> used(S, 0);
    std::vector<std::vector<std::size_t>> tied;
    std::int64_t best = -1;
    auto walk = [&](auto&& self, std::size_t k, std::int64_t cost) -> void {
        if (best >= 0 && cost > best) return;
        if (k == n) {
            if (best < 0 || cost < best) {
                best = cost;
                tied.clear();
            }
            tied.push_back(pick);
            return;
        }
        for (std::size_t s = 0; s < S; ++s) {
            if (used[s]) continue;
            std::int64_t add = 0;
            for (std::size_t q = 0; q < k; ++q)
                if (teams[k] >= 0 && teams[q] == teams[k]) add += dist[pick[q] * S + s];
            used[s] = 1;
            pick[k] = s;
            self(self, k + 1, cost + add);
            used[s] = 0;
        }
    };
    walk(walk, 0, 0);
    std::size_t chosen = 0;
    if (start_seed != 0 && tied.size() > 1) {
        std::vector<std::size_t> order(tied.size());
        for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::uint64_t z = start_seed;
        auto next = [&z]() {
            z += 0x9E3779B97F4A7C15ull;
            std::uint64_t r = z;
            r = (r ^ (r >> 30)) * 0xBF58476D1CE4E5B9ull;
            r = (r ^ (r >> 27)) * 0x94D049BB133111EBull;
            return r ^ (r >> 31);
        };
        for (std::size_t i = order.size() - 1; i > 0; --i) std::swap(order[i], order[std::size_t(next() % (i + 1))]);
        chosen = order[0];
    }
    st.cells.clear();
    for (std::size_t p = 0; p < n; ++p) st.cells.push_back(cell[tied[chosen][p]]);
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
    s.players.push_back(map_melee_player(pr, st.s0x(), st.s0y(), st.cx, st.cy, worker_count, patches));
    s.players.push_back(map_melee_player(pr, st.s1x(), st.s1y(), st.cx, st.cy, worker_count, patches));
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
        const int sx = side ? st.s1x() : st.s0x(), sy = side ? st.s1y() : st.s0y();
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
        for (int t = 0; t < 5; ++t) p.wave_ladders[std::size_t(t)] = low_tier_wave_ladder(0, t);
        p.wave_phases = low_tier_wave_ladder(0, 2);
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
