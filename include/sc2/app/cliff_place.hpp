#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
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
    std::size_t missing_materials = 0;
    std::size_t missing_cliff_heights = 0;
    std::size_t first_ramp_instance = 0;
    std::size_t ramp_pieces = 0;
    std::size_t unresolved_ramp_frames = 0;
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
    const std::string meshxml = read_all_mods("CliffMeshData.xml");
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

    struct SetInfo { std::string mesh, root; std::vector<formats::M3Layer> skin; float native = 0.0f; };
    std::vector<SetInfo> setinfo(sets.size());
    for (std::size_t s = 0; s < sets.size(); ++s) {
        std::size_t cp = cliffxml.find("<CCliff id=\"" + sets[s] + "\">");
        std::string mesh = cp == std::string::npos ? "" : cliff_xml_first(cliffxml.substr(cp), "<CliffMesh", "value");
        if (mesh.empty()) continue;
        setinfo[s].mesh = mesh;
        setinfo[s].root = assetbase + "Assets/Cliffs/" + mesh + "/";
        std::size_t mep = meshxml.find("<CCliffMesh id=\"" + mesh + "\">");
        std::string mh = mep == std::string::npos
                             ? ""
                             : cliff_xml_first(meshxml.substr(mep), "<CliffHeights", "value");
        setinfo[s].native = mh.empty() ? 0.0f : std::fabs(std::strtof(mh.c_str(), nullptr));
        if (setinfo[s].native <= 0.0f) ++cf.missing_cliff_heights;
        std::string mp = cp == std::string::npos
                             ? ""
                             : cliff_xml_first(cliffxml.substr(cp), "<CliffMaterial", "value");
        for (char& c : mp)
            if (c == '\\') c = '/';
        if (mp.empty()) mp = "Assets/Cliffs/" + sets[s] + "_Material/" + sets[s] + "_Material.m3";
        else {
            std::size_t idp;
            while ((idp = mp.find("##id##")) != std::string::npos) mp.replace(idp, 6, sets[s]);
        }
        try {
            formats::Model mm = formats::M3::parse(store.read_by_name(assetbase + mp)).decode_model();
            if (!mm.materials.empty()) setinfo[s].skin = mm.materials[0].layers;
            else ++cf.missing_materials;
        } catch (...) { ++cf.missing_materials; }
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
    const std::vector<float> tier_z = formats::tier_heights(map);
    auto tier_top = [&](int L) -> float {
        if (tier_z.empty()) return float(L) * tier_h;
        const std::size_t i = L < 0 ? 0 : std::size_t(L);
        return tier_z[i < tier_z.size() ? i : tier_z.size() - 1];
    };
    auto tier_rise = [&](int L) -> float {
        if (tier_z.size() < 2) return tier_h;
        const std::size_t i = L < 0 ? 0 : std::size_t(L);
        if (i + 1 < tier_z.size()) return tier_z[i + 1] - tier_z[i];
        return tier_z[tier_z.size() - 1] - tier_z[tier_z.size() - 2];
    };
    const float sign = (order & 1) ? -1.0f : 1.0f;

    auto load_piece = [&](int& si, const std::string& code, int var) -> const formats::Model* {
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
            m.cliff_skin_opaque = true;
            it = cf.pieces.emplace(key, std::move(m)).first;
        }
        return &it->second;
    };

    auto emit = [&](int si, const std::string& code, float wx, float wy, int mn, int steps, float xy, int var) {
        const formats::Model* piece = load_piece(si, code, var);
        const SetInfo& S = setinfo[std::size_t(si)];
        if (piece->vertex_count == 0) {
            ++cf.missing_pieces;
            cf.missing_codes.insert(code);
            return;
        }
        render::SceneInstance inst;
        inst.model = piece;
        const float rise = tier_rise(mn);
        const float native = S.native > 0.0f ? S.native : piece->bounds_max[2];
        const float zscale = native > 1e-3f ? (rise > 1e-3f ? rise : tier_h) / native : 1.0f;
        inst.world = render::translation(wx, wy, tier_top(mn)) *
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
            if (!(c.flag & 1)) continue;
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
    cf.first_ramp_instance = cf.instances.size();
    if (have_vlev && !map.ramps.empty()) {
        auto emit_ramp = [&](int si, const formats::RampFrame& f, bool left, int var) {
            if (!f.used() || f.w <= 0.0f || f.h <= 0.0f) return;
            const float sgn_r[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
            const float sgn_u[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
            const float ramp_side = left ? 1.0f : -1.0f;
            struct Miss { CliffField& cf; bool armed = true; ~Miss() { if (armed) ++cf.unresolved_ramp_frames; } } miss{cf};
            int lv[4];
            for (int k = 0; k < 4; ++k) {
                const float x = f.cx + sgn_r[k] * f.w * f.rx + sgn_u[k] * f.h * f.ux;
                const float y = f.cy + sgn_r[k] * f.w * f.ry + sgn_u[k] * f.h * f.uy;
                lv[k] = vlvl(int(std::lround(x)), int(std::lround(y)));
                if (lv[k] < 0) return;
            }
            int lo = lv[0];
            for (int k = 1; k < 4; ++k) lo = lv[k] < lo ? lv[k] : lo;
            const int base = lo > 0 ? lo - 1 : 0;
            std::string letters(4, ' ');
            for (int k = 0; k < 4; ++k) {
                const int rel = lv[k] - base;
                if (sgn_r[k] == ramp_side) {
                    if (rel < 1 || rel > 3) return;
                    letters[std::size_t(k)] = "QRS"[rel - 1];
                } else {
                    if (rel > 3) return;
                    letters[std::size_t(k)] = "ABCD"[rel];
                }
            }
            std::vector<std::string> cands{letters};
            for (int mask = 1; mask < 4; ++mask) {
                std::string c = letters;
                bool changed = false;
                int far = 0;
                for (int k = 0; k < 4; ++k) {
                    if (sgn_r[k] == ramp_side) continue;
                    const int rel = lv[k] - base;
                    if ((mask >> far) & 1 && rel >= 1 && rel <= 3) { c[std::size_t(k)] = "QRS"[rel - 1]; changed = true; }
                    ++far;
                }
                if (changed) cands.push_back(c);
            }
            const float want_r = 2.0f * f.w, want_u = 2.0f * f.h;
            for (const std::string& cand : cands)
            for (int yaw = 0; yaw < 4; ++yaw) {
                std::string code(4, ' ');
                for (int k = 0; k < 4; ++k) code[std::size_t(k)] = cand[std::size_t((k + yaw) & 3)];
                int use = si;
                const formats::Model* piece = load_piece(use, code, var);
                if (piece->vertex_count == 0) continue;
                const float ex = piece->bounds_max[0] - piece->bounds_min[0];
                const float ey = piece->bounds_max[1] - piece->bounds_min[1];
                const float need_x = (yaw & 1) ? want_u : want_r;
                const float need_y = (yaw & 1) ? want_r : want_u;
                if (need_x != need_y) {
                    const bool frame_x_longer = need_x > need_y;
                    const bool mesh_x_longer = ex > ey;
                    if (frame_x_longer != mesh_x_longer) continue;
                }
                int lo = lv[0], hi = lv[0];
                for (int k = 1; k < 4; ++k) { lo = lv[k] < lo ? lv[k] : lo; hi = lv[k] > hi ? lv[k] : hi; }
                const float rspan = piece->bounds_max[2] - piece->bounds_min[2];
                const float rnative = setinfo[std::size_t(use)].native > 0.0f
                                          ? setinfo[std::size_t(use)].native
                                          : rspan;
                const float zscale = rnative > 1e-3f && hi > lo
                                         ? (tier_top(hi) - tier_top(lo)) / rnative
                                         : 1.0f;
                render::SceneInstance inst;
                inst.model = piece;
                inst.world = render::translation(f.cx, f.cy,
                                                 tier_top(lo) - zscale * piece->bounds_min[2]) *
                             render::rotation_z(std::atan2(f.ry, f.rx) +
                                                sign * float(yaw) * 1.5707963f) *
                             render::scaling(1.0f, 1.0f, zscale);
                inst.team = {0, 0, 0, -1.0f};
                inst.seq = -1;
                cf.instances.push_back(inst);
                ++cf.ramp_pieces;
                miss.armed = false;
                return;
            }
        };
        for (const formats::TerrainRamp& r : map.ramps) {
            emit_ramp(int(r.cid), r.left_lo, true, int(r.left_lo_var));
            emit_ramp(int(r.cid), r.right_lo, false, int(r.right_lo_var));
            emit_ramp(int(r.cid), r.left_hi, true, int(r.left_hi_var));
            emit_ramp(int(r.cid), r.right_hi, false, int(r.right_hi_var));
        }
    }

    return cf;
}

}
