#include "sc2/app/test_map.hpp"

#include <algorithm>

namespace sc2::app {

namespace {

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

TestMap::Base make_base(const render::Vec3& town, const render::Vec3& interior,
                        float span) {
    TestMap::Base base;
    base.town = town;

    const render::Vec3 d = render::normalize(render::sub(interior, town));
    const render::Vec3 e = {-d.x, -d.y, 0.0f};
    const render::Vec3 p = {-d.y, d.x, 0.0f};

    const float standoff = TestMap::kBaseMineralDistance;
    const float line_len = span * 0.11f;
    const float arc_depth = span * 0.022f;

    base.resources.reserve(size_t(TestMap::kMineralsPerBase) + 1);
    for (int k = 0; k < TestMap::kMineralsPerBase; ++k) {
        const float t = (TestMap::kMineralsPerBase == 1)
                            ? 0.5f
                            : float(k) / float(TestMap::kMineralsPerBase - 1);
        const float lateral = (t - 0.5f) * line_len;
        const float bow = arc_depth * (1.0f - 4.0f * (t - 0.5f) * (t - 0.5f));
        const float mx = town.x + e.x * (standoff + bow) + p.x * lateral;
        const float my = town.y + e.y * (standoff + bow) + p.y * lateral;
        base.resources.push_back({{mx, my, 0.0f}, TestMap::ResourceKind::Mineral});
    }

    const float gas_standoff = TestMap::kBaseGeyserDistance;
    const float gas_side = span * 0.09f;
    const float gx = town.x + e.x * gas_standoff + p.x * gas_side;
    const float gy = town.y + e.y * gas_standoff + p.y * gas_side;
    base.resources.push_back({{gx, gy, 0.0f}, TestMap::ResourceKind::Geyser});
    return base;
}

TestMap::Base mirror_base(const TestMap::Base& b, float w, float h) {
    TestMap::Base out;
    out.town = mirror(b.town, w, h);
    out.resources.reserve(b.resources.size());
    for (const TestMap::Resource& r : b.resources)
        out.resources.push_back({mirror(r.position, w, h), r.kind});
    return out;
}

void block_sym(sim::NavGrid& nav, int gw, int gh, int cx, int cy) {
    nav.block_cell(cx, cy);
    nav.block_cell(gw - 1 - cx, gh - 1 - cy);
}

void block_rock(sim::NavGrid& nav, int gw, int gh, int cx, int cy, int size) {
    for (int j = 0; j < size; ++j)
        for (int i = 0; i < size; ++i) block_sym(nav, gw, gh, cx + i, cy + j);
}

}

TestMap TestMap::generate(float width, float height, int subdiv) {
    TestMap map;
    map.width = width;
    map.height = height;

    const float span = std::min(width, height);
    const render::Vec3 centre = {width * 0.5f, height * 0.5f, 0.0f};
    map.contested = centre;

    const float inset = span * 0.13f;
    const render::Vec3 main_town = {inset, inset, 0.0f};
    const render::Vec3 nat_town = {main_town.x + (centre.x - main_town.x) * 0.33f,
                                   main_town.y + (centre.y - main_town.y) * 0.33f,
                                   0.0f};

    PlayerSites p0;
    p0.main = make_base(main_town, centre, span);
    p0.natural = make_base(nat_town, centre, span);

    PlayerSites p1;
    p1.main = mirror_base(p0.main, width, height);
    p1.natural = mirror_base(p0.natural, width, height);

    map.players.push_back(p0);
    map.players.push_back(p1);

    map.ground = ground_grid(width, height, subdiv);

    int gw = int(width);
    int gh = int(height);
    if (gw < 1) gw = 1;
    if (gh < 1) gh = 1;
    map.nav.configure(gw, gh, sim::Fixed::from_int(1), sim::Fixed(), sim::Fixed());

    if (gw >= 4 && gh >= 4) {
        const int wall_l = gw / 2 - 1;
        int gap_half = gh / 16;
        if (gap_half < 2) gap_half = 2;
        const int gap_lo = gh / 2 - gap_half;
        const int gap_hi = gh / 2 + gap_half;
        for (int y = 0; y < gh; ++y) {
            if (y >= gap_lo && y < gap_hi) continue;
            block_sym(map.nav, gw, gh, wall_l, y);
        }

        const int rock = std::max(2, gw / 48);
        block_rock(map.nav, gw, gh, int(gw * 0.30f), int(gh * 0.66f), rock);
        block_rock(map.nav, gw, gh, int(gw * 0.16f), int(gh * 0.54f), rock);
    }

    return map;
}

}
