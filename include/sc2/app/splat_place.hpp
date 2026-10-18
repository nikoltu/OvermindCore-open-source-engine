#pragma once
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <map>
#include <utility>
#include <vector>

#include "sc2/app/melee_models.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/formats/map_archive.hpp"

namespace sc2::app {

struct ProjectorItem {
    const formats::Model* pm;
    formats::PlacedDoodad d;
};

template <class GroundZ>
inline formats::Model build_projector_items(const std::vector<ProjectorItem>& items, float terrain_spec_exp, GroundZ&& ground_z,
                                            float terrain_spec_gain = 1.0f) {
    using Item = ProjectorItem;
    formats::Model m;
    std::map<std::pair<const formats::Model*, std::uint64_t>, int> material_of;
    static const bool kTint = std::getenv("SC2_NO_PLACEMENT_TINT") == nullptr;
    bool any_tint = false;
    std::vector<std::uint32_t> layers;
    for (const Item& it : items)
        for (const formats::M3Projector& p : it.pm->projectors)
            if (std::find(layers.begin(), layers.end(), p.layer) == layers.end()) layers.push_back(p.layer);
    std::sort(layers.begin(), layers.end(), std::greater<std::uint32_t>());
    std::uint32_t order = 0;
    constexpr int kMaxDiv = 48;
    static const bool kByLayer = std::getenv("SC2_PROJ_DRAW_ITEM_ORDER") == nullptr;
    static const bool kBakeFirst = std::getenv("SC2_PROJ_BAKE_MIXED") == nullptr;
    const std::size_t per_group = std::max<std::size_t>(layers.size(), 1);
    const std::size_t passes = kByLayer ? per_group * (kBakeFirst ? 2 : 1) : 1;
    for (std::size_t pass = 0; pass < passes; ++pass)
    for (const Item& item : items) {
        const formats::PlacedDoodad& d = item.d;
        const formats::Model* pm = item.pm;
        if (const char* sk = std::getenv("SC2_PROJ_SKIP"))
            if (d.type == sk) continue;
        for (std::size_t pi = 0; pi < pm->projectors.size(); ++pi) {
            const formats::M3Projector& p = pm->projectors[pi];
            const float w = p.x_right - p.x_left, l = p.y_back - p.y_front;
            if (p.type != 1 || !(w > 1e-4f) || !(l > 1e-4f)) continue;
            if (kByLayer && std::size_t(std::find(layers.begin(), layers.end(), p.layer) - layers.begin()) != pass % per_group) continue;
            if (kByLayer && kBakeFirst && (p.material_type == 9 ? 0u : 1u) != pass / per_group) continue;
            int mi = -1;
            const bool tinted = kTint && d.has_tint;
            const std::uint8_t tb[3] = {std::uint8_t(std::clamp(d.tint[0], 0.0f, 1.0f) * 255.0f + 0.5f), std::uint8_t(std::clamp(d.tint[1], 0.0f, 1.0f) * 255.0f + 0.5f),
                                        std::uint8_t(std::clamp(d.tint[2], 0.0f, 1.0f) * 255.0f + 0.5f)};
            const std::uint64_t mkey = std::uint64_t(pi) | (tinted ? ((std::uint64_t(tb[0]) << 16) | (std::uint64_t(tb[1]) << 24) | (std::uint64_t(tb[2]) << 32) |
                                                                     (std::uint64_t(std::clamp(int(d.tint_mult * 100.0f + 0.5f), 0, 4095)) << 40) | (std::uint64_t(1) << 60))
                                                                  : 0);
            if (auto it = material_of.find({pm, mkey}); it != material_of.end()) {
                mi = it->second;
            } else {
                if (std::getenv("SC2_SPLAT_TRACE"))
                    std::fprintf(stderr, "[splat] '%s' proj %zu type %u mat type %u index %u layer %u size %.2f x %.2f z [%.2f, %.2f] diffuse '%s' normal '%s' specular '%s'\n",
                                 pm->name.c_str(), pi, unsigned(p.type), unsigned(p.material_type), unsigned(p.material_index), unsigned(p.layer), double(w), double(l),
                                 double(p.z_bottom), double(p.z_top), p.diffuse.c_str(), p.normal.c_str(), p.specular.c_str());
                formats::M3Material mat;
                bool ok = false;
                if (p.material_type == 9 && !p.diffuse.empty()) {
                    mat.name = pm->name;
                    mat.blend_mode = formats::kBlendAlphaBlend;
                    formats::M3Layer dl;
                    dl.usage = "Diffuse";
                    dl.image_path = p.diffuse;
                    formats::M3Layer al;
                    al.usage = "AlphaMask";
                    al.image_path = p.diffuse;
                    al.word44 = 2;
                    mat.layers.push_back(std::move(dl));
                    if (std::getenv("SC2_SPLAT9_OPAQUE")) mat.blend_mode = formats::kBlendOpaque;
                    else mat.layers.push_back(std::move(al));
                    if (!p.normal.empty()) {
                        formats::M3Layer nl;
                        nl.usage = "Normal";
                        nl.image_path = p.normal;
                        mat.layers.push_back(std::move(nl));
                    }
                    if (!p.specular.empty() && terrain_spec_exp > 0.0f) {
                        formats::M3Layer sl;
                        sl.usage = "Specular";
                        sl.image_path = p.specular;
                        mat.layers.push_back(std::move(sl));
                        mat.specularity = terrain_spec_exp;
                        mat.spec_undimmed = std::getenv("SC2_PROJ_SPEC_DIMMED") == nullptr;
                        mat.spec_luma = std::getenv("SC2_PROJ_SPEC_COLOUR") == nullptr;
                        mat.spec_mult = terrain_spec_gain;
                        if (const char* sm = std::getenv("SC2_PROJ_SPEC_MULT")) mat.spec_mult = float(std::atof(sm));
                    }
                    ok = true;
                } else if (p.material_type == 1 && p.material_index < pm->materials.size()) {
                    mat = pm->materials[p.material_index];
                    if (!formats::material_is_additive(mat)) {
                        mat.blend_mode = formats::kBlendAlphaBlend;
                        if (formats::material_alpha_mask_channel(mat) == 0) {
                            std::string diffuse;
                            for (const formats::M3Layer& L : mat.layers)
                                if (L.usage == "Diffuse") diffuse = L.image_path;
                            mat.layers.erase(std::remove_if(mat.layers.begin(), mat.layers.end(),
                                                            [](const formats::M3Layer& L) { return L.usage == "AlphaMask"; }),
                                             mat.layers.end());
                            formats::M3Layer al;
                            al.usage = "AlphaMask";
                            al.image_path = diffuse;
                            al.word44 = 2;
                            mat.layers.push_back(std::move(al));
                        }
                    }
                    ok = true;
                    bool has_diffuse = false;
                    const formats::M3Layer* mask = nullptr;
                    for (const formats::M3Layer& L : mat.layers) {
                        if (L.usage == "Diffuse") has_diffuse = true;
                        if (L.usage == "AlphaMask") mask = &L;
                    }
                    if (!has_diffuse) {
                        if (formats::material_is_additive(mat) || !mask) {
                            ok = false;
                        } else {
                            formats::M3Layer dl;
                            dl.usage = "Diffuse";
                            const char* nd = std::getenv("SC2_PROJ_NODIFFUSE");
                            if ((nd && std::string(nd) == "init") || mask->image_path.empty()) {
                                dl.inline_w = dl.inline_h = 1;
                                dl.inline_rgba = {std::uint8_t(mask->anim.color[0] * 255.0f), std::uint8_t(mask->anim.color[1] * 255.0f),
                                                  std::uint8_t(mask->anim.color[2] * 255.0f), 255};
                            } else {
                                dl.image_path = mask->image_path;
                            }
                            mat.layers.insert(mat.layers.begin(), std::move(dl));
                        }
                    }
                }
                if (ok) {
                    if (tinted) {
                        mat.flags |= 0x1u;
                        for (formats::M3Layer& tl : mat.layers)
                            if (tl.usage == "Diffuse") tl.anim.bright_mult *= d.tint_mult;
                    }
                    mi = int(m.materials.size());
                    m.materials.push_back(std::move(mat));
                }
                material_of.emplace(std::make_pair(pm, mkey), mi);
            }
            if (mi < 0) continue;
            const auto bone_point = [&](float x, float y, float z, float out[3]) {
                float v[3] = {x, y, z};
                int b = int(p.bone);
                for (int guard = 0; b >= 0 && std::size_t(b) < pm->bones.size() && guard < 16; ++guard) {
                    const formats::M3Bone& bn = pm->bones[std::size_t(b)];
                    for (int a = 0; a < 3; ++a)
                        if (std::fabs(bn.scale.init[a]) > 1e-6f) v[a] *= bn.scale.init[a];
                    const float* q = bn.rotation.init;
                    if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3] > 1e-6f) {
                        const float c1[3] = {q[1] * v[2] - q[2] * v[1], q[2] * v[0] - q[0] * v[2], q[0] * v[1] - q[1] * v[0]};
                        const float c2[3] = {q[1] * c1[2] - q[2] * c1[1], q[2] * c1[0] - q[0] * c1[2], q[0] * c1[1] - q[1] * c1[0]};
                        for (int a = 0; a < 3; ++a) v[a] += 2.0f * (q[3] * c1[a] + c2[a]);
                    }
                    for (int a = 0; a < 3; ++a) v[a] += bn.location.init[a];
                    b = bn.parent;
                }
                for (int a = 0; a < 3; ++a) out[a] = v[a];
            };
            float e0[3], ex[3], ey[3];
            bone_point(p.x_left + p.offset[0], p.y_front + p.offset[1], 0.0f, e0);
            bone_point(p.x_right + p.offset[0], p.y_front + p.offset[1], 0.0f, ex);
            bone_point(p.x_left + p.offset[0], p.y_back + p.offset[1], 0.0f, ey);
            const float wx = std::hypot((ex[0] - e0[0]) * d.sx, (ex[1] - e0[1]) * d.sy);
            const float wy = std::hypot((ey[0] - e0[0]) * d.sx, (ey[1] - e0[1]) * d.sy);
            const int nx = std::clamp(int(std::ceil(wx)), 1, kMaxDiv), ny = std::clamp(int(std::ceil(wy)), 1, kMaxDiv);
            const float cr = std::cos(d.rotation), sr = std::sin(d.rotation);
            const std::size_t rank = std::size_t(std::find(layers.begin(), layers.end(), p.layer) - layers.begin()) +
                                     ((kByLayer && kBakeFirst && p.material_type != 9) ? per_group : 0);
            static const float kLiftScale = std::getenv("SC2_PROJ_LIFT_SCALE") ? float(std::atof(std::getenv("SC2_PROJ_LIFT_SCALE"))) : 0.2f;
            const float lift = (0.05f + 0.012f * float(rank) + 0.0015f * float(order++ % 8u)) * kLiftScale;
            formats::M3Region rg;
            rg.first_vertex = std::uint32_t(m.positions.size() / 3);
            rg.first_index = std::uint32_t(m.indices.size());
            for (int j = 0; j <= ny; ++j)
                for (int i = 0; i <= nx; ++i) {
                    const float tu = float(i) / float(nx), tv = float(j) / float(ny);
                    const float lx = (e0[0] + (ex[0] - e0[0]) * tu + (ey[0] - e0[0]) * tv) * d.sx;
                    const float ly = (e0[1] + (ex[1] - e0[1]) * tu + (ey[1] - e0[1]) * tv) * d.sy;
                    const float vx = d.x + cr * lx - sr * ly, vy = d.y + sr * lx + cr * ly;
                    m.positions.insert(m.positions.end(), {vx, vy, float(ground_z(vx, vy)) + lift});
                    m.normals.insert(m.normals.end(), {0.0f, 0.0f, 1.0f});
                    if (tinted) { m.vertex_colors.insert(m.vertex_colors.end(), {tb[2], tb[1], tb[0], std::uint8_t(255)}); any_tint = true; }
                    else m.vertex_colors.insert(m.vertex_colors.end(), {std::uint8_t(255), std::uint8_t(255), std::uint8_t(255), std::uint8_t(255)});
                    static const std::string kProjUv = std::getenv("SC2_PROJ_UV") ? std::getenv("SC2_PROJ_UV") : "";
                    m.uvs.push_back(kProjUv == "u" || kProjUv == "uv" ? 1.0f - tu : tu);
                    m.uvs.push_back(kProjUv == "v" || kProjUv == "uv" ? tv : 1.0f - tv);
                }
            static const bool kZClip = std::getenv("SC2_PROJ_NO_ZCLIP") == nullptr;
            float zlo = -1e30f, zhi = 1e30f;
            if (kZClip && p.z_top > p.z_bottom) {
                float c0[3], cb[3], ct[3];
                const float mx = 0.5f * (p.x_left + p.x_right) + p.offset[0], my = 0.5f * (p.y_front + p.y_back) + p.offset[1];
                bone_point(mx, my, 0.0f, c0);
                bone_point(mx, my, p.z_bottom, cb);
                bone_point(mx, my, p.z_top, ct);
                const float clx = c0[0] * d.sx, cly = c0[1] * d.sy;
                const float zc = float(ground_z(d.x + cr * clx - sr * cly, d.y + sr * clx + cr * cly));
                zlo = zc + std::min(cb[2] - c0[2], ct[2] - c0[2]) * d.sz - 0.25f;
                zhi = zc + std::max(cb[2] - c0[2], ct[2] - c0[2]) * d.sz + 0.25f;
            }
            const std::uint16_t rw = std::uint16_t(nx + 1);
            for (int j = 0; j < ny; ++j)
                for (int i = 0; i < nx; ++i) {
                    const std::uint16_t v0 = std::uint16_t(j * rw + i), v1 = std::uint16_t(v0 + 1);
                    const std::uint16_t v2 = std::uint16_t(v0 + rw), v3 = std::uint16_t(v2 + 1);
                    bool inside = true;
                    for (const std::uint16_t v : {v0, v1, v2, v3}) {
                        const float z = m.positions[(std::size_t(rg.first_vertex) + v) * 3 + 2] - lift;
                        inside = inside && z >= zlo && z <= zhi;
                    }
                    if (!inside) continue;
                    m.indices.insert(m.indices.end(), {v0, v1, v2, v1, v3, v2, v0, v2, v1, v1, v2, v3});
                }
            rg.num_vertices = std::uint32_t(m.positions.size() / 3) - rg.first_vertex;
            rg.num_indices = std::uint32_t(m.indices.size()) - rg.first_index;
            if (const char* st = std::getenv("SC2_SPLAT_TRACE"); st && st[0] == '@' && d.type == st + 1) {
                const std::size_t fv = rg.first_vertex, last = std::size_t(m.positions.size() / 3) - 1;
                std::fprintf(stderr, "[splatat] %s at %.2f %.2f z %.2f rot %.3f proj %zu grid %dx%d corners (%.2f %.2f %.2f) (%.2f %.2f %.2f) box [%.2f, %.2f] cells drawn %u of %d\n",
                             d.type.c_str(), double(d.x), double(d.y), double(d.z), double(d.rotation), pi, nx, ny, double(m.positions[fv * 3]),
                             double(m.positions[fv * 3 + 1]), double(m.positions[fv * 3 + 2]), double(m.positions[last * 3]), double(m.positions[last * 3 + 1]),
                             double(m.positions[last * 3 + 2]), double(zlo), double(zhi), rg.num_indices / 12u, nx * ny);
            }
            m.regions.push_back(rg);
            m.region_material.push_back(mi);
        }
    }
    m.vertex_count = std::uint32_t(m.positions.size() / 3);
    m.material_refs.clear();
    m.placement_tint = any_tint && m.vertex_colors.size() == std::size_t(m.vertex_count) * 4;
    if (!m.placement_tint) m.vertex_colors.clear();
    for (int a = 0; a < 3; ++a) { m.bounds_min[a] = 1e30f; m.bounds_max[a] = -1e30f; }
    for (std::uint32_t i = 0; i < m.vertex_count; ++i)
        for (int a = 0; a < 3; ++a) {
            m.bounds_min[a] = std::min(m.bounds_min[a], m.positions[i * 3 + a]);
            m.bounds_max[a] = std::max(m.bounds_max[a], m.positions[i * 3 + a]);
        }
    return m;
}

inline formats::Model build_map_projectors(casc::Storage& store, MeleeModels& models, const formats::LoadedMap& map,
                                           float terrain_spec_exp = 100.0f, float terrain_spec_gain = 1.0f) {
    if (map.cliff.empty()) return formats::Model{};
    if (const char* pe = std::getenv("SC2_PROJ_SPEC_EXP")) terrain_spec_exp = float(std::atof(pe));
    const std::vector<float> th = formats::tier_heights(map);
    const float off = formats::engine_z_offset(map);
    std::vector<ProjectorItem> items;
    for (const formats::PlacedDoodad& d : map.objects.doodads)
        if (const formats::Model* pm = models.doodad_projectors(store, d.type, d.variation)) items.push_back({pm, d});
    return build_projector_items(items, terrain_spec_exp, [&](float x, float y) { return formats::vertex_ground_bilinear(map, th, off, x, y); },
                                 terrain_spec_gain);
}

inline void append_ground_overlay(formats::Model& dst, const formats::Model& src) {
    if (!src.vertex_count) return;
    const std::uint32_t vbase = std::uint32_t(dst.positions.size() / 3), ibase = std::uint32_t(dst.indices.size());
    const int mbase = int(dst.materials.size());
    const bool first = dst.vertex_count == 0;
    dst.positions.insert(dst.positions.end(), src.positions.begin(), src.positions.end());
    dst.normals.insert(dst.normals.end(), src.normals.begin(), src.normals.end());
    dst.uvs.insert(dst.uvs.end(), src.uvs.begin(), src.uvs.end());
    dst.indices.insert(dst.indices.end(), src.indices.begin(), src.indices.end());
    dst.materials.insert(dst.materials.end(), src.materials.begin(), src.materials.end());
    for (std::size_t r = 0; r < src.regions.size(); ++r) {
        formats::M3Region rg = src.regions[r];
        rg.first_vertex += vbase;
        rg.first_index += ibase;
        dst.regions.push_back(rg);
        dst.region_material.push_back(r < src.region_material.size() && src.region_material[r] >= 0 ? src.region_material[r] + mbase : -1);
    }
    dst.vertex_count = std::uint32_t(dst.positions.size() / 3);
    if (src.placement_tint || dst.placement_tint) {
        dst.vertex_colors.resize(std::size_t(vbase) * 4, 255);
        if (src.vertex_colors.size() == std::size_t(src.vertex_count) * 4) dst.vertex_colors.insert(dst.vertex_colors.end(), src.vertex_colors.begin(), src.vertex_colors.end());
        dst.vertex_colors.resize(std::size_t(dst.vertex_count) * 4, 255);
        dst.placement_tint = true;
    }
    for (int a = 0; a < 3; ++a) {
        dst.bounds_min[a] = first ? src.bounds_min[a] : std::min(dst.bounds_min[a], src.bounds_min[a]);
        dst.bounds_max[a] = first ? src.bounds_max[a] : std::max(dst.bounds_max[a], src.bounds_max[a]);
    }
}

}
