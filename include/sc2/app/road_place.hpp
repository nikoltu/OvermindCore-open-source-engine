#pragma once
#include <cmath>
#include <cstdlib>
#include <string>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/formats/map_archive.hpp"

namespace sc2::app {

inline formats::Model build_map_roads(casc::Storage& store, const formats::LoadedMap& map, float terrain_spec_exp = 0.0f,
                                        float terrain_spec_gain = 1.0f) {
    formats::Model m;
    if (map.hard_tiles.empty() || map.cliff.empty()) return m;
    const std::string assetbase = "mods/liberty.sc2mod/base.sc2assets/";
    std::string tiles;
    for (const char* mod : {"void", "swarm", "liberty", "core"}) {
        try {
            auto b = store.read_by_name(std::string("mods/") + mod + ".sc2mod/base.sc2data/GameData/TileData.xml");
            tiles.append(b.begin(), b.end());
        } catch (...) {
        }
    }
    const auto field = [](const std::string& block, const char* name, float fallback) {
        const std::string key = std::string("<") + name + " value=\"";
        const std::size_t p = block.find(key);
        return p == std::string::npos ? fallback : std::strtof(block.c_str() + p + key.size(), nullptr);
    };
    const std::vector<float> th = formats::tier_heights(map);
    const float off = formats::engine_z_offset(map);
    constexpr float kLift = 0.04f;
    constexpr int kAcross = 4;
    for (const formats::HardTile& road : map.hard_tiles) {
        if (road.points.size() < 2) continue;
        const std::size_t tp = tiles.find("<CTile id=\"" + road.tile + "\">");
        if (tp == std::string::npos) continue;
        const std::size_t te = tiles.find("</CTile>", tp);
        const std::string block = tiles.substr(tp, te == std::string::npos ? std::string::npos : te - tp);
        std::string mat;
        {
            const std::string key = "<Material value=\"";
            const std::size_t p = block.find(key);
            if (p == std::string::npos) continue;
            const std::size_t e = block.find('"', p + key.size());
            mat = block.substr(p + key.size(), e - p - key.size());
            for (char& c : mat)
                if (c == '\\') c = '/';
        }
        formats::Model skin;
        try {
            skin = formats::M3::parse(store.read_by_name(assetbase + mat)).decode_model();
        } catch (...) {
            continue;
        }
        if (skin.materials.empty()) continue;
        const float tess = std::max(field(block, "TesselationDistance", 1.0f), 0.25f);
        const float spline_w = field(block, "DefaultSplineWidth", 1.0f);
        const float wing_w = field(block, "DefaultWingWidth", 1.0f);
        static const bool kOldSize = std::getenv("SC2_ROAD_OLD_SIZE") != nullptr;
        const float kTexRepeat = kOldSize ? 9.6f : 8.0f;

        formats::M3Region rg;
        rg.first_vertex = std::uint32_t(m.positions.size() / 3);
        rg.first_index = std::uint32_t(m.indices.size());
        std::uint32_t rows = 0;
        float along = 0.0f;
        float px = 0.0f, py = 0.0f;
        bool have_prev = false;
        float u_fixed = -1.0f;
        bool next_starts = false;
        std::vector<char> strip_start;
        float first_row[5] = {0, 0, 0, 0, 0}, last_row[5] = {0, 0, 0, 0, 0};
        const auto emit_row = [&](float x, float y, float dx, float dy, float width) {
            const float dl = std::sqrt(dx * dx + dy * dy);
            if (dl < 1e-6f) return;
            if (u_fixed < 0.0f) {
                if (have_prev) along += std::sqrt((x - px) * (x - px) + (y - py) * (y - py));
                if (!have_prev) { first_row[0] = x; first_row[1] = y; first_row[2] = dx; first_row[3] = dy; first_row[4] = width; }
                last_row[0] = x; last_row[1] = y; last_row[2] = dx; last_row[3] = dy; last_row[4] = width;
                px = x; py = y; have_prev = true;
            }
            strip_start.push_back(next_starts ? 1 : 0);
            next_starts = false;
            const float half = spline_w * width + (kOldSize ? wing_w : 0.5f * wing_w);
            const float sx = -dy / dl * half, sy = dx / dl * half;
            for (int k = 0; k <= kAcross; ++k) {
                const float t = float(k) / float(kAcross), s = 2.0f * t - 1.0f;
                const float vx = x + sx * s, vy = y + sy * s;
                m.positions.insert(m.positions.end(), {vx, vy, formats::vertex_ground_bilinear(map, th, off, vx, vy) + kLift});
                m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
                m.uvs.push_back(u_fixed >= 0.0f ? u_fixed : along / kTexRepeat);
                m.uvs.push_back((u_fixed >= 0.0f ? 0.0f : 0.5f) + 0.5f * t);
            }
            ++rows;
        };
        for (std::size_t i = 0; i + 1 < road.points.size(); ++i) {
            const formats::HardTilePoint& a = road.points[i];
            const formats::HardTilePoint& b = road.points[i + 1];
            const float c0[2] = {a.pos[0], a.pos[1]}, c1[2] = {a.pos[0] + a.tan_out[0], a.pos[1] + a.tan_out[1]};
            const float c2[2] = {b.pos[0] + b.tan_in[0], b.pos[1] + b.tan_in[1]}, c3[2] = {b.pos[0], b.pos[1]};
            const float seg = std::sqrt((c3[0] - c0[0]) * (c3[0] - c0[0]) + (c3[1] - c0[1]) * (c3[1] - c0[1]));
            const int steps = std::max(1, int(std::ceil(seg / tess)));
            for (int s = (i == 0 ? 0 : 1); s <= steps; ++s) {
                if (rows * std::uint32_t(kAcross + 1) + std::uint32_t(kAcross + 1) > 60000u) break;
                const float t = float(s) / float(steps), u = 1.0f - t;
                const float w0 = u * u * u, w1 = 3.0f * u * u * t, w2 = 3.0f * u * t * t, w3 = t * t * t;
                const float x = w0 * c0[0] + w1 * c1[0] + w2 * c2[0] + w3 * c3[0];
                const float y = w0 * c0[1] + w1 * c1[1] + w2 * c2[1] + w3 * c3[1];
                const float d0 = 3.0f * u * u, d1 = 6.0f * u * t, d2 = 3.0f * t * t;
                const float dx = d0 * (c1[0] - c0[0]) + d1 * (c2[0] - c1[0]) + d2 * (c3[0] - c2[0]);
                const float dy = d0 * (c1[1] - c0[1]) + d1 * (c2[1] - c1[1]) + d2 * (c3[1] - c2[1]);
                emit_row(x, y, dx, dy, a.width + (b.width - a.width) * t);
            }
        }
        static const bool kNoCaps = std::getenv("SC2_ROAD_NO_END_CAPS") != nullptr;
        if (!kNoCaps && rows >= 2) {
            constexpr int kCapRows = 2;
            const float cell = kTexRepeat * 0.125f;
            const auto cap = [&](const float* e, float sign, float u_near, float u_far) {
                const float dl = std::sqrt(e[2] * e[2] + e[3] * e[3]);
                if (dl < 1e-6f) return;
                const float ux = sign * e[2] / dl, uy = sign * e[3] / dl;
                for (int s = 0; s <= kCapRows; ++s) {
                    const float f = float(s) / float(kCapRows);
                    u_fixed = u_near + (u_far - u_near) * f;
                    next_starts = s == 0;
                    emit_row(e[0] + ux * cell * f, e[1] + uy * cell * f, e[2], e[3], e[4]);
                }
                u_fixed = -1.0f;
            };
            cap(last_row, 1.0f, 0.375f, 0.5f);
            cap(first_row, -1.0f, 0.625f, 0.5f);
        }
        if (rows < 2) {
            m.positions.resize(std::size_t(rg.first_vertex) * 3);
            m.normals.resize(std::size_t(rg.first_vertex) * 3);
            m.uvs.resize(std::size_t(rg.first_vertex) * 2);
            continue;
        }
        const std::uint16_t rw = std::uint16_t(kAcross + 1);
        for (std::uint32_t r = 0; r + 1 < rows; ++r)
            for (int k = 0; k < kAcross && !strip_start[r + 1]; ++k) {
                const std::uint16_t v0 = std::uint16_t(r * rw + k), v1 = std::uint16_t(v0 + 1);
                const std::uint16_t v2 = std::uint16_t(v0 + rw), v3 = std::uint16_t(v2 + 1);
                m.indices.insert(m.indices.end(), {v0, v1, v2, v1, v3, v2, v0, v2, v1, v1, v2, v3});
            }
        rg.num_vertices = std::uint32_t(m.positions.size() / 3) - rg.first_vertex;
        rg.num_indices = std::uint32_t(m.indices.size()) - rg.first_index;
        m.regions.push_back(rg);
        m.region_material.push_back(int(m.materials.size()));
        m.materials.push_back(skin.materials[0]);
        if (terrain_spec_exp > 0.0f) {
            formats::M3Material& rm = m.materials.back();
            rm.specularity = terrain_spec_exp;
            rm.spec_undimmed = true;
            rm.spec_luma = true;
            rm.spec_mult = terrain_spec_gain;
        }
    }
    m.vertex_count = std::uint32_t(m.positions.size() / 3);
    for (int a = 0; a < 3; ++a) { m.bounds_min[a] = 1e30f; m.bounds_max[a] = -1e30f; }
    for (std::uint32_t i = 0; i < m.vertex_count; ++i)
        for (int a = 0; a < 3; ++a) {
            m.bounds_min[a] = std::min(m.bounds_min[a], m.positions[i * 3 + a]);
            m.bounds_max[a] = std::max(m.bounds_max[a], m.positions[i * 3 + a]);
        }
    return m;
}

}
