#pragma once
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <vector>

#include "sc2/app/melee_models.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/formats/map_archive.hpp"

namespace sc2::app {

inline std::size_t flatten_adjusted_terrain(formats::Model& ground, casc::Storage& store, MeleeModels& models,
                                            const formats::LoadedMap& map, float cell = 1.0f) {
    if (map.cliff.empty() || map.heightmap.width == 0 || ground.positions.empty() || cell <= 0.0f) return 0;
    const std::uint32_t W = map.cliff.width, H = map.cliff.height, VW = W + 1;
    if (std::getenv("SC2_NO_ADJUST_TERRAIN")) return 0;
    const float off = formats::engine_z_offset(map);
    const float stdh = map.quantize.found ? float(map.quantize.standard_height) : 0.0f;
    const bool has_raw = map.cliff.raw.size() == std::size_t(W) * H;
    auto uniform_level = [&](int c, int r) {
        int lv = -1;
        for (int oy = -2; oy <= 1; ++oy)
            for (int ox = -2; ox <= 1; ++ox) {
                const int x = std::min(std::max(c + ox, 0), int(W) - 1), y = std::min(std::max(r + oy, 0), int(H) - 1);
                const std::size_t k = std::size_t(y) * W + std::size_t(x);
                if (has_raw && (map.cliff.raw[k] & 63)) return -1;
                const int L = int(map.cliff.levels[k]);
                if (lv >= 0 && L != lv) return -1;
                lv = L;
            }
        return lv;
    };
    auto near_level = [&](int c, int r) {
        int lv = -1;
        for (int oy = -1; oy <= 0; ++oy)
            for (int ox = -1; ox <= 0; ++ox) {
                const int x = c + ox, y = r + oy;
                if (x < 0 || y < 0 || x >= int(W) || y >= int(H)) return -1;
                const std::size_t k = std::size_t(y) * W + std::size_t(x);
                if (has_raw && (map.cliff.raw[k] & 63)) return -1;
                const int L = int(map.cliff.levels[k]);
                if (lv >= 0 && L != lv) return -1;
                lv = L;
            }
        return lv;
    };
    auto pure = [&](int c, int r) {
        return formats::map_vertex_height(map, std::uint32_t(c), std::uint32_t(r)) - off -
               (formats::map_sculpt_height(map, std::uint32_t(c), std::uint32_t(r)) - stdh);
    };
    std::vector<float> visual(std::size_t(VW) * (H + 1), 0.0f);
    std::vector<std::uint8_t> mask(visual.size(), 0);
    std::size_t flagged = 0;
    std::vector<float> tier_cache;
    std::vector<std::uint8_t> tier_done;
    auto tier_of = [&](int L) {
        if (std::size_t(L) >= tier_done.size()) { tier_done.resize(std::size_t(L) + 1, 0); tier_cache.resize(std::size_t(L) + 1, 0.0f); }
        if (!tier_done[std::size_t(L)]) {
            std::vector<float> ps;
            for (int r = 0; r <= int(H); ++r)
                for (int c = 0; c <= int(W); ++c)
                    if (uniform_level(c, r) == L) ps.push_back(pure(c, r));
            float t = std::nanf("");
            if (!ps.empty()) {
                std::nth_element(ps.begin(), ps.begin() + std::ptrdiff_t(ps.size() / 2), ps.end());
                t = ps[ps.size() / 2];
            }
            tier_cache[std::size_t(L)] = t;
            tier_done[std::size_t(L)] = 1;
        }
        return tier_cache[std::size_t(L)];
    };
    static const bool kFlaggedOnly = std::getenv("SC2_ADJUST_FLAGGED_ONLY") != nullptr;
    if (!kFlaggedOnly)
        for (int r = 0; r <= int(H); ++r)
            for (int c = 0; c <= int(W); ++c) {
                static const bool kWide = std::getenv("SC2_ADJUST_WIDE_UNIFORM") != nullptr;
                const int L = kWide ? uniform_level(c, r) : near_level(c, r);
                if (L < 0) continue;
                const float tier = tier_of(L);
                if (std::isnan(tier)) continue;
                if (std::fabs(pure(c, r) - tier) <= 0.1f) continue;
                const std::size_t k = std::size_t(r) * VW + std::size_t(c);
                if (!mask[k]) ++flagged;
                mask[k] = 1;
                visual[k] = tier + formats::map_sculpt_height(map, std::uint32_t(c), std::uint32_t(r)) - stdh;
            }
    for (const formats::PlacedDoodad& d : map.objects.doodads) {
        if (!kFlaggedOnly) break;
        if (!models.doodad_adjusts_terrain(store, d.type)) continue;
        const formats::Model* pm = models.doodad_model(store, d.type, d.variation);
        if (!pm) continue;
        float rad2 = 0.0f;
        for (std::size_t i = 0; i + 2 < pm->positions.size(); i += 3)
            rad2 = std::max(rad2, pm->positions[i] * pm->positions[i] + pm->positions[i + 1] * pm->positions[i + 1]);
        const float rad = std::sqrt(rad2) * std::max(std::fabs(d.sx), std::fabs(d.sy)) + 1.0f;
        const int c0 = std::max(0, int(std::floor(d.x - rad))), c1 = std::min(int(W), int(std::ceil(d.x + rad)));
        const int r0 = std::max(0, int(std::floor(d.y - rad))), r1 = std::min(int(H), int(std::ceil(d.y + rad)));
        for (int r = r0; r <= r1; ++r)
            for (int c = c0; c <= c1; ++c) {
                const float dx = float(c) - d.x, dy = float(r) - d.y;
                if (dx * dx + dy * dy > rad * rad) continue;
                const int L = uniform_level(c, r);
                if (L < 0) continue;
                const float tier = tier_of(L);
                if (std::isnan(tier)) continue;
                const float p = pure(c, r);
                if (std::fabs(p - tier) <= 0.1f) continue;
                const float vis = tier + formats::map_sculpt_height(map, std::uint32_t(c), std::uint32_t(r)) - stdh;
                const std::size_t k = std::size_t(r) * VW + std::size_t(c);
                if (!mask[k]) ++flagged;
                mask[k] = 1;
                visual[k] = vis;
            }
    }
    if (!flagged) return 0;
    const bool has_normals = ground.normals.size() == ground.positions.size();
    std::size_t moved = 0;
    for (std::size_t i = 0; i + 2 < ground.positions.size(); i += 3) {
        const float fx = ground.positions[i] / cell, fy = ground.positions[i + 1] / cell;
        const float cx = std::round(fx), cy = std::round(fy);
        if (std::fabs(fx - cx) > 1e-4f || std::fabs(fy - cy) > 1e-4f || cx < 0.0f || cy < 0.0f || cx > float(W) || cy > float(H)) continue;
        const std::size_t k = std::size_t(cy) * VW + std::size_t(cx);
        if (!mask[k]) continue;
        const float sync = formats::map_vertex_height(map, std::uint32_t(cx), std::uint32_t(cy)) - off;
        if (std::fabs(ground.positions[i + 2] - sync) > 1e-3f) continue;
        ground.positions[i + 2] = visual[k];
        if (has_normals) {
            ground.normals[i] = 0.0f; ground.normals[i + 1] = 0.0f; ground.normals[i + 2] = 1.0f;
            if (!kFlaggedOnly) {
                auto zat = [&](int c, int r) {
                    c = std::min(std::max(c, 0), int(W));
                    r = std::min(std::max(r, 0), int(H));
                    const std::size_t q = std::size_t(r) * VW + std::size_t(c);
                    return mask[q] ? visual[q] : formats::map_vertex_height(map, std::uint32_t(c), std::uint32_t(r)) - off;
                };
                const int c = int(cx), r = int(cy);
                const float nx = -(zat(c + 1, r) - zat(c - 1, r)) / (2.0f * cell), ny = -(zat(c, r + 1) - zat(c, r - 1)) / (2.0f * cell);
                const float nl = std::sqrt(nx * nx + ny * ny + 1.0f);
                ground.normals[i] = nx / nl; ground.normals[i + 1] = ny / nl; ground.normals[i + 2] = 1.0f / nl;
            }
        }
        ++moved;
    }
    return moved;
}

}
