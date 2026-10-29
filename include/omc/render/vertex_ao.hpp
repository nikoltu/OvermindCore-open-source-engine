#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

#include "omc/formats/m3.hpp"

namespace omc::render {

struct VertexAoParams {
    float reach = 1.0f;
    int rays = 32;
    bool ground = false;
    float strength = 1.0f;
};

inline void bake_vertex_ao(formats::Model& m, const VertexAoParams& prm) {
    const std::size_t nv = m.vertex_count;
    if (nv == 0 || m.positions.size() < nv * 3 || m.normals.size() < nv * 3 || prm.rays <= 0 || !(prm.reach > 0.0f)) return;
    struct Tri { float a[3], e1[3], e2[3], c[3], r; };
    std::vector<Tri> tris;
    for (const formats::M3Region& rg : m.regions)
        for (std::uint32_t t = 0; t + 2 < rg.num_indices; t += 3) {
            const std::uint32_t i0 = rg.first_vertex + m.indices[rg.first_index + t];
            const std::uint32_t i1 = rg.first_vertex + m.indices[rg.first_index + t + 1];
            const std::uint32_t i2 = rg.first_vertex + m.indices[rg.first_index + t + 2];
            if (i0 >= nv || i1 >= nv || i2 >= nv) continue;
            Tri tr;
            float rr = 0.0f;
            for (int k = 0; k < 3; ++k) {
                tr.a[k] = m.positions[i0 * 3 + k];
                tr.e1[k] = m.positions[i1 * 3 + k] - tr.a[k];
                tr.e2[k] = m.positions[i2 * 3 + k] - tr.a[k];
                tr.c[k] = tr.a[k] + (tr.e1[k] + tr.e2[k]) / 3.0f;
            }
            for (const std::uint32_t iv : {i0, i1, i2}) {
                float d2 = 0.0f;
                for (int k = 0; k < 3; ++k) {
                    const float d = m.positions[iv * 3 + k] - tr.c[k];
                    d2 += d * d;
                }
                rr = std::max(rr, d2);
            }
            tr.r = std::sqrt(rr);
            tris.push_back(tr);
        }
    if (tris.empty() || double(nv) * double(tris.size()) * double(prm.rays) > 6.0e9) return;
    std::vector<std::array<float, 3>> fan(std::size_t(prm.rays));
    for (int i = 0; i < prm.rays; ++i) {
        const float u = (float(i) + 0.5f) / float(prm.rays);
        const float r = std::sqrt(u), phi = 2.39996323f * float(i);
        fan[std::size_t(i)] = {r * std::cos(phi), r * std::sin(phi), std::sqrt(std::max(0.0f, 1.0f - u))};
    }
    m.vertex_ao.assign(nv, 255);
    const float eps = 1e-3f;
    for (std::size_t v = 0; v < nv; ++v) {
        float n[3] = {m.normals[v * 3], m.normals[v * 3 + 1], m.normals[v * 3 + 2]};
        const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (nl < 1e-6f) continue;
        for (float& c : n) c /= nl;
        const float h[3] = {std::fabs(n[0]) < 0.9f ? 1.0f : 0.0f, std::fabs(n[0]) < 0.9f ? 0.0f : 1.0f, 0.0f};
        float tx[3] = {h[1] * n[2] - h[2] * n[1], h[2] * n[0] - h[0] * n[2], h[0] * n[1] - h[1] * n[0]};
        const float tl = std::sqrt(tx[0] * tx[0] + tx[1] * tx[1] + tx[2] * tx[2]);
        for (float& c : tx) c /= tl;
        const float ty[3] = {n[1] * tx[2] - n[2] * tx[1], n[2] * tx[0] - n[0] * tx[2], n[0] * tx[1] - n[1] * tx[0]};
        float o[3];
        for (int k = 0; k < 3; ++k) o[k] = m.positions[v * 3 + k] + n[k] * eps * 4.0f;
        std::vector<const Tri*> near;
        for (const Tri& tr : tris) {
            float d2 = 0.0f;
            for (int k = 0; k < 3; ++k) {
                const float d = tr.c[k] - o[k];
                d2 += d * d;
            }
            const float lim = prm.reach + tr.r;
            if (d2 <= lim * lim) near.push_back(&tr);
        }
        int blocked = 0;
        for (const std::array<float, 3>& f : fan) {
            float d[3];
            for (int k = 0; k < 3; ++k) d[k] = tx[k] * f[0] + ty[k] * f[1] + n[k] * f[2];
            bool hit = false;
            if (prm.ground && d[2] < -1e-4f) {
                const float t = -o[2] / d[2];
                if (t > eps && t <= prm.reach) hit = true;
            }
            for (std::size_t q = 0; !hit && q < near.size(); ++q) {
                const Tri& tr = *near[q];
                const float p[3] = {d[1] * tr.e2[2] - d[2] * tr.e2[1], d[2] * tr.e2[0] - d[0] * tr.e2[2], d[0] * tr.e2[1] - d[1] * tr.e2[0]};
                const float det = tr.e1[0] * p[0] + tr.e1[1] * p[1] + tr.e1[2] * p[2];
                if (std::fabs(det) < 1e-9f) continue;
                const float inv = 1.0f / det;
                const float s[3] = {o[0] - tr.a[0], o[1] - tr.a[1], o[2] - tr.a[2]};
                const float bu = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * inv;
                if (bu < 0.0f || bu > 1.0f) continue;
                const float qv[3] = {s[1] * tr.e1[2] - s[2] * tr.e1[1], s[2] * tr.e1[0] - s[0] * tr.e1[2], s[0] * tr.e1[1] - s[1] * tr.e1[0]};
                const float bv = (d[0] * qv[0] + d[1] * qv[1] + d[2] * qv[2]) * inv;
                if (bv < 0.0f || bu + bv > 1.0f) continue;
                const float t = (tr.e2[0] * qv[0] + tr.e2[1] * qv[1] + tr.e2[2] * qv[2]) * inv;
                if (t > eps && t <= prm.reach) hit = true;
            }
            if (hit) ++blocked;
        }
        const float open = 1.0f - prm.strength * float(blocked) / float(prm.rays);
        m.vertex_ao[v] = std::uint8_t(std::clamp(open, 0.0f, 1.0f) * 255.0f + 0.5f);
    }
}

}
