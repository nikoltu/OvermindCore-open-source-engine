#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "omc/formats/m3.hpp"
#include "omc/render/mat4.hpp"

namespace omc::render {

inline formats::Model make_ring_model(float inner = 0.82f, int segs = 64) {
    formats::Model m;
    m.name = "selection_ring";
    if (segs < 8) segs = 8;
    if (inner < 0.0f) inner = 0.0f;
    if (inner > 0.98f) inner = 0.98f;
    const int n = segs;
    m.positions.reserve(std::size_t(n) * 6);
    for (int i = 0; i < n; ++i) {
        const float a = 6.2831853071795864f * float(i) / float(n);
        const float c = std::cos(a), s = std::sin(a);
        m.positions.push_back(c);          m.positions.push_back(s);          m.positions.push_back(0.0f);
        m.positions.push_back(c * inner);  m.positions.push_back(s * inner);  m.positions.push_back(0.0f);
    }
    m.vertex_count = std::uint32_t(n) * 2;
    m.stride = 12;
    for (int i = 0; i < n; ++i) {
        const std::uint16_t o0 = std::uint16_t((i * 2) % (n * 2));
        const std::uint16_t i0 = std::uint16_t((i * 2 + 1) % (n * 2));
        const std::uint16_t o1 = std::uint16_t(((i + 1) * 2) % (n * 2));
        const std::uint16_t i1 = std::uint16_t(((i + 1) * 2 + 1) % (n * 2));
        m.indices.push_back(o0); m.indices.push_back(i0); m.indices.push_back(o1);
        m.indices.push_back(o1); m.indices.push_back(i0); m.indices.push_back(i1);
    }
    formats::M3Region r;
    r.first_vertex = 0;
    r.num_vertices = m.vertex_count;
    r.first_index = 0;
    r.num_indices = std::uint32_t(m.indices.size());
    m.regions.push_back(r);
    m.region_material.push_back(-1);
    m.bounds_min[0] = m.bounds_min[1] = -1.0f;
    m.bounds_max[0] = m.bounds_max[1] = 1.0f;
    m.bounds_radius = 1.0f;
    return m;
}

inline formats::Model make_arc_ring_model(float inner, int arcs, float solid, int segs = 8) {
    formats::Model m;
    m.name = "selection_arc_ring";
    if (arcs < 1) arcs = 1;
    if (segs < 1) segs = 1;
    if (inner < 0.0f) inner = 0.0f;
    if (inner > 0.98f) inner = 0.98f;
    if (solid <= 0.0f) solid = 0.01f;
    if (solid > 1.0f) solid = 1.0f;
    const float slice = 6.2831853071795864f / float(arcs);
    const float span = slice * solid;
    for (int a = 0; a < arcs; ++a) {
        const float a0 = slice * float(a);
        const std::uint16_t base = std::uint16_t(m.positions.size() / 3);
        for (int i = 0; i <= segs; ++i) {
            const float ang = a0 + span * float(i) / float(segs);
            const float c = std::cos(ang), s = std::sin(ang);
            m.positions.push_back(c);         m.positions.push_back(s);         m.positions.push_back(0.0f);
            m.positions.push_back(c * inner); m.positions.push_back(s * inner); m.positions.push_back(0.0f);
        }
        for (int i = 0; i < segs; ++i) {
            const std::uint16_t o0 = std::uint16_t(base + i * 2), i0 = std::uint16_t(base + i * 2 + 1);
            const std::uint16_t o1 = std::uint16_t(base + (i + 1) * 2), i1 = std::uint16_t(base + (i + 1) * 2 + 1);
            m.indices.push_back(o0); m.indices.push_back(i0); m.indices.push_back(o1);
            m.indices.push_back(o1); m.indices.push_back(i0); m.indices.push_back(i1);
        }
    }
    m.vertex_count = std::uint32_t(m.positions.size() / 3);
    m.stride = 12;
    formats::M3Region r;
    r.first_vertex = 0;
    r.num_vertices = m.vertex_count;
    r.first_index = 0;
    r.num_indices = std::uint32_t(m.indices.size());
    m.regions.push_back(r);
    m.region_material.push_back(-1);
    m.bounds_min[0] = m.bounds_min[1] = -1.0f;
    m.bounds_max[0] = m.bounds_max[1] = 1.0f;
    m.bounds_radius = 1.0f;
    return m;
}

inline formats::Model make_crescent_model(float offset, float shrink = 0.0f, int segs = 96) {
    formats::Model m;
    m.name = "selection_crescent";
    if (segs < 8) segs = 8;
    if (offset < 0.001f) offset = 0.001f;
    if (offset > 0.45f) offset = 0.45f;
    const float rad = 1.0f - offset;
    for (int i = 0; i < segs; ++i) {
        const float a = 6.2831853071795864f * float(i) / float(segs);
        const float c = std::cos(a), s = std::sin(a);
        float in = offset * s + std::sqrt(rad * rad - offset * offset * c * c) + shrink;
        if (in > 1.0f) in = 1.0f;
        m.positions.push_back(c);       m.positions.push_back(s);       m.positions.push_back(0.0f);
        m.positions.push_back(c * in);  m.positions.push_back(s * in);  m.positions.push_back(0.0f);
    }
    m.vertex_count = std::uint32_t(segs) * 2;
    m.stride = 12;
    for (int i = 0; i < segs; ++i) {
        const std::uint16_t o0 = std::uint16_t((i * 2) % (segs * 2)), i0 = std::uint16_t((i * 2 + 1) % (segs * 2));
        const std::uint16_t o1 = std::uint16_t(((i + 1) * 2) % (segs * 2)), i1 = std::uint16_t(((i + 1) * 2 + 1) % (segs * 2));
        m.indices.push_back(o0); m.indices.push_back(i0); m.indices.push_back(o1);
        m.indices.push_back(o1); m.indices.push_back(i0); m.indices.push_back(i1);
    }
    formats::M3Region r;
    r.first_vertex = 0;
    r.num_vertices = m.vertex_count;
    r.first_index = 0;
    r.num_indices = std::uint32_t(m.indices.size());
    m.regions.push_back(r);
    m.region_material.push_back(-1);
    m.bounds_min[0] = m.bounds_min[1] = -1.0f;
    m.bounds_max[0] = m.bounds_max[1] = 1.0f;
    m.bounds_radius = 1.0f;
    return m;
}

inline formats::Model make_beam_model() {
    formats::Model m;
    const float v[4][3] = {{0.0f, -0.5f, 0.0f}, {1.0f, -0.5f, 0.0f},
                           {1.0f, 0.5f, 0.0f},  {0.0f, 0.5f, 0.0f}};
    for (const auto& p : v) {
        m.positions.push_back(p[0]);
        m.positions.push_back(p[1]);
        m.positions.push_back(p[2]);
    }
    const std::uint32_t idx[6] = {0, 1, 2, 0, 2, 3};
    for (std::uint32_t i : idx) m.indices.push_back(i);
    m.vertex_count = 4;
    m.stride = 12;
    formats::M3Region r;
    r.first_vertex = 0;
    r.num_vertices = 4;
    r.first_index = 0;
    r.num_indices = 6;
    m.regions.push_back(r);
    m.region_material.push_back(-1);
    m.bounds_min[0] = 0.0f;  m.bounds_min[1] = -0.5f;
    m.bounds_max[0] = 1.0f;  m.bounds_max[1] = 0.5f;
    m.bounds_radius = 1.2f;
    return m;
}

inline Mat4 beam_transform(float ax, float ay, float az, float bx, float by, float bz, float width) {
    const float dx = bx - ax, dy = by - ay, dz = bz - az;
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (len <= 1e-6f) return translation(ax, ay, az) * scaling(0.0f, 0.0f, 0.0f);
    const float ux = dx / len, uy = dy / len, uz = dz / len;
    float rx = uy, ry = -ux, rz = 0.0f;
    const float rlen = std::sqrt(rx * rx + ry * ry);
    if (rlen <= 1e-6f) { rx = 0.0f; ry = 1.0f; rz = 0.0f; }
    else { rx /= rlen; ry /= rlen; }
    Mat4 t = Mat4::identity();
    t.m[0] = ux * len;   t.m[1] = uy * len;   t.m[2] = uz * len;
    t.m[4] = rx * width; t.m[5] = ry * width; t.m[6] = rz * width;
    t.m[12] = ax;        t.m[13] = ay;        t.m[14] = az;
    return t;
}

}
