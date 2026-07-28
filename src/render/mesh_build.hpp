#pragma once

#include <cstdint>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"

namespace sc2::render {

struct Vertex {
    float pos[3];
    float nrm[3];
    float uv[2];
    float weight[4];
    uint32_t bone[4];
};

struct Draw {
    uint32_t first_index;
    uint32_t index_count;
    int material;
};

inline std::vector<uint32_t> resolve_indices(const formats::Model& m,
                                             std::vector<Draw>& draws) {
    std::vector<uint32_t> out;
    out.reserve(m.indices.size());
    for (size_t ri = 0; ri < m.regions.size(); ++ri) {
        const formats::M3Region& r = m.regions[ri];
        uint32_t start = uint32_t(out.size());
        uint32_t end = r.first_index + r.num_indices;
        for (uint32_t k = r.first_index; k < end && k < m.indices.size(); ++k)
            out.push_back(m.indices[k] + r.first_vertex);
        int mat = ri < m.region_material.size() ? m.region_material[ri] : -1;
        draws.push_back({start, uint32_t(out.size()) - start, mat});
    }
    if (m.regions.empty()) {
        for (uint16_t i : m.indices) out.push_back(i);
        draws.push_back({0, uint32_t(out.size()), -1});
    }
    return out;
}

inline std::vector<Vertex> build_vertices(const formats::Model& m,
                                          const std::vector<uint32_t>& tri) {
    std::vector<Vertex> v(m.vertex_count);
    bool uv = m.uvs.size() >= size_t(m.vertex_count) * 2;
    bool skin = !m.bones.empty() &&
                m.vertex_bone_weights.size() >= size_t(m.vertex_count) * 4;
    std::vector<uint32_t> vregion(m.vertex_count, 0);
    for (size_t ri = 0; ri < m.regions.size(); ++ri) {
        const auto& r = m.regions[ri];
        for (uint32_t x = r.first_vertex;
             x < r.first_vertex + r.num_vertices && x < m.vertex_count; ++x)
            vregion[x] = uint32_t(ri);
    }
    for (uint32_t i = 0; i < m.vertex_count; ++i) {
        v[i] = {};
        v[i].pos[0] = m.positions[i * 3];
        v[i].pos[1] = m.positions[i * 3 + 1];
        v[i].pos[2] = m.positions[i * 3 + 2];
        if (uv) { v[i].uv[0] = m.uvs[i * 2]; v[i].uv[1] = m.uvs[i * 2 + 1]; }
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
