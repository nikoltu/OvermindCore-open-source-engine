#include "sc2/app/synth_map.hpp"

#include "sc2/formats/map_archive.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>

namespace sc2::app {

namespace {

constexpr double kPi = 3.14159265358979323846;

render::Vec3 mirror(const render::Vec3& p, float w, float h) {
    return {w - p.x, h - p.y, p.z};
}

formats::Model ground_grid(float w, float h, int subdiv) {
    if (subdiv < 1) subdiv = 1;
    const int n = subdiv + 1;

    formats::Model m;
    m.vertex_count = uint32_t(n) * uint32_t(n);
    m.positions.reserve(size_t(n) * size_t(n) * 3);
    m.uvs.reserve(size_t(n) * size_t(n) * 2);
    for (int j = 0; j < n; ++j) {
        const float v = float(j) / float(subdiv);
        const float y = v * h;
        for (int i = 0; i < n; ++i) {
            const float u = float(i) / float(subdiv);
            const float x = u * w;
            m.positions.push_back(x);
            m.positions.push_back(y);
            m.positions.push_back(0.0f);
            m.uvs.push_back(u);
            m.uvs.push_back(v);
        }
    }
    m.indices.reserve(size_t(subdiv) * size_t(subdiv) * 6);
    for (int j = 0; j < subdiv; ++j)
        for (int i = 0; i < subdiv; ++i) {
            const uint16_t a = uint16_t(j * n + i);
            const uint16_t b = uint16_t(j * n + i + 1);
            const uint16_t c = uint16_t((j + 1) * n + i + 1);
            const uint16_t d = uint16_t((j + 1) * n + i);
            m.indices.push_back(a);
            m.indices.push_back(b);
            m.indices.push_back(c);
            m.indices.push_back(a);
            m.indices.push_back(c);
            m.indices.push_back(d);
        }

    formats::M3Region r;
    r.first_vertex = 0;
    r.num_vertices = m.vertex_count;
    r.first_index = 0;
    r.num_indices = uint32_t(m.indices.size());
    m.regions.push_back(r);
    m.region_material.push_back(-1);
    return m;
}

double series_cos(double a) {
    const double a2 = a * a;
    double term = 1.0, sum = 1.0;
    for (int k = 1; k <= 8; ++k) {
        term *= -a2 / double((2 * k - 1) * (2 * k));
        sum += term;
    }
    return sum;
}
double series_sin(double a) {
    const double a2 = a * a;
    double term = a, sum = a;
    for (int k = 1; k <= 8; ++k) {
        term *= -a2 / double((2 * k) * (2 * k + 1));
        sum += term;
    }
    return sum;
}

}

SynthMap SynthMap::generate(float width, float height, int subdiv) {
    SynthMap map;
    map.width = width;
    map.height = height;

    const float span = std::min(width, height);
    const float margin = span * 0.15f;
    const float radius = span * 0.07f;

    StartLocation s0;
    s0.position = {margin, margin, 0.0f};
    const double kInterior = kPi * 0.25;
    const double kArc = kPi * 0.5;
    s0.minerals.reserve(size_t(kMineralsPerBase));
    for (int k = 0; k < kMineralsPerBase; ++k) {
        const double t =
            (kMineralsPerBase == 1) ? 0.5 : double(k) / double(kMineralsPerBase - 1);
        const double ang = kInterior + (t - 0.5) * kArc;
        const float mx = s0.position.x + float(double(radius) * series_cos(ang));
        const float my = s0.position.y + float(double(radius) * series_sin(ang));
        s0.minerals.push_back({mx, my, 0.0f});
    }

    StartLocation s1;
    s1.position = mirror(s0.position, width, height);
    s1.minerals.reserve(s0.minerals.size());
    for (const render::Vec3& m : s0.minerals)
        s1.minerals.push_back(mirror(m, width, height));

    map.starts.push_back(s0);
    map.starts.push_back(s1);

    map.ground = ground_grid(width, height, subdiv);

    int gw = int(width);
    int gh = int(height);
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    map.nav.configure(gw, gh, sim::Fixed::from_int(1), sim::Fixed(), sim::Fixed());

    return map;
}

RealMap RealMap::load(const char* path) {
    RealMap out;
    if (!path || !*path) return out;
    std::ifstream in(path, std::ios::binary);
    if (!in) return out;
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                    std::istreambuf_iterator<char>());
    if (bytes.size() < 1000) return out;
    try {
        out.map = formats::load_map_terrain(std::move(bytes));
    } catch (const std::exception&) {
        return out;
    }
    if (out.map.width == 0 || out.map.height == 0) return out;
    try {
        out.ground = formats::build_map_mesh_stepped(out.map, 1.0f, 128, true);
    } catch (const std::exception&) {
        return out;
    }
    out.ok = true;
    return out;
}

}
