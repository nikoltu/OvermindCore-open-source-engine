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
    for (size_t i = 0; i < cells; ++i) {
        const uint32_t lvl = uint32_t(r.u16le()) >> 6;
        cm.levels[i] = uint8_t(lvl > 255 ? 255 : lvl);
    }
    return cm;
}

}
