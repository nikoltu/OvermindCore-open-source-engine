#pragma once
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/map_archive.hpp"
#include "sc2/formats/placed_objects.hpp"

namespace sc2::app {

inline std::unordered_map<std::string, std::string> water_type_field(casc::Storage& store, const std::string& field,
                                                                     const std::vector<std::uint8_t>* map_xml = nullptr) {
    std::unordered_map<std::string, std::string> out;
    static const char* kMods[] = {nullptr, "void", "swarm", "liberty", "core"};
    const std::string want = "<" + field + " value=\"";
    for (const char* m : kMods) {
        std::string xml;
        if (!m) {
            if (!map_xml) continue;
            xml.assign(map_xml->begin(), map_xml->end());
        } else {
            try {
                const auto b = store.read_by_name(std::string("mods/") + m + ".sc2mod/base.sc2data/GameData/WaterData.xml");
                xml.assign(b.begin(), b.end());
            } catch (const std::exception&) {
                continue;
            }
        }
        for (std::size_t pos = xml.find("<CWater id=\""); pos != std::string::npos; pos = xml.find("<CWater id=\"", pos + 1)) {
            const std::size_t ids = pos + 12, ide = xml.find('"', ids);
            if (ide == std::string::npos) break;
            const std::string id = xml.substr(ids, ide - ids);
            if (out.count(id)) continue;
            const std::size_t gt = xml.find('>', ide);
            if (gt == std::string::npos || xml[gt - 1] == '/') continue;
            const std::size_t end = xml.find("</CWater>", gt);
            const std::size_t f = xml.find(want, gt);
            if (f == std::string::npos || f > end) continue;
            const std::size_t vs = f + want.size(), ve = xml.find('"', vs);
            if (ve != std::string::npos) out.emplace(id, xml.substr(vs, ve - vs));
        }
    }
    return out;
}

inline std::vector<formats::PlacedDoodad> shoreline_placements(const formats::LoadedMap& map,
                                                                const std::unordered_map<std::string, float>& type_heights,
                                                                casc::Storage& store, const std::vector<std::uint8_t>* map_xml,
                                                                float spacing = 2.0f,
                                                                float cliff_step = 1.5f) {
    std::vector<formats::PlacedDoodad> out;
    if (map.water.empty() || spacing <= 0.0f) return out;
    const float scale = map.quantize.found ? float(map.quantize.quantize_scale) : 1.0f / 512.0f;
    const float bias = (map.quantize.found ? float(map.quantize.quantize_bias) : 0.0f) +
                       (map.quantize.found ? float(map.quantize.standard_height) : 0.0f);
    const formats::HeightMap& hm = map.heightmap;
    const std::vector<std::uint8_t>& raw = map.heightmap_raw;
    if (hm.width < 2 || hm.height < 2 || hm.stride < 2) return out;
    static const bool kSculpt = std::getenv("SC2_SHORELINE_SCULPT") != nullptr;
    const float z_off = formats::engine_z_offset(map);
    auto height_at = [&](std::uint32_t col, std::uint32_t row) -> float {
        if (!kSculpt) return formats::map_vertex_height(map, col, row) - z_off;
        if (col >= hm.width) col = hm.width - 1;
        if (row >= hm.height) row = hm.height - 1;
        const std::size_t rec = hm.data_offset + (std::size_t(row) * hm.width + col) * hm.stride;
        if (rec + 1 >= raw.size()) return 0.0f;
        const std::uint16_t h = std::uint16_t(raw[rec]) | std::uint16_t(raw[rec + 1]) << 8;
        return float(h) * scale - bias;
    };
    std::unordered_set<std::uint64_t> taken;
    const auto beaches = water_type_field(store, "BeachShoreline", map_xml);
    const auto cliffs = water_type_field(store, "CliffShoreline", map_xml);
    auto lookup = [](const std::unordered_map<std::string, std::string>& m, const std::string& k) -> std::optional<std::string> {
        const auto it = m.find(k);
        return it == m.end() || it->second.empty() ? std::nullopt : std::optional<std::string>(it->second);
    };
    for (const formats::WaterPlane& p : map.water.planes) {
        const auto th = type_heights.find(p.type);
        const std::optional<std::string> beach = lookup(beaches, p.type);
        const std::optional<std::string> cliff = lookup(cliffs, p.type);
        if (std::getenv("SC2_SHORELINE_TRACE"))
            std::fprintf(stderr, "[shoreline] plane '%s' bbox %.0f,%.0f..%.0f,%.0f height %s beach '%s' cliff '%s' (CWater entry %d)\n", p.type.c_str(),
                         double(p.min_x), double(p.min_y), double(p.max_x), double(p.max_y),
                         th == type_heights.end() ? "-" : std::to_string(th->second).c_str(), beach ? beach->c_str() : "-", cliff ? cliff->c_str() : "-", 1);
        if (th == type_heights.end()) continue;
        const float level = th->second - z_off;
        if (!beach && !cliff) continue;
        const int x0 = int(std::floor(std::min(p.min_x, p.max_x))), x1 = int(std::ceil(std::max(p.min_x, p.max_x)));
        const int y0 = int(std::floor(std::min(p.min_y, p.max_y))), y1 = int(std::ceil(std::max(p.min_y, p.max_y)));
        for (int y = std::max(0, y0); y <= std::min(int(hm.height) - 1, y1); ++y)
            for (int x = std::max(0, x0); x <= std::min(int(hm.width) - 1, x1); ++x) {
                const float hw = height_at(std::uint32_t(x), std::uint32_t(y));
                if (hw > level) continue;
                static const int kN[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (const auto& n : kN) {
                    const int nx = x + n[0], ny = y + n[1];
                    if (nx < 0 || ny < 0 || nx >= int(hm.width) || ny >= int(hm.height)) continue;
                    const float hl = height_at(std::uint32_t(nx), std::uint32_t(ny));
                    if (hl <= level) continue;
                    const float t = (level - hw) / std::max(hl - hw, 1e-4f);
                    const float px = float(x) + t * float(n[0]), py = float(y) + t * float(n[1]);
                    const std::uint64_t key = (std::uint64_t(std::uint32_t(std::floor(px / spacing))) << 32) |
                                              std::uint32_t(std::floor(py / spacing));
                    if (!taken.insert(key).second) continue;
                    formats::PlacedDoodad d;
                    const bool is_cliff = cliff && (hl - hw) > cliff_step;
                    d.type = is_cliff ? *cliff : (beach ? *beach : *cliff);
                    d.x = px;
                    d.y = py;
                    static const float kLift = std::getenv("SC2_SHORELINE_LIFT") ? float(std::atof(std::getenv("SC2_SHORELINE_LIFT"))) : 0.0f;
                    d.z = level + kLift;
                    d.rotation = std::atan2(-float(n[1]), -float(n[0]));
                    d.height_absolute = true;
                    out.push_back(d);
                }
            }
    }
    return out;
}

}
