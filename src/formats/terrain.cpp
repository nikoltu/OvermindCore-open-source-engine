#include "sc2/formats/terrain.hpp"

#include <algorithm>
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

}
