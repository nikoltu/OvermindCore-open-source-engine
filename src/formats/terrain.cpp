#include "sc2/formats/terrain.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <stdexcept>

#include "sc2/core/byte_reader.hpp"
#include "sc2/core/xml.hpp"

namespace sc2::formats {

using core::ByteReader;

HeightMap HeightMap::parse(const std::vector<uint8_t>& data) {
    if (data.size() < 32 || data[0] != 'H' || data[1] != 'M' || data[2] != 'A' ||
        data[3] != 'P')
        throw std::runtime_error("terrain: not an HMAP heightmap");

    ByteReader r(data);
    r.seek(4);
    HeightMap hm;
    hm.version = r.u32le();
    hm.width = r.u32le();
    hm.height = r.u32le();
    hm.data_offset = 32;

    size_t verts = hm.vertex_count();
    if (verts == 0 || data.size() < hm.data_offset)
        throw std::runtime_error("terrain: bad dimensions");
    size_t region = data.size() - hm.data_offset;
    if (region % verts != 0)
        throw std::runtime_error("terrain: record region not a multiple of vertices");
    hm.stride = uint32_t(region / verts);
    return hm;
}

TerrainQuantize parse_terrain_quantize(const std::vector<uint8_t>& t3terrain_xml) {
    TerrainQuantize q;
    core::XmlNode root =
        core::xml_parse(std::string(t3terrain_xml.begin(), t3terrain_xml.end()));
    std::vector<const core::XmlNode*> stack{&root};
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (n->name == "vertData" && n->attr_or("name", "") == "t3HeightMap") {
            const std::string* s = n->attr("quantizeScale");
            const std::string* b = n->attr("quantizeBias");
            const std::string* h = n->attr("standardHeight");
            if (s) q.quantize_scale = float(std::atof(s->c_str()));
            if (b) q.quantize_bias = float(std::atof(b->c_str()));
            if (h) q.standard_height = float(std::atof(h->c_str()));
            q.found = true;
            return q;
        }
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    return q;
}

TerrainUvTiling parse_terrain_uvtiling(const std::vector<uint8_t>& t3terrain_xml) {
    TerrainUvTiling out;
    core::XmlNode root =
        core::xml_parse(std::string(t3terrain_xml.begin(), t3terrain_xml.end()));
    std::vector<const core::XmlNode*> stack{&root};
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (n->name == "heightMap") {
            if (const std::string* s = n->attr("uvtiling")) {
                float u = 0, v = 0;
                if (std::sscanf(s->c_str(), "%f %f", &u, &v) == 2 && u > 0.0f && v > 0.0f) {
                    out.u = u;
                    out.v = v;
                    out.found = true;
                }
            }
            break;
        }
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    return out;
}

std::string parse_terrain_tileset(const std::vector<uint8_t>& t3terrain_xml) {
    core::XmlNode root =
        core::xml_parse(std::string(t3terrain_xml.begin(), t3terrain_xml.end()));
    std::vector<const core::XmlNode*> stack{&root};
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (const std::string* t = n->attr("tileSet"); t && !t->empty() && *t != "0")
            return *t;
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    return {};
}

std::string parse_terrain_cliffset(const std::vector<uint8_t>& t3terrain_xml) {
    core::XmlNode root =
        core::xml_parse(std::string(t3terrain_xml.begin(), t3terrain_xml.end()));
    std::vector<const core::XmlNode*> stack{&root};
    std::string best;
    int best_i = 1 << 30;
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (n->name == "cliffSet") {
            const std::string* nm = n->attr("name");
            int idx = 0;
            if (const std::string* i = n->attr("i")) idx = std::atoi(i->c_str());
            if (nm && !nm->empty() && idx < best_i) { best_i = idx; best = *nm; }
        }
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    return best;
}

std::vector<std::string> parse_terrain_cliffsets(const std::vector<uint8_t>& t3terrain_xml) {
    core::XmlNode root =
        core::xml_parse(std::string(t3terrain_xml.begin(), t3terrain_xml.end()));
    std::vector<std::pair<int, std::string>> found;
    std::vector<const core::XmlNode*> stack{&root};
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (n->name == "cliffSet") {
            const std::string* nm = n->attr("name");
            int idx = 0;
            if (const std::string* i = n->attr("i")) idx = std::atoi(i->c_str());
            if (nm && !nm->empty()) found.emplace_back(idx, *nm);
        }
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> out;
    for (const auto& p : found) {
        if (int(out.size()) <= p.first) out.resize(p.first + 1);
        out[p.first] = p.second;
    }
    return out;
}

namespace {
RampFrame parse_ramp_frame(const std::string* v) {
    RampFrame f;
    if (!v) return f;
    std::sscanf(v->c_str(), "u(%g, %g) r(%g, %g) c=(%g, %g) w=%g h=%g", &f.ux, &f.uy, &f.rx,
                &f.ry, &f.cx, &f.cy, &f.w, &f.h);
    return f;
}
uint32_t ramp_u32(const core::XmlNode& n, const char* key, uint32_t dflt) {
    if (const std::string* a = n.attr(key)) return uint32_t(std::strtoul(a->c_str(), nullptr, 10));
    return dflt;
}
}

std::vector<TerrainRamp> parse_terrain_ramps(const std::vector<uint8_t>& t3terrain_xml) {
    std::vector<TerrainRamp> out;
    core::XmlNode root =
        core::xml_parse(std::string(t3terrain_xml.begin(), t3terrain_xml.end()));
    const core::XmlNode* list = nullptr;
    std::vector<const core::XmlNode*> stack{&root};
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (n->name == "rampList") { list = n; break; }
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    if (!list) return out;
    for (const core::XmlNode& c : list->children) {
        if (c.name != "ramp") continue;
        TerrainRamp r;
        r.dir = ramp_u32(c, "dir", 0);
        r.hi = ramp_u32(c, "hi", 0);
        r.lo = ramp_u32(c, "lo", 0);
        r.cid = ramp_u32(c, "cid", 0);
        r.left_lo = parse_ramp_frame(c.attr("leftLo"));
        r.left_hi = parse_ramp_frame(c.attr("leftHi"));
        r.right_lo = parse_ramp_frame(c.attr("rightLo"));
        r.right_hi = parse_ramp_frame(c.attr("rightHi"));
        r.base = parse_ramp_frame(c.attr("base"));
        r.mid = parse_ramp_frame(c.attr("mid"));
        r.left_lo_var = ramp_u32(c, "leftLoVar", 0xFFFFFFFFu);
        r.left_hi_var = ramp_u32(c, "leftHiVar", 0xFFFFFFFFu);
        r.right_lo_var = ramp_u32(c, "rightLoVar", 0xFFFFFFFFu);
        r.right_hi_var = ramp_u32(c, "rightHiVar", 0xFFFFFFFFu);
        out.push_back(r);
    }
    return out;
}

std::vector<CliffCell> parse_terrain_cliffcells(const std::vector<uint8_t>& t3terrain_xml,
                                                uint32_t& grid_w, uint32_t& grid_h) {
    grid_w = grid_h = 0;
    core::XmlNode root =
        core::xml_parse(std::string(t3terrain_xml.begin(), t3terrain_xml.end()));
    std::vector<CliffCell> out;
    const core::XmlNode* list = nullptr;
    std::vector<const core::XmlNode*> stack{&root};
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (n->name == "cliffCellList") { list = n; break; }
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    if (!list) return out;
    uint32_t num = 0;
    if (const std::string* s = list->attr("num")) num = uint32_t(std::atoi(s->c_str()));
    std::vector<const core::XmlNode*> st2{&root};
    while (!st2.empty()) {
        const core::XmlNode* n = st2.back();
        st2.pop_back();
        if (n->name == "heightMap") {
            if (const std::string* d = n->attr("dim")) {
                int W = 0, H = 0;
                if (std::sscanf(d->c_str(), "%d %d", &W, &H) == 2 && W > 1 && H > 1) {
                    grid_w = uint32_t(W) / 2;
                    grid_h = uint32_t(H) / 2;
                }
            }
            break;
        }
        for (const core::XmlNode& c : n->children) st2.push_back(&c);
    }
    if (grid_w && num && grid_w * grid_h != num) {
        if (num % grid_w == 0) grid_h = num / grid_w;
    }
    for (const core::XmlNode& c : list->children) {
        if (c.name != "cc") continue;
        CliffCell cell;
        if (const std::string* s = c.attr("i")) cell.index = uint32_t(std::atoi(s->c_str()));
        if (const std::string* s = c.attr("cid")) cell.cid = uint8_t(std::atoi(s->c_str()));
        if (const std::string* s = c.attr("cvar")) cell.cvar = uint8_t(std::atoi(s->c_str()));
        if (const std::string* s = c.attr("f")) cell.flag = uint8_t(std::atoi(s->c_str()));
        out.push_back(cell);
    }
    return out;
}

std::vector<std::string> parse_terrain_textures(const std::vector<uint8_t>& t3terrain_xml) {
    core::XmlNode root =
        core::xml_parse(std::string(t3terrain_xml.begin(), t3terrain_xml.end()));
    std::vector<std::pair<int, std::string>> slots;
    std::vector<const core::XmlNode*> stack{&root};
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (n->name == "texture") {
            const std::string* nm = n->attr("name");
            if (nm && !nm->empty()) {
                int idx = 0;
                if (const std::string* i = n->attr("i")) idx = std::atoi(i->c_str());
                slots.emplace_back(idx, *nm);
            }
        }
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    std::sort(slots.begin(), slots.end(),
              [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<std::string> out;
    out.reserve(slots.size());
    for (auto& s : slots) out.push_back(std::move(s.second));
    return out;
}

Model build_terrain_mesh(const HeightMap& hm, const std::vector<uint8_t>& data,
                         float cell, float quantize_scale, float quantize_bias,
                         uint32_t max_dim) {
    if (hm.width < 2 || hm.height < 2 || hm.stride < 2)
        throw std::runtime_error("terrain mesh: degenerate heightmap");

    uint32_t nx = std::min(hm.width, std::max<uint32_t>(2, max_dim));
    uint32_t ny = std::min(hm.height, std::max<uint32_t>(2, max_dim));

    auto src = [](uint32_t i, uint32_t n, uint32_t dim) -> uint32_t {
        return uint32_t(uint64_t(i) * (dim - 1) / (n - 1));
    };
    auto height_at = [&](uint32_t col, uint32_t row) -> float {
        size_t rec = hm.data_offset + (size_t(row) * hm.width + col) * hm.stride;
        if (rec + 1 >= data.size()) return 0.0f;
        uint16_t h = uint16_t(data[rec]) | uint16_t(data[rec + 1]) << 8;
        return float(h) * quantize_scale - quantize_bias;
    };

    Model m;
    m.vertex_count = nx * ny;
    m.positions.reserve(size_t(m.vertex_count) * 3);
    m.uvs.reserve(size_t(m.vertex_count) * 2);
    for (uint32_t j = 0; j < ny; ++j)
        for (uint32_t i = 0; i < nx; ++i) {
            float z = height_at(src(i, nx, hm.width), src(j, ny, hm.height));
            m.positions.push_back(float(i) * cell);
            m.positions.push_back(float(j) * cell);
            m.positions.push_back(z);
            m.uvs.push_back(float(i) / float(nx - 1));
            m.uvs.push_back(float(j) / float(ny - 1));
        }

    m.indices.reserve(size_t(nx - 1) * (ny - 1) * 6);
    for (uint32_t j = 0; j + 1 < ny; ++j)
        for (uint32_t i = 0; i + 1 < nx; ++i) {
            uint16_t a = uint16_t(j * nx + i);
            uint16_t b = uint16_t(a + 1);
            uint16_t c = uint16_t(a + nx);
            uint16_t d = uint16_t(c + 1);
            m.indices.insert(m.indices.end(), {a, c, b, b, c, d});
        }

    M3Region r;
    r.first_vertex = 0;
    r.num_vertices = m.vertex_count;
    r.first_index = 0;
    r.num_indices = uint32_t(m.indices.size());
    m.regions.push_back(r);
    m.region_material.push_back(-1);
    return m;
}

void graded_cell_corners(const CliffMap& cliff, const std::vector<float>& level_height,
                         const std::vector<uint8_t>* ramp, uint32_t i, uint32_t j, float out[4],
                         const std::vector<float>* relief) {
    const uint32_t cw = cliff.width, nx = cliff.width, ny = cliff.height;
    const bool have_raw = cliff.raw.size() == size_t(cliff.width) * cliff.height;
    auto height_of = [&](uint32_t a, uint32_t b) -> float {
        const size_t k = size_t(b) * cw + a;
        const uint8_t L = cliff.levels[k];
        const float base = level_height[L < level_height.size() ? L : level_height.size() - 1];
        if (!have_raw) return base;
        const uint8_t rem = uint8_t(cliff.raw[k] & 63);
        if (rem == 0) return base;
        const size_t up = size_t(L) + 1;
        if (up >= level_height.size()) return base;
        return base + (level_height[up] - base) * (float(rem) / 64.0f);
    };
    const bool have_ramps = ramp && !ramp->empty();
    auto ramp_of = [&](uint32_t a, uint32_t b) -> bool {
        if (!have_ramps) return false;
        const size_t k = size_t(b) * cw + a;
        return k < ramp->size() && (*ramp)[k] != 0;
    };
    const float z = height_of(i, j);
    out[0] = out[1] = out[2] = out[3] = z;
    if (!ramp_of(i, j)) {
        if (relief && relief->size() == size_t(cw) * ny) {
            const uint8_t L = cliff.levels[size_t(j) * cw + i];
            auto rel = [&](int a, int b, float& sum, int& n) {
                if (a < 0 || b < 0 || a >= int(cw) || b >= int(ny)) return;
                const size_t k = size_t(b) * cw + a;
                if (cliff.levels[k] != L || ramp_of(uint32_t(a), uint32_t(b))) return;
                sum += (*relief)[k];
                ++n;
            };
            const int cx[4] = {-1, -1, 0, 0}, cy[4] = {-1, 0, -1, 0};
            const int ox[4] = {0, 0, 1, 1}, oy[4] = {0, 1, 0, 1};
            for (int c = 0; c < 4; ++c) {
                float sum = 0.0f;
                int n = 0;
                for (int q = 0; q < 4; ++q) rel(int(i) + ox[c] + cx[q], int(j) + oy[c] + cy[q], sum, n);
                out[c] = z + (n ? sum / float(n) : 0.0f);
            }
        }
        return;
    }
    auto edge = [&](bool inb, uint32_t ni, uint32_t nj) -> float {
        if (!inb) return z;
        const float zn = height_of(ni, nj);
        return ramp_of(ni, nj) ? 0.5f * (z + zn) : zn;
    };
    const float eL = edge(i > 0, i - 1, j);
    const float eR = edge(i + 1 < nx, i + 1, j);
    const float eD = edge(j > 0, i, j - 1);
    const float eU = edge(j + 1 < ny, i, j + 1);
    const float zL = i > 0 ? height_of(i - 1, j) : z;
    const float zR = i + 1 < nx ? height_of(i + 1, j) : z;
    const float zD = j > 0 ? height_of(i, j - 1) : z;
    const float zU = j + 1 < ny ? height_of(i, j + 1) : z;
    const float lo = std::min({z, zL, zR, zD, zU});
    const float hi = std::max({z, zL, zR, zD, zU});
    auto corner = [&](float ex, float ey) { return std::min(hi, std::max(lo, ex + ey - z)); };
    out[0] = corner(eL, eD);
    out[1] = corner(eL, eU);
    out[2] = corner(eR, eD);
    out[3] = corner(eR, eU);
}

float graded_height_at(const CliffMap& cliff, const std::vector<float>& level_height,
                       const std::vector<uint8_t>* ramp, float cx, float cy,
                       const std::vector<float>* relief) {
    if (cliff.empty() || level_height.empty()) return 0.0f;
    if (cx < 0.0f) cx = 0.0f;
    if (cy < 0.0f) cy = 0.0f;
    uint32_t i = uint32_t(cx), j = uint32_t(cy);
    if (i >= cliff.width) i = cliff.width - 1;
    if (j >= cliff.height) j = cliff.height - 1;
    float c[4];
    graded_cell_corners(cliff, level_height, ramp, i, j, c, relief);
    const float fx = cx - float(i), fy = cy - float(j);
    const float a = c[0] + (c[2] - c[0]) * fx;
    const float b = c[1] + (c[3] - c[1]) * fx;
    return a + (b - a) * fy;
}

Model build_terrain_mesh_stepped(const CliffMap& cliff,
                                 const std::vector<float>& level_height, float cell,
                                 const std::vector<uint8_t>* ramp, bool with_walls,
                                 const std::vector<float>* relief) {
    if (cliff.empty() || cliff.width < 2 || cliff.height < 2 || level_height.empty())
        throw std::runtime_error("stepped terrain mesh: degenerate cliff grid");

    const uint32_t cw = cliff.width, ch = cliff.height;
    const uint32_t nx = cw, ny = ch;

    auto height_of = [&](uint32_t i, uint32_t j) -> float {
        uint8_t L = cliff.levels[size_t(j) * cw + i];
        return level_height[L < level_height.size() ? L : level_height.size() - 1];
    };
    const bool have_ramps = ramp && !ramp->empty();
    auto ramp_of = [&](uint32_t i, uint32_t j) -> bool {
        if (!have_ramps) return false;
        size_t k = size_t(j) * cw + i;
        return k < ramp->size() && (*ramp)[k] != 0;
    };
    auto corners_of = [&](uint32_t i, uint32_t j, float out[4]) {
        graded_cell_corners(cliff, level_height, ramp, i, j, out, relief);
    };

    Model m;
    const float ex = float(nx) * cell, ey = float(ny) * cell;
    auto push_vert = [&](float x, float y, float z) {
        m.positions.push_back(x);
        m.positions.push_back(y);
        m.positions.push_back(z);
        m.uvs.push_back(ex > 0.0f ? x / ex : 0.0f);
        m.uvs.push_back(ey > 0.0f ? y / ey : 0.0f);
    };
    const size_t kRegionVerts = 65536;
    uint32_t region_base = 0, region_first_index = 0;
    auto flush_region = [&]() {
        const uint32_t vend = uint32_t(m.positions.size() / 3);
        const uint32_t iend = uint32_t(m.indices.size());
        if (iend == region_first_index) return;
        M3Region r;
        r.first_vertex = region_base;
        r.num_vertices = vend - region_base;
        r.first_index = region_first_index;
        r.num_indices = iend - region_first_index;
        m.regions.push_back(r);
        m.region_material.push_back(-1);
        region_base = vend;
        region_first_index = iend;
    };

    if (size_t(nx) * 4 > kRegionVerts)
        throw std::runtime_error("stepped terrain mesh: cliff grid too wide to index");
    const uint32_t rows_per_band =
        std::max<uint32_t>(1, uint32_t(kRegionVerts / (size_t(nx) * 4 + 4)));
    for (uint32_t j = 0; j < ny; ++j) {
        if (j && j % rows_per_band == 0) flush_region();
        for (uint32_t i = 0; i < nx; ++i) {
            const float x0 = float(i) * cell, x1 = x0 + cell;
            const float y0 = float(j) * cell, y1 = y0 + cell;
            float c[4];
            corners_of(i, j, c);
            const uint16_t b = uint16_t(m.positions.size() / 3 - region_base);
            push_vert(x0, y0, c[0]);
            push_vert(x0, y1, c[1]);
            push_vert(x1, y0, c[2]);
            push_vert(x1, y1, c[3]);
            m.indices.insert(m.indices.end(),
                             {b, uint16_t(b + 1), uint16_t(b + 2), uint16_t(b + 2),
                              uint16_t(b + 1), uint16_t(b + 3)});
        }
    }
    flush_region();

    auto add_wall = [&](float ax, float ay, float bx, float by, float zlo, float zhi) {
        if (m.positions.size() / 3 + 4 - region_base > kRegionVerts) flush_region();
        const uint16_t b = uint16_t(m.positions.size() / 3 - region_base);
        push_vert(ax, ay, zlo);
        push_vert(bx, by, zlo);
        push_vert(ax, ay, zhi);
        push_vert(bx, by, zhi);
        m.indices.insert(m.indices.end(),
                         {b, uint16_t(b + 1), uint16_t(b + 2), uint16_t(b + 2),
                          uint16_t(b + 1), uint16_t(b + 3)});
    };
    for (uint32_t j = 0; with_walls && j < ny; ++j)
        for (uint32_t i = 0; i < nx; ++i) {
            const float z = height_of(i, j);
            const bool r = ramp_of(i, j);
            if (i + 1 < nx) {
                const float zr = height_of(i + 1, j);
                if (z != zr && !r && !ramp_of(i + 1, j)) {
                    const float x = float(i + 1) * cell, y0 = float(j) * cell, y1 = y0 + cell;
                    add_wall(x, y0, x, y1, std::min(z, zr), std::max(z, zr));
                }
            }
            if (j + 1 < ny) {
                const float zd = height_of(i, j + 1);
                if (z != zd && !r && !ramp_of(i, j + 1)) {
                    const float y = float(j + 1) * cell, x0 = float(i) * cell, x1 = x0 + cell;
                    add_wall(x0, y, x1, y, std::min(z, zd), std::max(z, zd));
                }
            }
        }
    flush_region();

    m.vertex_count = uint32_t(m.positions.size() / 3);
    return m;
}

TextureInfo TextureInfo::parse(const std::vector<uint8_t>& d) {
    if (d.size() < 20 || d[0] != 'R' || d[1] != 'T' || d[2] != 'X' || d[3] != 'T')
        throw std::runtime_error("terrain: not an RTXT t3SyncTextureInfo");

    ByteReader r(d);
    r.seek(4);
    TextureInfo ti;
    ti.version = r.u32le();
    ti.width = r.u32le();
    ti.height = r.u32le();
    ti.field3 = r.u32le();

    if (ti.width == 0 || ti.height == 0 || ti.width > 65536 || ti.height > 65536)
        throw std::runtime_error("terrain: RTXT dims out of range");

    const size_t blob = size_t(ti.width) * ti.height * 8;
    if (d.size() < 20 + blob)
        throw std::runtime_error("terrain: RTXT slot blob does not fit");
    const size_t blob_start = d.size() - blob;

    size_t off = 20;
    while (off < blob_start) {
        if (d[off] == 0) {
            ++off;
            continue;
        }
        size_t end = off;
        while (end < d.size() && d[end] != 0) ++end;
        if (end >= d.size() || end > blob_start) break;
        ti.layer_names.emplace_back(reinterpret_cast<const char*>(&d[off]), end - off);
        off = end + 1;
    }
    ti.slots.assign(d.begin() + blob_start, d.begin() + blob_start + blob);
    return ti;
}

TextureMask TextureMask::parse(const std::vector<uint8_t>& d) {
    if (d.size() < 20 || d[0] != 'M' || d[1] != 'A' || d[2] != 'S' || d[3] != 'K')
        throw std::runtime_error("terrain: not a MASK t3TextureMasks");

    ByteReader r(d);
    r.seek(4);
    TextureMask tm;
    tm.version = r.u32le();
    r.u32le();
    tm.mask_w = r.u32le();
    tm.mask_h = r.u32le();

    if (tm.mask_w == 0 || tm.mask_h == 0 || tm.mask_w > 524288 || tm.mask_h > 524288)
        throw std::runtime_error("terrain: MASK dims out of range");

    const size_t field = size_t(tm.mask_w) * tm.mask_h * 4;
    if (d.size() < field)
        throw std::runtime_error("terrain: MASK texel field does not fit");
    const size_t header = d.size() - field;
    if (header < 20)
        throw std::runtime_error("terrain: MASK field overruns its header");
    tm.texels.assign(d.begin() + header, d.end());
    return tm;
}

uint8_t TextureMask::layer_weight(uint32_t layer, uint32_t x, uint32_t y) const {
    if (layer >= 8 || x >= mask_w || !planar_ok()) return 0;
    const uint32_t qw = quad_w(), qh = quad_h();
    if (y >= qh * 64) return 0;
    const uint32_t bx = x >> 1;
    const uint32_t qx = bx >> 6, qy = y >> 6;
    const uint32_t tX = (bx >> 5) & 1, tY = (y >> 5) & 1;
    const uint32_t px = bx & 31, py = y & 31;
    const size_t tile = ((size_t(layer) * qh + qy) * qw + qx) * 4 + tX * 2 + tY;
    const size_t off = tile * 1024 + py * 32 + px;
    if (off >= texels.size()) return 0;
    const uint8_t b = texels[off];
    return uint8_t(((x & 1) ? (b & 0x0F) : (b >> 4)) * 17);
}

PaintedPathing PaintedPathing::parse(const std::vector<uint8_t>& d) {
    PaintedPathing out;
    if (d.size() != kHeader + size_t(kRows) * kStride) return out;
    out.cells.assign(d.begin() + kHeader, d.end());
    return out;
}

CellFlags CellFlags::parse(const std::vector<uint8_t>& d) {
    CellFlags out;
    if (d.size() < 0x20 ||
        !((d[0] == 'T' && d[1] == 'C' && d[2] == 'F' && d[3] == 'L') ||
          (d[0] == 'L' && d[1] == 'F' && d[2] == 'C' && d[3] == 'T'))) {
        return out;
    }
    const auto u32 = [&d](size_t off) {
        return uint32_t(d[off]) | (uint32_t(d[off + 1]) << 8) | (uint32_t(d[off + 2]) << 16) |
               (uint32_t(d[off + 3]) << 24);
    };
    const uint32_t w = u32(0x18), h = u32(0x1C);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return out;
    const size_t need = size_t(w) * h;
    if (d.size() < 0x20 + need) return out;
    out.width = w;
    out.height = h;
    out.cells.assign(d.begin() + 0x20, d.begin() + 0x20 + std::ptrdiff_t(need));
    return out;
}

SyncHeightMap SyncHeightMap::parse(const std::vector<uint8_t>& d) {
    if (d.size() < 16 || d[0] != 'S' || d[1] != 'M' || d[2] != 'A' || d[3] != 'P')
        throw std::runtime_error("terrain: not an SMAP t3SyncHeightMap");
    ByteReader r(d);
    r.seek(4);
    SyncHeightMap sh;
    sh.version = r.u32le();
    sh.width = r.u32le();
    sh.height = r.u32le();
    if (sh.width == 0 || sh.height == 0 || sh.width > 65535 || sh.height > 65535)
        throw std::runtime_error("terrain: SMAP dims out of range");
    const size_t field = size_t(sh.width) * sh.height * 4;
    if (d.size() < field) throw std::runtime_error("terrain: SMAP record field does not fit");
    const size_t header = d.size() - field;
    if (header < 16) throw std::runtime_error("terrain: SMAP field overruns its header");
    const size_t n = size_t(sh.width) * sh.height;
    sh.height_i16.resize(n);
    sh.level.resize(n);
    for (size_t i = 0; i < n; ++i) {
        const size_t o = header + i * 4;
        sh.height_i16[i] = int16_t(uint16_t(uint16_t(d[o]) | uint16_t(d[o + 1]) << 8));
        sh.level[i] = uint16_t(uint16_t(d[o + 2]) | uint16_t(d[o + 3]) << 8);
    }
    return sh;
}

WaterMap WaterMap::parse(const std::vector<uint8_t>& d) {
    if (d.size() < 12 || d[0] != 'W' || d[1] != 'A' || d[2] != 'T' || d[3] != 'R')
        throw std::runtime_error("terrain: not a WATR t3Water");

    ByteReader r(d);
    r.seek(4);
    WaterMap wm;
    wm.version = r.u32le();
    const uint32_t count = r.u32le();
    if (count > 65536)
        throw std::runtime_error("terrain: WATR plane count out of range");

    const size_t hdr = 32, rec = 56;
    auto f32 = [&d](size_t at) -> float {
        uint32_t u = uint32_t(d[at]) | uint32_t(d[at + 1]) << 8 | uint32_t(d[at + 2]) << 16 |
                     uint32_t(d[at + 3]) << 24;
        float v;
        std::memcpy(&v, &u, 4);
        return v;
    };
    for (uint32_t i = 0; i < count; ++i) {
        const size_t base = hdr + size_t(i) * rec;
        if (base + rec > d.size()) break;
        size_t ne = 0;
        while (ne < 16 && d[base + ne] != 0) ++ne;
        WaterPlane p;
        p.type.assign(reinterpret_cast<const char*>(&d[base]), ne);
        for (int k = 0; k < 6; ++k) p.params[size_t(k)] = f32(base + 16 + size_t(k) * 4);
        p.min_x = f32(base + 16 + 6 * 4);
        p.min_y = f32(base + 16 + 7 * 4);
        p.max_x = f32(base + 16 + 8 * 4);
        p.max_y = f32(base + 16 + 9 * 4);
        wm.planes.push_back(std::move(p));
    }
    return wm;
}

Model build_water_mesh(const WaterMap& water, const std::vector<float>& level, float cell) {
    Model m;
    if (water.planes.empty()) return m;

    bool have_bounds = false;
    float lo[3] = {0, 0, 0}, hi[3] = {0, 0, 0};
    auto grow = [&](float x, float y, float z) {
        if (!have_bounds) {
            lo[0] = hi[0] = x; lo[1] = hi[1] = y; lo[2] = hi[2] = z;
            have_bounds = true;
            return;
        }
        lo[0] = std::min(lo[0], x); hi[0] = std::max(hi[0], x);
        lo[1] = std::min(lo[1], y); hi[1] = std::max(hi[1], y);
        lo[2] = std::min(lo[2], z); hi[2] = std::max(hi[2], z);
    };

    for (size_t i = 0; i < water.planes.size(); ++i) {
        const WaterPlane& p = water.planes[i];
        const float z = i < level.size() ? level[i] : 0.0f;
        const float x0 = std::min(p.min_x, p.max_x) * cell;
        const float x1 = std::max(p.min_x, p.max_x) * cell;
        const float y0 = std::min(p.min_y, p.max_y) * cell;
        const float y1 = std::max(p.min_y, p.max_y) * cell;
        if (!(x1 > x0) || !(y1 > y0)) continue;

        const uint16_t b = 0;
        const uint32_t fv = uint32_t(m.positions.size() / 3);
        const uint32_t fi = uint32_t(m.indices.size());
        auto pv = [&](float x, float y) {
            m.positions.push_back(x);
            m.positions.push_back(y);
            m.positions.push_back(z);
            m.uvs.push_back(x);
            m.uvs.push_back(y);
            grow(x, y, z);
        };
        pv(x0, y0);
        pv(x0, y1);
        pv(x1, y0);
        pv(x1, y1);
        m.indices.insert(m.indices.end(), {b, uint16_t(b + 1), uint16_t(b + 2), uint16_t(b + 2),
                                           uint16_t(b + 1), uint16_t(b + 3)});
        M3Region r;
        r.first_vertex = fv;
        r.num_vertices = 4;
        r.first_index = fi;
        r.num_indices = 6;
        m.regions.push_back(r);
        m.region_material.push_back(-1);
    }

    m.vertex_count = uint32_t(m.positions.size() / 3);
    if (have_bounds) {
        for (int k = 0; k < 3; ++k) {
            m.bounds_min[k] = lo[k];
            m.bounds_max[k] = hi[k];
        }
    }
    return m;
}

CliffMap CliffMap::parse(const std::vector<uint8_t>& d) {
    if (d.size() < 32 || d[0] != 'C' || d[1] != 'L' || d[2] != 'I' || d[3] != 'F')
        throw std::runtime_error("terrain: not a CLIF t3SyncCliffLevel");
    ByteReader r(d);
    r.seek(4);
    CliffMap cm;
    cm.version = r.u32le();
    cm.width = r.u32le();
    cm.height = r.u32le();
    if (cm.width == 0 || cm.height == 0 || cm.width > 65536 || cm.height > 65536)
        throw std::runtime_error("terrain: CLIF dims out of range");
    const size_t cells = size_t(cm.width) * cm.height;
    if (d.size() < 32 + cells * 2)
        throw std::runtime_error("terrain: CLIF cell region too short");
    r.seek(32);
    cm.levels.resize(cells);
    cm.raw.resize(cells);
    for (size_t i = 0; i < cells; ++i) {
        const uint16_t raw = r.u16le();
        cm.raw[i] = raw;
        const uint32_t lvl = uint32_t(raw) >> 6;
        cm.levels[i] = uint8_t(lvl > 255 ? 255 : lvl);
    }
    return cm;
}

}
