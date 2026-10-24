#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/dds.hpp"
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

inline bool cliff_piece_is_ground(const formats::Model& m);

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

    struct SetInfo { std::string mesh, root; std::vector<formats::M3Layer> skin; float native = 0.0f; std::uint32_t blend = 0; };
    std::vector<SetInfo> setinfo(sets.size());
    for (std::size_t s = 0; s < sets.size(); ++s) {
        std::size_t cp = cliffxml.find("<CCliff id=\"" + sets[s] + "\">");
        const std::size_t ce = cp == std::string::npos ? std::string::npos : cliffxml.find("</CCliff>", cp);
        const std::string cblock = cp == std::string::npos ? std::string() : cliffxml.substr(cp, ce == std::string::npos ? std::string::npos : ce - cp);
        std::string mesh = cp == std::string::npos ? "" : cliff_xml_first(cblock, "<CliffMesh", "value");
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
                             : cliff_xml_first(cblock, "<CliffMaterial", "value");
        for (char& c : mp)
            if (c == '\\') c = '/';
        if (mp.empty()) mp = "Assets/Cliffs/" + sets[s] + "_Material/" + sets[s] + "_Material.m3";
        else {
            std::size_t idp;
            while ((idp = mp.find("##id##")) != std::string::npos) mp.replace(idp, 6, sets[s]);
        }
        try {
            formats::Model mm = formats::M3::parse(store.read_by_name(assetbase + mp)).decode_model();
            if (!mm.materials.empty()) { setinfo[s].skin = mm.materials[0].layers; setinfo[s].blend = mm.materials[0].blend_mode; }
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
    std::vector<float> sculpt_of_level, pure_of_level;
    if (!std::getenv("SC2_CLIFF_SCULPT_TWICE") && map.quantize.found && map.heightmap.width > 0 && map.heightmap.height > 0) {
        const float stdh = float(map.quantize.standard_height);
        const float zoff_pure = formats::engine_z_offset(map);
        std::map<int, std::vector<float>> by_level, pure_by_level;
        for (std::uint32_t r = 1; r < cm.height; ++r)
            for (std::uint32_t c = 1; c < cm.width; ++c) {
                const std::size_t k = std::size_t(r) * cm.width + c;
                const int L = cm.levels[k];
                if (cm.levels[k - 1] != L || cm.levels[k - cm.width] != L || cm.levels[k - cm.width - 1] != L) continue;
                if (have_raw && ((cm.raw[k] | cm.raw[k - 1] | cm.raw[k - cm.width] | cm.raw[k - cm.width - 1]) & 63)) continue;
                by_level[L].push_back(formats::map_sculpt_height(map, c, r) - stdh);
                pure_by_level[L].push_back(formats::vertex_ground_z(map, tier_z, zoff_pure, c, r) - by_level[L].back());
            }
        for (auto& kv : pure_by_level) {
            if (kv.first < 0 || kv.second.empty()) continue;
            std::nth_element(kv.second.begin(), kv.second.begin() + std::ptrdiff_t(kv.second.size() / 2), kv.second.end());
            if (pure_of_level.size() <= std::size_t(kv.first)) pure_of_level.resize(std::size_t(kv.first) + 1, 1e30f);
            pure_of_level[std::size_t(kv.first)] = kv.second[kv.second.size() / 2];
        }
        for (auto& kv : by_level) {
            if (kv.first < 0 || kv.second.empty()) continue;
            std::nth_element(kv.second.begin(), kv.second.begin() + std::ptrdiff_t(kv.second.size() / 2), kv.second.end());
            if (sculpt_of_level.size() <= std::size_t(kv.first)) sculpt_of_level.resize(std::size_t(kv.first) + 1, 0.0f);
            sculpt_of_level[std::size_t(kv.first)] = kv.second[kv.second.size() / 2];
        }
    }
    auto pure_top = [&](int L) -> float {
        const std::size_t i = L < 0 ? 0 : std::size_t(L);
        if (i < pure_of_level.size() && pure_of_level[i] < 1e29f) return pure_of_level[i];
        return tier_top(L) - (i < sculpt_of_level.size() ? sculpt_of_level[i] : 0.0f);
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
                for (formats::M3Material& mat : m.materials) { mat.layers = S.skin; mat.blend_mode = S.blend; }
            m.vertex_alpha_coverage = true;
            m.cliff_skin_opaque = (m.vertex_flags & 0x200u) == 0;
            static const bool kSolidMasked = std::getenv("SC2_CLIFF_SOLID_MASKED") != nullptr;
            if (m.cliff_skin_opaque && !kSolidMasked && m.vertex_count && !m.materials.empty() &&
                formats::material_alpha_mask_channel(m.materials[0]) >= 1 && formats::material_alpha_mask_channel(m.materials[0]) <= 4) {
                const formats::M3Layer* ml = formats::material_alpha_mask(m.materials[0]);
                const int ch = formats::material_alpha_mask_channel(m.materials[0]);
                const std::vector<float>& uv = ml->word40 == 1 && m.uvs1.size() >= std::size_t(m.vertex_count) * 2 ? m.uvs1 : m.uvs;
                static std::map<std::string, std::pair<formats::DdsInfo, std::vector<std::uint8_t>>> masks;
                auto mit = masks.find(ml->image_path);
                if (mit == masks.end()) {
                    std::pair<formats::DdsInfo, std::vector<std::uint8_t>> d;
                    std::string ip = ml->image_path;
                    for (char& c : ip)
                        if (c == '\\') c = '/';
                    for (const char* r : {"mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/", "mods/swarm.sc2mod/base.sc2assets/", "mods/void.sc2mod/base.sc2assets/", ""}) {
                        try { d.second = formats::dds_decode_rgba(store.read_by_name(std::string(r) + ip), d.first); } catch (...) { d.second.clear(); }
                        if (!d.second.empty()) break;
                    }
                    mit = masks.emplace(ml->image_path, std::move(d)).first;
                }
                const formats::DdsInfo& di = mit->second.first;
                const std::vector<std::uint8_t>& px = mit->second.second;
                if (!px.empty() && di.width && di.height && uv.size() >= std::size_t(m.vertex_count) * 2 &&
                    px.size() >= std::size_t(di.width) * di.height * 4) {
                    const float tu = ml->anim.uv_tiling[0] != 0.0f ? ml->anim.uv_tiling[0] : 1.0f, tv = ml->anim.uv_tiling[1] != 0.0f ? ml->anim.uv_tiling[1] : 1.0f;
                    const float mul = ml->raw_record.empty() ? 1.0f : ml->anim.bright_mult, add = ml->raw_record.empty() ? 0.0f : ml->anim.midtone_offset;
                    const int sel = ch == 1 ? 3 : ch - 2;
                    m.vertex_colors.assign(std::size_t(m.vertex_count) * 4, 255);
                    for (std::uint32_t v = 0; v < m.vertex_count; ++v) {
                        float fu = uv[std::size_t(v) * 2] * tu, fv = uv[std::size_t(v) * 2 + 1] * tv;
                        fu -= std::floor(fu); fv -= std::floor(fv);
                        const std::uint32_t x = std::min(di.width - 1, std::uint32_t(fu * float(di.width)));
                        const std::uint32_t y = std::min(di.height - 1, std::uint32_t(fv * float(di.height)));
                        const float a = std::clamp(float(px[(std::size_t(y) * di.width + x) * 4 + std::size_t(sel)]) / 255.0f * mul + add, 0.0f, 1.0f);
                        m.vertex_colors[std::size_t(v) * 4 + 3] = std::uint8_t(a * 255.0f + 0.5f);
                    }
                    m.vertex_flags |= 0x200u;
                    m.cliff_skin_opaque = false;
                }
            }
            it = cf.pieces.emplace(key, std::move(m)).first;
        }
        return &it->second;
    };

    auto emit = [&](int si, const std::string& code_in, float wx, float wy, int mn, int steps, float xy, int var) {
        std::string code = code_in;
        const formats::Model* piece = load_piece(si, code, var);
        if (piece->vertex_count == 0) {
            char lo = 'Z';
            bool plain = true;
            for (char ch : code) { plain = plain && ch >= 'A' && ch <= 'D'; lo = std::min(lo, ch); }
            if (plain && lo > 'A') {
                std::string rel = code;
                for (char& ch : rel) ch = char('A' + (ch - lo));
                const formats::Model* alt = load_piece(si, rel, var);
                if (alt->vertex_count) { piece = alt; code = rel; }
            }
        }
        const SetInfo& S = setinfo[std::size_t(si)];
        if (piece->vertex_count == 0) {
            ++cf.missing_pieces;
            cf.missing_codes.insert(code);
            return;
        }
        render::SceneInstance inst;
        inst.model = piece;
        int span = 1;
        {
            char lo = 'Z', hi = 'A';
            bool plain = true;
            for (char ch : code) { plain = plain && ch >= 'A' && ch <= 'D'; lo = std::min(lo, ch); hi = std::max(hi, ch); }
            if (plain) span = std::max(1, int(hi - lo));
            else for (char ch : code) span = std::max(span, int(ch - 'A'));
        }
        bool abs_code = true;
        for (char ch : code) abs_code = abs_code && ch >= 'A' && ch <= 'D';
        float base_z = tier_top(mn);
        float shear[3] = {0.0f, 0.0f, 0.0f};
        bool on_pure = false;
        if (!std::getenv("SC2_CLIFF_TIER_BASE")) {
            on_pure = !sculpt_of_level.empty();
            if (on_pure) base_z = pure_top(mn);
            const float off = formats::engine_z_offset(map);
            const float stdh = map.quantize.found ? float(map.quantize.standard_height) : 0.0f;
            const bool has_sculpt = map.heightmap.width > 0 && map.heightmap.height > 0 && map.quantize.found;
            const float h = xy >= 0.75f ? 1.0f : 0.5f;
            const int n = xy >= 0.75f ? 3 : 2;
            float lowest = 1e30f, sx = 0.0f, sy = 0.0f, sm = 0.0f, sxx = 0.0f;
            int cnt = 0;
            for (int j = 0; j < n; ++j)
                for (int i = 0; i < n; ++i) {
                    const float dx = n == 3 ? float(i - 1) : (i ? h : -h), dy = n == 3 ? float(j - 1) : (j ? h : -h);
                    const int vx = int(std::lround(wx + dx)), vy = int(std::lround(wy + dy));
                    if (vx < 0 || vy < 0 || vx > int(cm.width) || vy > int(cm.height)) continue;
                    const float delta = has_sculpt ? formats::map_sculpt_height(map, std::uint32_t(vx), std::uint32_t(vy)) - stdh : 0.0f;
                    bool uniform = true;
                    if (on_pure)
                        for (int oy = -1; oy <= 0 && uniform; ++oy)
                            for (int ox = -1; ox <= 0 && uniform; ++ox) {
                                const int cx2 = std::clamp(vx + ox, 0, int(cm.width) - 1), cy2 = std::clamp(vy + oy, 0, int(cm.height) - 1);
                                const std::size_t k = std::size_t(cy2) * cm.width + std::size_t(cx2);
                                uniform = int(cm.levels[k]) == lvl(std::clamp(vx, 0, int(cm.width) - 1), std::clamp(vy, 0, int(cm.height) - 1)) &&
                                          !(have_raw && (cm.raw[k] & 63));
                            }
                    if (uniform) lowest = std::min(lowest, formats::vertex_ground_z(map, tier_z, off, std::uint32_t(vx), std::uint32_t(vy)) - delta);
                    sx += dx * delta; sy += dy * delta; sm += delta; sxx += dx * dx;
                    ++cnt;
                }
            const float block_lowest = lowest;
            float wide_lowest = lowest;
            if (cnt && n == 3 && !std::getenv("SC2_CLIFF_BASE_BLOCK_ONLY"))
                for (int j = -2; j <= 2; ++j)
                    for (int i = -2; i <= 2; ++i) {
                        const int vx = int(std::lround(wx)) + i, vy = int(std::lround(wy)) + j;
                        if (vx < 1 || vy < 1 || vx >= int(cm.width) || vy >= int(cm.height)) continue;
                        bool low = true;
                        for (int oy = -1; oy <= 0 && low; ++oy)
                            for (int ox = -1; ox <= 0 && low; ++ox) {
                                const std::size_t k = std::size_t(vy + oy) * cm.width + std::size_t(vx + ox);
                                low = int(cm.levels[k]) == mn && !(cm.raw.size() == cm.levels.size() && (cm.raw[k] & 63));
                            }
                        if (!low) continue;
                        const float delta = has_sculpt ? formats::map_sculpt_height(map, std::uint32_t(vx), std::uint32_t(vy)) - stdh : 0.0f;
                        wide_lowest = std::min(wide_lowest, formats::vertex_ground_z(map, tier_z, off, std::uint32_t(vx), std::uint32_t(vy)) - delta);
                    }
            static const float kDrop = std::getenv("SC2_CLIFF_BASE_DROP") ? float(std::atof(std::getenv("SC2_CLIFF_BASE_DROP"))) : 3.25f;
            if (std::getenv("SC2_CLIFF_PIECE_DUMP") && block_lowest - wide_lowest > 0.5f)
                std::fprintf(stderr, "[base] %s at %.1f %.1f mn %d block %.2f wide %.2f table %.2f\n", code.c_str(), double(wx), double(wy), mn, double(block_lowest), double(wide_lowest), double(base_z));
            if (abs_code && code.find('A') != std::string::npos && block_lowest - wide_lowest <= kDrop) lowest = wide_lowest;
            if (cnt) {
                if (lowest < base_z - 0.5f) base_z = lowest;
                if (sxx > 1e-6f) { shear[0] = sx / sxx; shear[1] = sy / sxx; }
                shear[2] = sm / float(cnt);
            }
        }
        const float top_z = !on_pure ? (span > 1 ? tier_top(mn + span) : tier_top(mn) + tier_rise(mn))
                            : span > 1 || std::size_t(mn) + 1 < tier_z.size() ? pure_top(mn + span)
                                                                               : pure_top(mn) + tier_rise(mn);
        const float rise = top_z - base_z;
        const float native = abs_code || span > 1 || S.native <= 0.0f ? piece->bounds_max[2] : S.native;
        const float zscale = native > 1e-3f ? (rise > 1e-3f ? rise : tier_h) / native : 1.0f;
        inst.world = render::translation(wx, wy, base_z) *
                     render::rotation_z(sign * float(steps) * 1.5707963f) *
                     render::scaling(xy, xy, zscale);
        inst.world.m[2] += shear[0] * inst.world.m[0] + shear[1] * inst.world.m[1];
        inst.world.m[6] += shear[0] * inst.world.m[4] + shear[1] * inst.world.m[5];
        inst.world.m[14] += shear[2];
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
        int mn = t[0], mx = t[0];
        for (int k = 1; k < 4; ++k) { mn = t[k] < mn ? t[k] : mn; mx = t[k] > mx ? t[k] : mx; }
        int shift = mx > 3 ? mx - 3 : 0;
        if (mn - shift < 0 || std::getenv("SC2_CLIFF_RELATIVE_CODES")) shift = mn;
        code.clear();
        for (int k = 0; k < 4; ++k) code += char('A' + (t[(k + best_r) & 3] - shift));
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
        static const bool kOldFlanks = std::getenv("SC2_RAMP_OLD_FLANKS") != nullptr;
        auto emit_ramp = [&](int si, const formats::RampFrame& f, bool left, int var, const formats::RampFrame& mid) {
            if (!f.used()) return;
            const float sgn_r[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
            const float sgn_u[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
            const float ramp_side = left ? 1.0f : -1.0f;
            struct Miss { CliffField& cf; bool armed = true; ~Miss() { if (armed) ++cf.unresolved_ramp_frames; } } miss{cf};
            if (f.w <= 0.0f || f.h <= 0.0f) return;
            int lv[4];
            bool on_deck[4];
            float corner_z[4];
            const float zoff = formats::engine_z_offset(map);
            for (int k = 0; k < 4; ++k) {
                const float x = f.cx + sgn_r[k] * f.w * f.rx + sgn_u[k] * f.h * f.ux;
                const float y = f.cy + sgn_r[k] * f.w * f.ry + sgn_u[k] * f.h * f.uy;
                lv[k] = vlvl(int(std::lround(x)), int(std::lround(y)));
                if (lv[k] < 0) return;
                on_deck[k] = sgn_r[k] == ramp_side;
                if (!kOldFlanks && mid.used() && mid.w > 0.0f)
                    on_deck[k] = std::fabs((x - mid.cx) * mid.rx + (y - mid.cy) * mid.ry) <= mid.w + 0.01f;
                corner_z[k] = formats::vertex_ground_z(map, tier_z, zoff, std::uint32_t(std::lround(x)), std::uint32_t(std::lround(y)));
            }
            int lo = lv[0];
            for (int k = 1; k < 4; ++k) lo = lv[k] < lo ? lv[k] : lo;
            const int base = lo > 0 ? lo - 1 : 0;
            std::string letters(4, ' ');
            for (int k = 0; k < 4; ++k) {
                const int rel = lv[k] - base;
                if (on_deck[k]) {
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
                    if (on_deck[k]) continue;
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
                float base_z = tier_top(lo) - zscale * piece->bounds_min[2], zs = zscale;
                float fshear[3] = {0.0f, 0.0f, 0.0f};
                if (!kOldFlanks) {
                    float cz[4] = {corner_z[0], corner_z[1], corner_z[2], corner_z[3]};
                    static const bool kFlankSculptOld = std::getenv("SC2_CLIFF_SCULPT_TWICE") != nullptr;
                    const bool has_sculpt = !kFlankSculptOld && map.heightmap.width > 0 && map.heightmap.height > 0 && map.quantize.found;
                    if (has_sculpt) {
                        const float stdh = float(map.quantize.standard_height);
                        auto delta_at = [&](float x, float y) {
                            const long vx = std::lround(x), vy = std::lround(y);
                            if (vx < 0 || vy < 0 || vx > long(cm.width) || vy > long(cm.height)) return 0.0f;
                            return formats::map_sculpt_height(map, std::uint32_t(vx), std::uint32_t(vy)) - stdh;
                        };
                        for (int k = 0; k < 4; ++k) {
                            const float x = f.cx + sgn_r[k] * f.w * f.rx + sgn_u[k] * f.h * f.ux;
                            const float y = f.cy + sgn_r[k] * f.w * f.ry + sgn_u[k] * f.h * f.uy;
                            cz[k] -= delta_at(x, y);
                        }
                        const int ex = int(std::ceil(std::max(std::fabs(f.w * f.rx) + std::fabs(f.h * f.ux), std::fabs(f.w * f.ry) + std::fabs(f.h * f.uy))));
                        float sx = 0.0f, sy = 0.0f, sm = 0.0f, sxx = 0.0f, syy = 0.0f;
                        int cnt = 0;
                        for (int j = -ex; j <= ex; ++j)
                            for (int i = -ex; i <= ex; ++i) {
                                const float d = delta_at(f.cx + float(i), f.cy + float(j));
                                sx += float(i) * d; sy += float(j) * d; sm += d; sxx += float(i * i); syy += float(j * j);
                                ++cnt;
                            }
                        if (cnt) {
                            fshear[0] = sxx > 0.0f ? sx / sxx : 0.0f;
                            fshear[1] = syy > 0.0f ? sy / syy : 0.0f;
                            fshear[2] = sm / float(cnt);
                        }
                    }
                    const float zlo = std::min(std::min(cz[0], cz[1]), std::min(cz[2], cz[3]));
                    const float zhi = std::max(std::max(cz[0], cz[1]), std::max(cz[2], cz[3]));
                    base_z = zlo;
                    zs = hi > lo && zhi - zlo > 1e-3f ? (zhi - zlo) / (2.0f * float(hi - lo)) : 1.0f;
                }
                inst.world = render::translation(f.cx, f.cy, base_z) *
                             render::rotation_z(std::atan2(f.ry, f.rx) +
                                                sign * float(yaw) * 1.5707963f) *
                             render::scaling(1.0f, 1.0f, zs);
                inst.world.m[2] += fshear[0] * inst.world.m[0] + fshear[1] * inst.world.m[1];
                inst.world.m[6] += fshear[0] * inst.world.m[4] + fshear[1] * inst.world.m[5];
                inst.world.m[14] += fshear[2];
                inst.team = {0, 0, 0, -1.0f};
                inst.seq = -1;
                if (std::getenv("SC2_RAMP_TRACE"))
                    std::fprintf(stderr, "[ramp] %s c=(%.1f,%.1f) lv=%d%d%d%d read=%s code=%s yaw=%d verts=%u bounds x %.2f..%.2f y %.2f..%.2f z %.2f..%.2f zscale %.3f base %.2f\n",
                                 left ? "L" : "R", double(f.cx), double(f.cy), lv[0], lv[1], lv[2], lv[3], letters.c_str(), code.c_str(), yaw, piece->vertex_count,
                                 double(piece->bounds_min[0]), double(piece->bounds_max[0]), double(piece->bounds_min[1]), double(piece->bounds_max[1]),
                                 double(piece->bounds_min[2]), double(piece->bounds_max[2]), double(zs), double(base_z));
                cf.instances.push_back(inst);
                ++cf.ramp_pieces;
                miss.armed = false;
                return;
            }
        };
        for (const formats::TerrainRamp& r : map.ramps) {
            emit_ramp(int(r.cid), r.left_lo, true, int(r.left_lo_var), r.mid);
            emit_ramp(int(r.cid), r.right_lo, false, int(r.right_lo_var), r.mid);
            emit_ramp(int(r.cid), r.left_hi, true, int(r.left_hi_var), r.mid);
            emit_ramp(int(r.cid), r.right_hi, false, int(r.right_hi_var), r.mid);
        }
    }

    if (!std::getenv("SC2_CLIFF_NO_CONFORM") && !std::getenv("SC2_RAMP_OLD_FLANKS")) {
        constexpr float kSnap = 0.25f;
        const float zoff = formats::engine_z_offset(map);
        const float FW = float(cm.width), FH = float(cm.height);
        auto field_z = [&](float x, float y) {
            x = std::clamp(x, 0.0f, FW); y = std::clamp(y, 0.0f, FH);
            const std::uint32_t c = std::min(std::uint32_t(x), cm.width - 1), r = std::min(std::uint32_t(y), cm.height - 1);
            const float fx = x - float(c), fy = y - float(r);
            const float za = formats::vertex_ground_z(map, tier_z, zoff, c, r), zb = formats::vertex_ground_z(map, tier_z, zoff, c + 1, r);
            const float zd = formats::vertex_ground_z(map, tier_z, zoff, c, r + 1), ze = formats::vertex_ground_z(map, tier_z, zoff, c + 1, r + 1);
            return fx + fy <= 1.0f ? za + fx * (zb - za) + fy * (zd - za) : ze + (1.0f - fx) * (zd - ze) + (1.0f - fy) * (zb - ze);
        };
        for (std::size_t i = 0; i < cf.instances.size(); ++i) {
            render::SceneInstance& in = cf.instances[i];
            if (!in.model || !cliff_piece_is_ground(*in.model)) continue;
            const formats::Model& src = *in.model;
            const float* M = in.world.m.data();
            if (std::fabs(M[10]) < 1e-4f) continue;
            formats::Model copy;
            bool moved = false;
            for (std::uint32_t v = 0; v < src.vertex_count; ++v) {
                if (src.vertex_colors[std::size_t(v) * 4 + 3] > 2) continue;
                const float* p = &src.positions[std::size_t(v) * 3];
                const float wx = M[0] * p[0] + M[4] * p[1] + M[8] * p[2] + M[12];
                const float wy = M[1] * p[0] + M[5] * p[1] + M[9] * p[2] + M[13];
                const float wz = M[2] * p[0] + M[6] * p[1] + M[10] * p[2] + M[14];
                const float fz = field_z(wx, wy);
                if (std::fabs(fz - wz) > kSnap || std::fabs(fz - wz) < 1e-4f) continue;
                if (!moved) { copy = src; moved = true; }
                copy.positions[std::size_t(v) * 3 + 2] = (fz - M[2] * p[0] - M[6] * p[1] - M[14]) / M[10];
            }
            if (moved) in.model = &cf.pieces.emplace("#conf" + std::to_string(i), std::move(copy)).first->second;
        }
    }
    if (std::getenv("SC2_CLIFF_PIECE_DUMP")) {
        for (const auto& kv : cf.pieces) {
            const formats::Model* pm = &kv.second;
            const char* code = kv.first.c_str();
            std::fprintf(stderr, "[piece] %s verts=%u idx=%zu flags=%x\n", code, pm->vertex_count, pm->indices.size(), unsigned(pm->vertex_flags));
            for (std::uint32_t v = 0; v < pm->vertex_count; ++v)
                std::fprintf(stderr, "[pv] %s %u %.4f %.4f %.4f %d\n", code, v, double(pm->positions[v * 3]), double(pm->positions[v * 3 + 1]), double(pm->positions[v * 3 + 2]),
                             pm->vertex_colors.size() >= std::size_t(pm->vertex_count) * 4 ? int(pm->vertex_colors[std::size_t(v) * 4 + 3]) : -1);
            for (const formats::M3Region& pr : pm->regions)
                for (std::uint32_t t = 0; t + 2 < pr.num_indices; t += 3)
                    std::fprintf(stderr, "[pt] %s %u %u %u\n", code, pr.first_vertex + pm->indices[pr.first_index + t], pr.first_vertex + pm->indices[pr.first_index + t + 1],
                                 pr.first_vertex + pm->indices[pr.first_index + t + 2]);
        }
        for (std::size_t i = 0; i < cf.instances.size(); ++i) {
            const render::SceneInstance& in = cf.instances[i];
            const char* key = "?";
            for (const auto& kv : cf.pieces) if (&kv.second == in.model) key = kv.first.c_str();
            std::fprintf(stderr, "[cpl] %s %d", key, int(i >= cf.first_ramp_instance));
            for (int k = 0; k < 16; ++k) std::fprintf(stderr, " %.4f", double(in.world.m[std::size_t(k)]));
            std::fprintf(stderr, "\n");
        }
    }
    if (const char* only = std::getenv("SC2_CLIFF_ONLY")) {
        const std::string o = only;
        if (o == "walls") cf.instances.resize(cf.first_ramp_instance);
        else if (o == "ramps") { cf.instances.erase(cf.instances.begin(), cf.instances.begin() + std::ptrdiff_t(cf.first_ramp_instance)); cf.first_ramp_instance = 0; }
        else if (o == "none") { cf.instances.clear(); cf.first_ramp_instance = 0; }
    }
    return cf;
}

inline bool cliff_piece_is_ground(const formats::Model& m) {
    return m.vertex_alpha_coverage && !m.cliff_skin_opaque && m.vertex_colors.size() >= std::size_t(m.vertex_count) * 4;
}

inline std::vector<std::uint8_t> cliff_ground_cells(const CliffField& cf, const formats::LoadedMap& map) {
    const std::uint32_t W = map.cliff.width, H = map.cliff.height;
    std::vector<std::uint8_t> cells(std::size_t(W) * H, 0);
    for (const render::SceneInstance& inst : cf.instances) {
        if (!inst.model || !cliff_piece_is_ground(*inst.model)) continue;
        const formats::Model& m = *inst.model;
        float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
        for (int k = 0; k < 4; ++k) {
            const float lx = (k & 1) ? m.bounds_max[0] : m.bounds_min[0], ly = (k & 2) ? m.bounds_max[1] : m.bounds_min[1];
            const float wx = inst.world.m[0] * lx + inst.world.m[4] * ly + inst.world.m[12];
            const float wy = inst.world.m[1] * lx + inst.world.m[5] * ly + inst.world.m[13];
            lo[0] = std::min(lo[0], wx); hi[0] = std::max(hi[0], wx);
            lo[1] = std::min(lo[1], wy); hi[1] = std::max(hi[1], wy);
        }
        for (int y = int(std::floor(lo[1] + 0.25f)); y < int(std::ceil(hi[1] - 0.25f)); ++y)
            for (int x = int(std::floor(lo[0] + 0.25f)); x < int(std::ceil(hi[0] - 0.25f)); ++x)
                if (x >= 0 && y >= 0 && x < int(W) && y < int(H)) cells[std::size_t(y) * W + std::size_t(x)] = 1;
    }
    if (!std::getenv("SC2_CLIFF_GROUND_BOX")) {
        std::vector<std::uint8_t> cover(cells.size(), 0);
        std::uint32_t gw = 0, gh = 0;
        std::vector<float> gz = formats::bake_ground_heights_vertex(map, gw, gh);
        const bool has_gz = gw == W && gh == H && gz.size() == cells.size();
        if (has_gz && !std::getenv("SC2_CLIFF_COVER_DRAWN")) {
            const std::vector<float> cth = formats::tier_heights(map);
            const float coff = formats::engine_z_offset(map);
            for (std::uint32_t y = 0; y < H; ++y)
                for (std::uint32_t x = 0; x < W; ++x) {
                    float lowc = 1e30f;
                    for (int i = 0; i < 4; ++i)
                        lowc = std::min(lowc, formats::vertex_ground_z(map, cth, coff, x + std::uint32_t(i & 1), y + std::uint32_t(i >> 1)));
                    float& g = gz[std::size_t(y) * W + x];
                    g = std::min(g, lowc);
                }
        }
        static constexpr float kOff[5][2] = {{0.5f, 0.5f}, {0.2f, 0.2f}, {0.8f, 0.2f}, {0.2f, 0.8f}, {0.8f, 0.8f}};
        for (const render::SceneInstance& inst : cf.instances) {
            if (!inst.model || !cliff_piece_is_ground(*inst.model)) continue;
            const formats::Model& m = *inst.model;
            const float* M = inst.world.m.data();
            for (const formats::M3Region& pr : m.regions) {
                if (std::size_t(pr.first_vertex) + pr.num_vertices > m.positions.size() / 3 ||
                    std::size_t(pr.first_index) + pr.num_indices > m.indices.size())
                    continue;
                for (std::uint32_t t = 0; t + 2 < pr.num_indices; t += 3) {
                    float q[3][3];
                    bool ok = true;
                    for (int k = 0; k < 3 && ok; ++k) {
                        const std::uint32_t vi = pr.first_vertex + m.indices[std::size_t(pr.first_index) + t + std::uint32_t(k)];
                        if (vi >= m.vertex_count) { ok = false; break; }
                        const float* p = &m.positions[std::size_t(vi) * 3];
                        q[k][0] = M[0] * p[0] + M[4] * p[1] + M[8] * p[2] + M[12];
                        q[k][1] = M[1] * p[0] + M[5] * p[1] + M[9] * p[2] + M[13];
                        q[k][2] = M[2] * p[0] + M[6] * p[1] + M[10] * p[2] + M[14];
                    }
                    if (!ok) continue;
                    const float area = (q[1][0] - q[0][0]) * (q[2][1] - q[0][1]) - (q[2][0] - q[0][0]) * (q[1][1] - q[0][1]);
                    if (std::fabs(area) < 1e-6f) continue;
                    const int x0 = std::max(0, int(std::floor(std::min({q[0][0], q[1][0], q[2][0]}))));
                    const int x1 = std::min(int(W) - 1, int(std::floor(std::max({q[0][0], q[1][0], q[2][0]}))));
                    const int y0 = std::max(0, int(std::floor(std::min({q[0][1], q[1][1], q[2][1]}))));
                    const int y1 = std::min(int(H) - 1, int(std::floor(std::max({q[0][1], q[1][1], q[2][1]}))));
                    for (int y = y0; y <= y1; ++y)
                        for (int x = x0; x <= x1; ++x) {
                            std::uint8_t& c = cover[std::size_t(y) * W + std::size_t(x)];
                            if (c == 31) continue;
                            for (int s = 0; s < 5; ++s) {
                                const float px = float(x) + kOff[s][0], py = float(y) + kOff[s][1];
                                const float e0 = ((q[1][0] - q[0][0]) * (py - q[0][1]) - (px - q[0][0]) * (q[1][1] - q[0][1])) / area;
                                const float e1 = ((q[2][0] - q[1][0]) * (py - q[1][1]) - (px - q[1][0]) * (q[2][1] - q[1][1])) / area;
                                const float e2 = ((q[0][0] - q[2][0]) * (py - q[2][1]) - (px - q[2][0]) * (q[0][1] - q[2][1])) / area;
                                if (e0 < -1e-3f || e1 < -1e-3f || e2 < -1e-3f) continue;
                                const float z = e0 * q[2][2] + e1 * q[0][2] + e2 * q[1][2];
                                if (has_gz && z < gz[std::size_t(y) * W + std::size_t(x)] - 1.2f) continue;
                                c = std::uint8_t(c | (1u << s));
                            }
                        }
                }
            }
        }
        for (std::size_t i = 0; i < cells.size(); ++i)
            if (cover[i] != 31) cells[i] = 0;
    }
    if (map.cliff.raw.size() == cells.size() && std::getenv("SC2_RAMP_OLD_FLANKS"))
        for (std::size_t i = 0; i < cells.size(); ++i)
            if (map.cliff.raw[i] & 63) cells[i] = 0;
    if (std::getenv("SC2_CLIFF_PIECE_DUMP"))
        for (std::size_t i = 0; i < cells.size(); ++i)
            if (cells[i]) std::fprintf(stderr, "[skip] %zu %zu\n", i % W, i / W);
    return cells;
}

inline std::size_t append_cliff_ground(formats::Model& ground, const CliffField& cf, const formats::LoadedMap& map) {
    if (ground.vertex_count == 0) return 0;
    if (!std::getenv("SC2_CLIFF_KEEP_UNDERLAY") && !std::getenv("SC2_CLIFF_NO_CONFORM") && !std::getenv("SC2_RAMP_OLD_FLANKS")) {
        const std::uint32_t CW = map.cliff.width, CH = map.cliff.height;
        std::vector<float> low;
        auto build_low = [&]() {
            low.assign(std::size_t(CW) * CH, 1e30f);
            static constexpr float kOff[5][2] = {{0.5f, 0.5f}, {0.2f, 0.2f}, {0.8f, 0.2f}, {0.2f, 0.8f}, {0.8f, 0.8f}};
            for (const render::SceneInstance& inst : cf.instances) {
                if (!inst.model || !cliff_piece_is_ground(*inst.model)) continue;
                const formats::Model& m = *inst.model;
                const float* M = inst.world.m.data();
                for (const formats::M3Region& pr : m.regions) {
                    if (std::size_t(pr.first_vertex) + pr.num_vertices > m.positions.size() / 3 || std::size_t(pr.first_index) + pr.num_indices > m.indices.size()) continue;
                    for (std::uint32_t t = 0; t + 2 < pr.num_indices; t += 3) {
                        float q[3][3];
                        bool ok = true;
                        for (int k = 0; k < 3 && ok; ++k) {
                            const std::uint32_t vi = pr.first_vertex + m.indices[std::size_t(pr.first_index) + t + std::uint32_t(k)];
                            if (vi >= m.vertex_count) { ok = false; break; }
                            const float* p = &m.positions[std::size_t(vi) * 3];
                            q[k][0] = M[0] * p[0] + M[4] * p[1] + M[8] * p[2] + M[12];
                            q[k][1] = M[1] * p[0] + M[5] * p[1] + M[9] * p[2] + M[13];
                            q[k][2] = M[2] * p[0] + M[6] * p[1] + M[10] * p[2] + M[14];
                        }
                        if (!ok) continue;
                        for (int k = 0; k < 3; ++k) {
                            const int vx = int(std::floor(q[k][0])), vy = int(std::floor(q[k][1]));
                            for (int oy = -1; oy <= 0; ++oy)
                                for (int ox = -1; ox <= 0; ++ox) {
                                    const int cx = vx + ox, cy = vy + oy;
                                    if (cx < 0 || cy < 0 || cx >= int(CW) || cy >= int(CH)) continue;
                                    if (q[k][0] < float(cx) - 1e-3f || q[k][0] > float(cx + 1) + 1e-3f || q[k][1] < float(cy) - 1e-3f || q[k][1] > float(cy + 1) + 1e-3f) continue;
                                    float& lz = low[std::size_t(cy) * CW + std::size_t(cx)];
                                    lz = std::min(lz, q[k][2]);
                                }
                        }
                        const float area = (q[1][0] - q[0][0]) * (q[2][1] - q[0][1]) - (q[2][0] - q[0][0]) * (q[1][1] - q[0][1]);
                        if (std::fabs(area) < 1e-6f) continue;
                        const int x0 = std::max(0, int(std::floor(std::min({q[0][0], q[1][0], q[2][0]}))));
                        const int x1 = std::min(int(CW) - 1, int(std::floor(std::max({q[0][0], q[1][0], q[2][0]}))));
                        const int y0 = std::max(0, int(std::floor(std::min({q[0][1], q[1][1], q[2][1]}))));
                        const int y1 = std::min(int(CH) - 1, int(std::floor(std::max({q[0][1], q[1][1], q[2][1]}))));
                        for (int y = y0; y <= y1; ++y)
                            for (int x = x0; x <= x1; ++x)
                                for (int sidx = 0; sidx < 5; ++sidx) {
                                    const float px = float(x) + kOff[sidx][0], py = float(y) + kOff[sidx][1];
                                    const float e0 = ((q[1][0] - q[0][0]) * (py - q[0][1]) - (px - q[0][0]) * (q[1][1] - q[0][1])) / area;
                                    const float e1 = ((q[2][0] - q[1][0]) * (py - q[1][1]) - (px - q[1][0]) * (q[2][1] - q[1][1])) / area;
                                    const float e2 = ((q[0][0] - q[2][0]) * (py - q[2][1]) - (px - q[2][0]) * (q[0][1] - q[2][1])) / area;
                                    if (e0 < -1e-3f || e1 < -1e-3f || e2 < -1e-3f) continue;
                                    float& lz = low[std::size_t(y) * CW + std::size_t(x)];
                                    lz = std::min(lz, e0 * q[2][2] + e1 * q[0][2] + e2 * q[1][2]);
                                }
                    }
                }
            }
        };
        for (std::size_t ri = ground.regions.size(); ri-- > 0;) {
            const formats::M3Region& rg = ground.regions[ri];
            const std::size_t nv = rg.num_vertices, ni = rg.num_indices;
            if (nv < 4 || nv % 4 || ni != nv / 4 * 6 || std::size_t(rg.first_vertex) + nv > ground.positions.size() / 3) break;
            bool quads = true;
            for (std::size_t q = 0; q < nv && quads; q += 4) {
                const float* a = &ground.positions[(std::size_t(rg.first_vertex) + q) * 3];
                quads = a[3] == a[0] + 1.0f && a[4] == a[1] && a[6] == a[0] && a[7] == a[1] + 1.0f && a[9] == a[0] + 1.0f && a[10] == a[1] + 1.0f &&
                        a[5] == a[2] && a[8] == a[2] && a[11] == a[2];
            }
            if (!quads) break;
            if (low.empty()) build_low();
            for (std::size_t q = 0; q < nv; q += 4) {
                float* a = &ground.positions[(std::size_t(rg.first_vertex) + q) * 3];
                const int cx = int(a[0]), cy = int(a[1]);
                if (cx < 0 || cy < 0 || cx >= int(CW) || cy >= int(CH)) continue;
                const float lz = low[std::size_t(cy) * CW + std::size_t(cx)];
                if (lz > 1e29f) continue;
                a[2] = a[5] = a[8] = a[11] = lz - 0.15f;
            }
        }
    }
    const float W = float(map.cliff.width), H = float(map.cliff.height);
    const bool has_normals = ground.normals.size() == ground.positions.size();
    constexpr float kSink = 0.02f;
    if (!std::getenv("SC2_CLIFF_NO_RIM_SNAP") && !std::getenv("SC2_CLIFF_NO_CONFORM") && !std::getenv("SC2_RAMP_OLD_FLANKS")) {
        const std::uint32_t VW = map.cliff.width + 1, VH = map.cliff.height + 1;
        std::vector<float> rim(std::size_t(VW) * VH, 1e30f);
        for (const render::SceneInstance& inst : cf.instances) {
            if (!inst.model || !cliff_piece_is_ground(*inst.model)) continue;
            const formats::Model& m = *inst.model;
            const float* M = inst.world.m.data();
            for (std::uint32_t v = 0; v < m.vertex_count; ++v) {
                if (m.vertex_colors[std::size_t(v) * 4 + 3] > 2) continue;
                const float* p = &m.positions[std::size_t(v) * 3];
                const float wx = M[0] * p[0] + M[4] * p[1] + M[8] * p[2] + M[12];
                const float wy = M[1] * p[0] + M[5] * p[1] + M[9] * p[2] + M[13];
                const float rx = std::round(wx), ry = std::round(wy);
                if (std::fabs(wx - rx) > 0.02f || std::fabs(wy - ry) > 0.02f || rx < 0.0f || ry < 0.0f || rx >= float(VW) || ry >= float(VH)) continue;
                float& r = rim[std::size_t(ry) * VW + std::size_t(rx)];
                r = std::min(r, M[2] * p[0] + M[6] * p[1] + M[10] * p[2] + M[14]);
            }
        }
        const float roff = formats::engine_z_offset(map);
        for (std::size_t i = 0; i + 2 < ground.positions.size(); i += 3) {
            const float rx = std::round(ground.positions[i]), ry = std::round(ground.positions[i + 1]);
            if (std::fabs(ground.positions[i] - rx) > 1e-4f || std::fabs(ground.positions[i + 1] - ry) > 1e-4f || rx < 0.0f || ry < 0.0f || rx >= float(VW) || ry >= float(VH)) continue;
            const float r = rim[std::size_t(ry) * VW + std::size_t(rx)];
            if (r > 1e29f) continue;
            const float sync = formats::map_vertex_height(map, std::uint32_t(rx), std::uint32_t(ry)) - roff;
            if (std::fabs(ground.positions[i + 2] - sync) > 1e-3f || std::fabs(r - sync) <= 0.25f) continue;
            ground.positions[i + 2] = r;
        }
    }
    std::size_t placed = 0;
    formats::M3Region rg;
    bool open = false;
    auto close = [&]() {
        if (!open) return;
        rg.num_vertices = std::uint32_t(ground.positions.size() / 3) - rg.first_vertex;
        rg.num_indices = std::uint32_t(ground.indices.size()) - rg.first_index;
        if (rg.num_indices) {
            ground.regions.push_back(rg);
            if (!ground.region_material.empty()) ground.region_material.push_back(ground.region_material.front());
        }
        open = false;
    };
    for (const render::SceneInstance& inst : cf.instances) {
        if (!inst.model || !cliff_piece_is_ground(*inst.model)) continue;
        const formats::Model& m = *inst.model;
        const float* M = inst.world.m.data();
        for (const formats::M3Region& pr : m.regions) {
            if (std::size_t(pr.first_vertex) + pr.num_vertices > m.positions.size() / 3 ||
                std::size_t(pr.first_index) + pr.num_indices > m.indices.size())
                continue;
            for (std::uint32_t t = 0; t + 2 < pr.num_indices; t += 3) {
                std::uint32_t vi[3];
                bool apron = false;
                for (int k = 0; k < 3; ++k) {
                    vi[k] = pr.first_vertex + m.indices[std::size_t(pr.first_index) + t + std::uint32_t(k)];
                    if (vi[k] >= m.vertex_count) { apron = false; vi[0] = UINT32_MAX; break; }
                    if (m.vertex_colors[std::size_t(vi[k]) * 4 + 3] < 250) apron = true;
                }
                if (!apron || vi[0] == UINT32_MAX) continue;
                if (!open || std::uint32_t(ground.positions.size() / 3) - rg.first_vertex + 3 > 60000u) {
                    close();
                    rg = formats::M3Region{};
                    rg.first_vertex = std::uint32_t(ground.positions.size() / 3);
                    rg.first_index = std::uint32_t(ground.indices.size());
                    open = true;
                }
                const std::uint16_t base = std::uint16_t(std::uint32_t(ground.positions.size() / 3) - rg.first_vertex);
                for (int k = 0; k < 3; ++k) {
                    const float* p = &m.positions[std::size_t(vi[k]) * 3];
                    const float wx = M[0] * p[0] + M[4] * p[1] + M[8] * p[2] + M[12];
                    const float wy = M[1] * p[0] + M[5] * p[1] + M[9] * p[2] + M[13];
                    const float wz = M[2] * p[0] + M[6] * p[1] + M[10] * p[2] + M[14];
                    static const bool kFlatSink = std::getenv("SC2_CLIFF_APRON_FLAT_SINK") != nullptr;
                    const float sink = kFlatSink ? kSink : kSink * float(m.vertex_colors[std::size_t(vi[k]) * 4 + 3]) / 255.0f;
                    ground.positions.insert(ground.positions.end(), {std::clamp(wx, 0.0f, W), std::clamp(wy, 0.0f, H), wz - sink});
                    ground.uvs.push_back(0.0f);
                    ground.uvs.push_back(0.0f);
                    if (has_normals) {
                        float n[3] = {0.0f, 0.0f, 1.0f};
                        if (m.normals.size() == m.positions.size()) {
                            const float* q = &m.normals[std::size_t(vi[k]) * 3];
                            n[0] = M[0] * q[0] + M[4] * q[1] + M[8] * q[2];
                            n[1] = M[1] * q[0] + M[5] * q[1] + M[9] * q[2];
                            n[2] = M[2] * q[0] + M[6] * q[1] + M[10] * q[2];
                            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                            if (nl > 1e-6f) { n[0] /= nl; n[1] /= nl; n[2] /= nl; } else { n[0] = 0.0f; n[1] = 0.0f; n[2] = 1.0f; }
                        }
                        ground.normals.insert(ground.normals.end(), {n[0], n[1], n[2]});
                    }
                }
                ground.indices.insert(ground.indices.end(), {base, std::uint16_t(base + 1), std::uint16_t(base + 2),
                                                             base, std::uint16_t(base + 2), std::uint16_t(base + 1)});
                ++placed;
            }
        }
    }
    close();
    ground.vertex_count = std::uint32_t(ground.positions.size() / 3);
    return placed;
}

inline std::size_t apply_cliff_vertex_shade(CliffField& cf, const formats::LoadedMap& map) {
    if (std::getenv("SC2_NO_CLIFF_SHADE")) return 0;
    std::size_t shaded = 0, idx = 0;
    for (const formats::VertexTint::Patch& vp : map.vertex_tint.patches) {
        ++idx;
        if (vp.plain() || vp.data.empty()) continue;
        bool painted = false;
        for (std::uint8_t b : vp.data)
            if (b != 255) { painted = true; break; }
        if (!painted) continue;
        for (render::SceneInstance& in : cf.instances) {
            const float px = in.world.m[12], py = in.world.m[13];
            if (px < vp.x0 - 0.01f || px > vp.x1 + 0.01f || py < vp.y0 - 0.01f || py > vp.y1 + 0.01f) continue;
            if (!in.model || in.model->vertex_count != vp.data.size() || !in.model->vertex_shade.empty()) continue;
            formats::Model copy = *in.model;
            copy.vertex_shade = vp.data;
            in.model = &cf.pieces.emplace("#shade" + std::to_string(idx), std::move(copy)).first->second;
            ++shaded;
            break;
        }
    }
    return shaded;
}

}
