#pragma once

#include <cmath>
#include <cstdint>
#include <vector>

#include "sc2/formats/m3.hpp"

namespace sc2::render {

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

}
