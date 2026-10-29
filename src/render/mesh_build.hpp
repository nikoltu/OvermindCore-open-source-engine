#pragma once

#include <cstdlib>
#include <cstdint>
#include <vector>

#include "omc/formats/m3.hpp"
#include "omc/render/mat4.hpp"

namespace omc::render {

struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float weight[4];
    uint32_t bone[4];
    float uv1[2];
    uint32_t color = 0xffffffffu;
};

struct Draw {
    uint32_t first_index;
    uint32_t index_count;
    int material;
    bool overlay = false;
};

inline std::vector<uint32_t> resolve_indices(const formats::Model& m,
                                             std::vector<Draw>& draws) {
    std::vector<uint32_t> out;
    out.reserve(m.indices.size());
    const bool from_m3 = !m.material_refs.empty() && m.region_material.size() == m.regions.size();
    for (size_t ri = 0; ri < m.regions.size(); ++ri) {
        const formats::M3Region& r = m.regions[ri];
        uint32_t start = uint32_t(out.size());
        int mat = ri < m.region_material.size() ? m.region_material[ri] : -1;
        const bool untextured_by_design = mat == formats::kUntexturedRegion;
        const bool layerless = from_m3 && mat >= 0 && std::size_t(mat) < m.materials.size() && m.materials[std::size_t(mat)].layers.empty() &&
                               !std::getenv("OMC_DRAW_LAYERLESS");
        const bool suppress = (from_m3 && mat < 0 && !untextured_by_design) || layerless;
        if (!suppress) {
            uint32_t end = r.first_index + r.num_indices;
            for (uint32_t k = r.first_index; k < end && k < m.indices.size(); ++k)
                out.push_back(m.indices[k] + r.first_vertex);
        }
        draws.push_back({start, uint32_t(out.size()) - start, untextured_by_design ? -1 : mat, false});
    }
    static const bool first_only = std::getenv("OMC_COMPOSITE_FIRST_ONLY") != nullptr;
    if (from_m3 && !first_only && m.region_composite.size() == m.regions.size())
        for (size_t ri = 0; ri < m.regions.size(); ++ri) {
            const int ci = m.region_composite[ri];
            if (ci < 0 || size_t(ci) >= m.composites.size()) continue;
            bool first = true;
            for (const formats::M3CompositeChild& ch : m.composites[size_t(ci)].children) {
                if (!ch.resolved || ch.type != 1 || ch.index >= m.materials.size()) continue;
                if (first) {
                    first = false;
                    continue;
                }
                if (m.materials[ch.index].layers.empty() || ch.alpha <= 0.0f) continue;
                const uint32_t start = uint32_t(out.size());
                const formats::M3Region& r = m.regions[ri];
                for (uint32_t k = r.first_index; k < r.first_index + r.num_indices && k < m.indices.size(); ++k)
                    out.push_back(m.indices[k] + r.first_vertex);
                draws.push_back({start, uint32_t(out.size()) - start, int(ch.index), true});
            }
        }
    if (m.regions.empty()) {
        for (uint16_t i : m.indices) out.push_back(i);
        draws.push_back({0, uint32_t(out.size()), -1, false});
    }
    return out;
}

inline std::vector<Vertex> build_vertices(const formats::Model& m,
                                          const std::vector<uint32_t>& tri) {
    std::vector<Vertex> v(m.vertex_count);
    const uint32_t n_pos = uint32_t(m.positions.size() / 3) < m.vertex_count ? uint32_t(m.positions.size() / 3) : m.vertex_count;
    bool uv = m.uvs.size() >= size_t(m.vertex_count) * 2;
    bool skin = !m.bones.empty() &&
                m.vertex_bone_weights.size() >= size_t(m.vertex_count) * 4 &&
                m.vertex_bone_indices.size() >= size_t(m.vertex_count) * 4;
    std::vector<uint32_t> vregion(m.vertex_count, 0);
    for (size_t ri = 0; ri < m.regions.size(); ++ri) {
        const auto& r = m.regions[ri];
        for (uint32_t x = r.first_vertex;
             x < r.first_vertex + r.num_vertices && x < m.vertex_count; ++x)
            vregion[x] = uint32_t(ri);
    }
    static const bool kVertexColour = std::getenv("OMC_NO_VERTEX_COLOUR") == nullptr;
    std::vector<uint8_t> region_coloured(m.regions.size(), 0);
    if (kVertexColour && (!m.material_refs.empty() || m.placement_tint) && m.vertex_colors.size() >= size_t(m.vertex_count) * 4)
        for (size_t ri = 0; ri < m.regions.size() && ri < m.region_material.size(); ++ri) {
            const int mat = m.region_material[ri];
            if (mat >= 0 && size_t(mat) < m.materials.size() && (m.materials[size_t(mat)].flags & 0x1u) != 0) region_coloured[ri] = 1;
        }
    for (uint32_t i = 0; i < n_pos; ++i) {
        v[i] = {};
        if (!region_coloured.empty() && region_coloured[vregion[i]]) {
            const uint8_t* c = &m.vertex_colors[size_t(i) * 4];
            v[i].color = uint32_t(c[2]) | (uint32_t(c[1]) << 8) | (uint32_t(c[0]) << 16) | (uint32_t(c[3]) << 24);
        }
        v[i].color = (v[i].color & 0x00ffffffu) | (uint32_t(i < m.vertex_ao.size() ? m.vertex_ao[i] : 255u) << 24);
        v[i].pos[0] = m.positions[i * 3];
        v[i].pos[1] = m.positions[i * 3 + 1];
        v[i].pos[2] = m.positions[i * 3 + 2];
        if (uv) { v[i].uv[0] = m.uvs[i * 2]; v[i].uv[1] = m.uvs[i * 2 + 1]; }
        if (m.uvs1.size() >= size_t(m.vertex_count) * 2) { v[i].uv1[0] = m.uvs1[i * 2]; v[i].uv1[1] = m.uvs1[i * 2 + 1]; }
        if (skin) {
            uint32_t base =
                m.regions.empty() ? 0 : m.regions[vregion[i]].first_bone_lookup;
            float wsum = 0;
            for (int k = 0; k < 4; ++k) {
                float w = m.vertex_bone_weights[i * 4 + k] / 255.0f;
                v[i].weight[k] = w;
                wsum += w;
                uint32_t look = base + m.vertex_bone_indices[i * 4 + k];
                uint32_t bone =
                    look < m.bone_lookup.size() ? m.bone_lookup[look] : 0;
                v[i].bone[k] = bone < m.bones.size() ? bone : 0;
            }
            if (wsum > 0)
                for (int k = 0; k < 4; ++k) v[i].weight[k] /= wsum;
        }
        if (m.vertex_alpha_coverage && v[i].weight[3] == 0.0f && m.vertex_colors.size() >= size_t(i) * 4 + 4) {
            static const float kAlphaGain = std::getenv("OMC_CLIFF_ALPHA_GAIN") ? float(std::atof(std::getenv("OMC_CLIFF_ALPHA_GAIN"))) : 1.0f;
            const float a = std::min(255.0f, float(m.vertex_colors[size_t(i) * 4 + 3]) * kAlphaGain);
            v[i].bone[3] = 0x10000u | uint32_t(a + 0.5f);
        }
        if (i < m.vertex_shade.size() && m.vertex_shade[i] != 255 && v[i].weight[3] == 0.0f) {
            if (!(v[i].bone[3] & 0x10000u)) v[i].bone[3] = 0x10000u | 255u;
            v[i].bone[3] |= uint32_t(255 - m.vertex_shade[i]) << 8;
        }
    }
    const bool authored = m.normals.size() >= size_t(m.vertex_count) * 3;
    if (authored) {
        for (uint32_t i = 0; i < m.vertex_count; ++i) {
            Vec3 n = normalize({m.normals[i * 3], m.normals[i * 3 + 1], m.normals[i * 3 + 2]});
            v[i].nrm[0] = n.x; v[i].nrm[1] = n.y; v[i].nrm[2] = n.z;
        }
        return v;
    }
    for (size_t t = 0; t + 2 < tri.size(); t += 3) {
        uint32_t a = tri[t], b = tri[t + 1], c = tri[t + 2];
        if (a >= m.vertex_count || b >= m.vertex_count || c >= m.vertex_count)
            continue;
        Vec3 pa{v[a].pos[0], v[a].pos[1], v[a].pos[2]};
        Vec3 pb{v[b].pos[0], v[b].pos[1], v[b].pos[2]};
        Vec3 pc{v[c].pos[0], v[c].pos[1], v[c].pos[2]};
        Vec3 n = cross(sub(pb, pa), sub(pc, pa));
        for (uint32_t idx : {a, b, c}) {
            v[idx].nrm[0] += n.x; v[idx].nrm[1] += n.y; v[idx].nrm[2] += n.z;
        }
    }
    for (auto& vert : v) {
        Vec3 n = normalize({vert.nrm[0], vert.nrm[1], vert.nrm[2]});
        vert.nrm[0] = n.x; vert.nrm[1] = n.y; vert.nrm[2] = n.z;
    }
    return v;
}

inline std::string layer_of(const formats::Model& m, int mat, const char* usage) {
    if (mat < 0 || mat >= int(m.materials.size())) return {};
    for (const auto& l : m.materials[mat].layers)
        if (l.usage == usage) return l.image_path;
    return {};
}

inline const formats::M3Layer* layer_ptr_of(const formats::Model& m, int mat,
                                            const char* usage) {
    if (mat < 0 || mat >= int(m.materials.size())) return nullptr;
    for (const auto& l : m.materials[mat].layers)
        if (l.usage == usage) return &l;
    return nullptr;
}

inline std::string diffuse_of(const formats::Model& m, int mat) {
    return layer_of(m, mat, "Diffuse");
}

inline std::string normal_of(const formats::Model& m, int mat) {
    return layer_of(m, mat, "Normal");
}

inline std::string emissive_of(const formats::Model& m, int mat) {
    return layer_of(m, mat, "Emissive");
}

}
