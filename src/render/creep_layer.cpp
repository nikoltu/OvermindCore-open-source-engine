#include "sc2/render/creep_layer.hpp"

#include <algorithm>
#include <cstddef>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/dds.hpp"

namespace sc2::render {

void set_model_diffuse(formats::Model& m, const std::string& diffuse_path) {
    m.materials.clear();
    m.material_refs.clear();
    if (diffuse_path.empty()) {
        m.region_material.assign(m.regions.size(), -1);
        return;
    }
    formats::M3Layer layer;
    layer.usage = "Diffuse";
    layer.image_path = diffuse_path;
    formats::M3Material mat;
    mat.name = "terrain";
    mat.layers.push_back(layer);
    m.materials.push_back(mat);
    formats::M3MaterialRef ref;
    ref.type = 1;
    ref.index = 0;
    m.material_refs.push_back(ref);
    m.region_material.assign(m.regions.size(), 0);
}

const std::vector<std::string>& creep_texture_candidates() {
    static const std::vector<std::string> kPaths = {
        "Assets/Textures/creep.dds",
        "Assets/Textures/creepedgenormal.dds",
        "Assets/Textures/creepheight.dds",
        "Assets/Textures/creepnoise.dds",
        "Assets/Textures/creepspecularmask.dds",
        "Assets/Textures/creeptexturedisplacement.dds",
    };
    return kPaths;
}

std::string resolve_creep_diffuse(const casc::Storage& store) {
    static const char* kRoots[] = {"mods/core.sc2mod/base.sc2assets/",
                                   "mods/liberty.sc2mod/base.sc2assets/",
                                   "mods/swarm.sc2mod/base.sc2assets/",
                                   "mods/void.sc2mod/base.sc2assets/", ""};
    const std::string& rel = creep_texture_candidates().front();
    for (const char* root : kRoots) {
        try {
            std::vector<uint8_t> bytes = store.read_by_name(std::string(root) + rel);
            formats::DdsInfo info;
            std::vector<uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
            if (!rgba.empty() && info.width && info.height) return rel;
        } catch (const std::exception&) {
        }
    }
    return {};
}

formats::Model build_creep_model(const sim::CreepGrid& grid,
                                 const std::function<float(float, float)>& ground_z,
                                 float z_offset, float uv_tiles_per_cell,
                                 const std::string& diffuse_path) {
    formats::Model m;
    const int w = grid.width(), h = grid.height();
    if (w <= 0 || h <= 0) return m;

    const float cell = float(grid.cell_size().to_double());
    const float ox = float(grid.origin_x().to_double());
    const float oy = float(grid.origin_y().to_double());
    if (cell <= 0.0f) return m;

    float zmin = 1e30f, zmax = -1e30f, xmin = 1e30f, xmax = -1e30f, ymin = 1e30f, ymax = -1e30f;
    std::size_t region_first = 0, region_first_index = 0;
    for (int cy = 0; cy < h; ++cy) {
        for (int cx = 0; cx < w; ++cx) {
            if (!grid.is_creep_cell(cx, cy)) continue;
            if (m.positions.size() / 3 - region_first + 4 > 65535) {
                formats::M3Region r;
                r.first_vertex = uint32_t(region_first);
                r.num_vertices = uint32_t(m.positions.size() / 3 - region_first);
                r.first_index = uint32_t(region_first_index);
                r.num_indices = uint32_t(m.indices.size() - region_first_index);
                m.regions.push_back(r);
                region_first = m.positions.size() / 3;
                region_first_index = m.indices.size();
            }
            const float x0 = ox + float(cx) * cell, x1 = x0 + cell;
            const float y0 = oy + float(cy) * cell, y1 = y0 + cell;
            const uint16_t base = uint16_t(m.positions.size() / 3 - region_first);
            const float xs[4] = {x0, x1, x1, x0};
            const float ys[4] = {y0, y0, y1, y1};
            const float us[4] = {0.0f, uv_tiles_per_cell, uv_tiles_per_cell, 0.0f};
            const float vs[4] = {0.0f, 0.0f, uv_tiles_per_cell, uv_tiles_per_cell};
            for (int k = 0; k < 4; ++k) {
                const float z = (ground_z ? ground_z(xs[k], ys[k]) : 0.0f) + z_offset;
                m.positions.push_back(xs[k]);
                m.positions.push_back(ys[k]);
                m.positions.push_back(z);
                m.uvs.push_back(us[k]);
                m.uvs.push_back(vs[k]);
                zmin = std::min(zmin, z);
                zmax = std::max(zmax, z);
                xmin = std::min(xmin, xs[k]);
                xmax = std::max(xmax, xs[k]);
                ymin = std::min(ymin, ys[k]);
                ymax = std::max(ymax, ys[k]);
            }
            m.indices.push_back(uint16_t(base + 0));
            m.indices.push_back(uint16_t(base + 1));
            m.indices.push_back(uint16_t(base + 2));
            m.indices.push_back(uint16_t(base + 0));
            m.indices.push_back(uint16_t(base + 2));
            m.indices.push_back(uint16_t(base + 3));
        }
    }
    m.vertex_count = uint32_t(m.positions.size() / 3);
    if (m.vertex_count == 0) return m;

    formats::M3Region r;
    r.first_vertex = uint32_t(region_first);
    r.num_vertices = uint32_t(m.vertex_count - region_first);
    r.first_index = uint32_t(region_first_index);
    r.num_indices = uint32_t(m.indices.size() - region_first_index);
    m.regions.push_back(r);

    m.bounds_min[0] = xmin;
    m.bounds_min[1] = ymin;
    m.bounds_min[2] = zmin;
    m.bounds_max[0] = xmax;
    m.bounds_max[1] = ymax;
    m.bounds_max[2] = zmax;

    set_model_diffuse(m, diffuse_path);
    return m;
}

std::vector<std::uint8_t> creep_model_cells(const formats::Model& model,
                                            const sim::CreepGrid& grid) {
    std::vector<std::uint8_t> out;
    const int w = grid.width(), h = grid.height();
    if (w <= 0 || h <= 0) return out;
    out.assign(std::size_t(w) * h, 0);
    for (const formats::M3Region& reg : model.regions)
    for (std::size_t i = reg.first_index; i + 5 < std::size_t(reg.first_index) + reg.num_indices && i + 5 < model.indices.size(); i += 6) {
        const std::size_t a = std::size_t(model.indices[i]) + reg.first_vertex, c = std::size_t(model.indices[i + 2]) + reg.first_vertex;
        if (std::size_t(a) * 3 + 2 >= model.positions.size()) continue;
        if (std::size_t(c) * 3 + 2 >= model.positions.size()) continue;
        const float mx = (model.positions[std::size_t(a) * 3] +
                          model.positions[std::size_t(c) * 3]) *
                         0.5f;
        const float my = (model.positions[std::size_t(a) * 3 + 1] +
                          model.positions[std::size_t(c) * 3 + 1]) *
                         0.5f;
        int cx = 0, cy = 0;
        grid.to_cell(sim::Fixed::from_double(mx), sim::Fixed::from_double(my), cx, cy);
        if (!grid.in_bounds(cx, cy)) continue;
        out[std::size_t(cy) * w + cx] = 1;
    }
    return out;
}

std::vector<std::uint8_t> creep_grid_cells(const sim::CreepGrid& grid) {
    std::vector<std::uint8_t> out;
    const int w = grid.width(), h = grid.height();
    if (w <= 0 || h <= 0) return out;
    out.assign(std::size_t(w) * h, 0);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            out[std::size_t(y) * w + x] = grid.is_creep_cell(x, y) ? 1 : 0;
    return out;
}

}
