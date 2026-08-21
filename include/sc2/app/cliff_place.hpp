#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/formats/map_archive.hpp"
#include "sc2/render/scene.hpp"

namespace sc2::app {

struct CliffField {
    std::map<std::string, formats::Model> pieces;
    std::vector<render::SceneInstance> instances;
    std::size_t missing_pieces = 0;
    std::set<std::string> missing_codes;
};

inline std::string cliff_xml_first(const std::string& xml, const std::string& open,
                                   const std::string& attr) {
    std::size_t p = xml.find(open);
    if (p == std::string::npos) return "";
    std::size_t a = xml.find(attr + "=\"", p);
    if (a == std::string::npos) return "";
    a += attr.size() + 2;
    std::size_t e = xml.find('"', a);
    return e == std::string::npos ? "" : xml.substr(a, e - a);
}

inline CliffField place_cliffs(casc::Storage& store, const formats::LoadedMap& map, int order = 0,
                               float tier_h = 2.0f, float ccx = 0, float ccy = 0, float crad = 0) {
    CliffField cf;
    if (map.cliff.empty()) return cf;
    const std::string assetbase = "mods/liberty.sc2mod/base.sc2assets/";
    auto read_text = [&](const std::string& name) -> std::string {
        try { auto b = store.read_by_name(name); return std::string(b.begin(), b.end()); }
        catch (...) { return ""; }
    };
    auto read_all_mods = [&](const char* file) -> std::string {
        static const char* kMods[] = {"void", "swarm", "liberty", "core"};
        std::string all;
        for (const char* m : kMods)
            all += read_text(std::string("mods/") + m + ".sc2mod/base.sc2data/GameData/" + file);
        return all;
    };
    const std::string terr = read_all_mods("TerrainData.xml");
    const std::string cliffxml = read_all_mods("CliffData.xml");
    std::vector<std::string> sets = map.cliff_sets;
    if (sets.empty()) {
        std::string s = map.cliff_set;
        if (s.empty()) {
            std::size_t tp = terr.find("<CTerrain id=\"" + map.tileset + "\">");
            s = tp == std::string::npos ? "" : cliff_xml_first(terr.substr(tp), "<CliffSets", "value");
        }
        if (!s.empty()) sets.push_back(s);
    }
    if (sets.empty()) return cf;

    struct SetInfo { std::string mesh, root; std::vector<formats::M3Layer> skin; };
    std::vector<SetInfo> setinfo(sets.size());
    for (std::size_t s = 0; s < sets.size(); ++s) {
        std::size_t cp = cliffxml.find("<CCliff id=\"" + sets[s] + "\">");
        std::string mesh = cp == std::string::npos ? "" : cliff_xml_first(cliffxml.substr(cp), "<CliffMesh", "value");
        if (mesh.empty()) continue;
        setinfo[s].mesh = mesh;
        setinfo[s].root = assetbase + "Assets/Cliffs/" + mesh + "/";
        try {
            const std::string mp =
                assetbase + "Assets/Cliffs/" + sets[s] + "_Material/" + sets[s] + "_Material.m3";
            formats::Model mm = formats::M3::parse(store.read_by_name(mp)).decode_model();
            if (!mm.materials.empty()) setinfo[s].skin = mm.materials[0].layers;
        } catch (...) {}
    }
    if (setinfo[0].mesh.empty()) return cf;

    const formats::CliffMap& cm = map.cliff;
    auto lvl = [&](int x, int y) -> int {
        if (x < 0 || y < 0 || x >= int(cm.width) || y >= int(cm.height)) return -1;
        return int(cm.levels[std::size_t(y) * cm.width + x]);
    };
    const formats::SyncHeightMap& shm = map.sync_height;
    const bool have_vlev = !shm.empty() && shm.level.size() == std::size_t(shm.width) * shm.height;
    auto vlvl = [&](int x, int y) -> int {
        if (!have_vlev) return -1;
        if (x < 0 || y < 0 || x >= int(shm.width) || y >= int(shm.height)) return -1;
        return int(shm.level[std::size_t(y) * shm.width + x]);
    };
    const bool have_raw = cm.raw.size() == std::size_t(cm.width) * cm.height;
    auto is_ramp = [&](int x, int y) -> bool {
        if (!have_raw || x < 0 || y < 0 || x >= int(cm.width) || y >= int(cm.height)) return false;
        return (cm.raw[std::size_t(y) * cm.width + x] & 63) != 0;
    };
    const float kPieceWalkable = 4.8f;
    const float zscale = tier_h / kPieceWalkable;
    const float sign = (order & 1) ? -1.0f : 1.0f;

    auto emit = [&](int si, const std::string& code, float wx, float wy, int mn, int steps, float xy, int var) {
        if (si < 0 || si >= int(setinfo.size()) || setinfo[std::size_t(si)].mesh.empty()) si = 0;
        const SetInfo& S = setinfo[std::size_t(si)];
        char vc = char('0' + (var >= 0 && var <= 3 ? var : 0));
        const std::string key = S.mesh + "_" + code + "_" + std::to_string(si) + "_" + vc;
        auto it = cf.pieces.find(key);
        if (it == cf.pieces.end()) {
            formats::Model m;
            for (const std::string& nn : {std::string("_0") + vc + ".m3", std::string("_00.m3"),
                                          std::string("_01.m3"), std::string("_02.m3"), std::string("_03.m3")}) {
                try { m = formats::M3::parse(store.read_by_name(S.root + S.mesh + "_" + code + nn)).decode_model();
                      if (m.vertex_count) break; } catch (...) {}
            }
            if (m.vertex_count && !S.skin.empty())
                for (formats::M3Material& mat : m.materials) mat.layers = S.skin;
            it = cf.pieces.emplace(key, std::move(m)).first;
        }
        if (it->second.vertex_count == 0) {
            ++cf.missing_pieces;
            cf.missing_codes.insert(code);
            return;
        }
        render::SceneInstance inst;
        inst.model = &it->second;
        inst.world = render::translation(wx, wy, float(mn) * tier_h) *
                     render::rotation_z(sign * float(steps) * 1.5707963f) *
                     render::scaling(xy, xy, zscale);
        inst.team = {0, 0, 0, -1.0f};
        inst.seq = -1;
        cf.instances.push_back(inst);
    };
    auto codeify = [](const int t[4], std::string& code) -> int {
        int best_r = 0;
        for (int r = 1; r < 4; ++r)
            for (int k = 0; k < 4; ++k) {
                const int a = t[(k + r) & 3], b = t[(k + best_r) & 3];
                if (a != b) { if (a < b) best_r = r; break; }
            }
        int mn = t[0];
        for (int k = 1; k < 4; ++k) mn = t[k] < mn ? t[k] : mn;
        code.clear();
        for (int k = 0; k < 4; ++k) code += char('A' + (t[(k + best_r) & 3] - mn));
        return best_r;
    };

    if (!map.cliff_cells.empty() && map.cliff_grid_w > 1) {
        const int gw = int(map.cliff_grid_w);
        for (const formats::CliffCell& c : map.cliff_cells) {
            if (c.flag != 1) continue;
            const int cx = int(c.index) % gw, cy = int(c.index) / gw;
            int t[4];
            if (have_vlev) {
                t[0] = vlvl(2 * cx, 2 * cy);
                t[1] = vlvl(2 * cx + 2, 2 * cy);
                t[2] = vlvl(2 * cx + 2, 2 * cy + 2);
                t[3] = vlvl(2 * cx, 2 * cy + 2);
            } else {
                t[0] = lvl(2 * cx - 1, 2 * cy - 1);
                t[1] = lvl(2 * cx + 1, 2 * cy - 1);
                t[2] = lvl(2 * cx + 1, 2 * cy + 1);
                t[3] = lvl(2 * cx - 1, 2 * cy + 1);
            }
            if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[3] < 0) continue;
            if (t[0] == t[1] && t[1] == t[2] && t[2] == t[3]) {
                t[0] = lvl(2 * cx - 1, 2 * cy - 1);
                t[1] = lvl(2 * cx + 1, 2 * cy - 1);
                t[2] = lvl(2 * cx + 1, 2 * cy + 1);
                t[3] = lvl(2 * cx - 1, 2 * cy + 1);
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[3] < 0) continue;
                if (t[0] == t[1] && t[1] == t[2] && t[2] == t[3]) continue;
            }
            std::string code;
            const int steps = codeify(t, code);
            const int mn = std::min(std::min(t[0], t[1]), std::min(t[2], t[3]));
            emit(int(c.cid), code, float(2 * cx + 1), float(2 * cy + 1), mn, steps, 1.0f, int(c.cvar));
        }
    } else {
        for (int vy = 1; vy < int(cm.height); ++vy)
            for (int vx = 1; vx < int(cm.width); ++vx) {
                const int t[4] = {lvl(vx - 1, vy - 1), lvl(vx, vy - 1), lvl(vx, vy), lvl(vx - 1, vy)};
                if (t[0] < 0 || t[1] < 0 || t[2] < 0 || t[3] < 0) continue;
                if (t[0] == t[1] && t[1] == t[2] && t[2] == t[3]) continue;
                const int nramp = int(is_ramp(vx - 1, vy - 1)) + int(is_ramp(vx, vy - 1)) +
                                  int(is_ramp(vx, vy)) + int(is_ramp(vx - 1, vy));
                if (nramp >= 2) continue;
                if (crad > 0) {
                    const float ddx = float(vx) - ccx, ddy = float(vy) - ccy;
                    if (ddx * ddx + ddy * ddy > crad * crad) continue;
                }
                std::string code;
                const int steps = codeify(t, code);
                const int mn = std::min(std::min(t[0], t[1]), std::min(t[2], t[3]));
                emit(0, code, float(vx), float(vy), mn, steps, 0.5f, 0);
            }
    }
    return cf;
}

}
