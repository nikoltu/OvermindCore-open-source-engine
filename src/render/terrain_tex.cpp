#include "sc2/render/terrain_tex.hpp"

#include <cmath>
#include <exception>

#include "sc2/casc/storage.hpp"
#include "sc2/core/xml.hpp"
#include "sc2/formats/dds.hpp"

namespace sc2::render {

namespace {

const char* const kGameDataRoots[] = {
    "mods/void.sc2mod/base.sc2data/GameData/",
    "mods/swarm.sc2mod/base.sc2data/GameData/",
    "mods/liberty.sc2mod/base.sc2data/GameData/",
    "mods/core.sc2mod/base.sc2data/GameData/",
};

const char* const kTexRoots[] = {"mods/liberty.sc2mod/base.sc2assets/",
                                  "mods/swarm.sc2mod/base.sc2assets/",
                                  "mods/void.sc2mod/base.sc2assets/",
                                  "mods/core.sc2mod/base.sc2assets/", ""};

std::vector<core::XmlNode> load_catalogs(const casc::Storage& store, const char* file) {
    std::vector<core::XmlNode> out;
    for (const char* root : kGameDataRoots) {
        try {
            std::vector<uint8_t> bytes = store.read_by_name(std::string(root) + file);
            out.push_back(core::xml_parse(bytes));
        } catch (const std::exception&) {
        }
    }
    return out;
}

const core::XmlNode* find_by_id(const core::XmlNode& root, const std::string& tag,
                                const std::string& id) {
    std::vector<const core::XmlNode*> stack{&root};
    while (!stack.empty()) {
        const core::XmlNode* n = stack.back();
        stack.pop_back();
        if (n->name == tag && n->attr_or("id", "") == id) return n;
        for (const core::XmlNode& c : n->children) stack.push_back(&c);
    }
    return nullptr;
}

std::string normalise_asset_path(std::string p) {
    for (char& c : p)
        if (c == '\\') c = '/';
    return p;
}

bool diffuse_decodes(const casc::Storage& store, const std::string& rel) {
    for (const char* root : kTexRoots) {
        try {
            std::vector<uint8_t> bytes = store.read_by_name(std::string(root) + rel);
            formats::DdsInfo info;
            std::vector<uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
            if (!rgba.empty() && info.width && info.height) return true;
        } catch (const std::exception&) {
        }
    }
    return false;
}

bool decode_asset(const casc::Storage& store, const std::string& rel, uint32_t& w, uint32_t& h,
                  std::vector<uint8_t>& out) {
    for (const char* root : kTexRoots) {
        try {
            std::vector<uint8_t> bytes = store.read_by_name(std::string(root) + rel);
            formats::DdsInfo info;
            std::vector<uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
            if (!rgba.empty() && info.width && info.height) {
                w = info.width;
                h = info.height;
                out = std::move(rgba);
                return true;
            }
        } catch (const std::exception&) {
        }
    }
    return false;
}

}

const std::vector<std::string>& tileset_diffuse_candidates() {
    static const std::vector<std::string> kCandidates = {
        "Assets/Textures/shakuras_dirt_cracked.dds",
        "Assets/Textures/slayngrass_1.dds",
        "Assets/Textures/shakuras_sand.dds",
        "Assets/Textures/portcity_terraintrims_diff.dds",
        "Assets/Textures/terrainlab.dds",
        "Assets/Textures/shakurassandmix_diffuse.dds",
    };
    return kCandidates;
}

std::string resolve_tileset_diffuse(const casc::Storage& store) {
    static const char* kRoots[] = {"mods/liberty.sc2mod/base.sc2assets/",
                                   "mods/core.sc2mod/base.sc2assets/",
                                   "mods/void.sc2mod/base.sc2assets/",
                                   "mods/swarm.sc2mod/base.sc2assets/", ""};
    for (const std::string& rel : tileset_diffuse_candidates())
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

std::string resolve_map_tileset_diffuse(const casc::Storage& store,
                                        const std::string& tileset,
                                        const std::vector<std::string>& preferred_ids) {
    if (tileset.empty() && preferred_ids.empty()) return {};

    std::vector<std::string> tex_ids = preferred_ids;

    if (!tileset.empty()) {
        std::vector<core::XmlNode> terrain_cats = load_catalogs(store, "TerrainData.xml");
        for (const core::XmlNode& cat : terrain_cats) {
            const core::XmlNode* ct = find_by_id(cat, "CTerrain", tileset);
            if (!ct) continue;
            bool any = false;
            for (const core::XmlNode& c : ct->children)
                if (c.name == "BlendTextures")
                    if (const std::string* v = c.attr("value"); v && !v->empty()) {
                        tex_ids.push_back(*v);
                        any = true;
                    }
            if (any) break;
        }
    }
    if (tex_ids.empty()) return {};

    std::vector<core::XmlNode> tex_cats = load_catalogs(store, "TerrainTexData.xml");
    for (const std::string& tid : tex_ids) {
        for (const core::XmlNode& cat : tex_cats) {
            const core::XmlNode* tex = find_by_id(cat, "CTerrainTex", tid);
            if (!tex) continue;
            for (const core::XmlNode& c : tex->children) {
                if (c.name != "Texture") continue;
                const std::string* v = c.attr("value");
                if (!v || v->empty()) continue;
                std::string rel = normalise_asset_path(*v);
                if (diffuse_decodes(store, rel)) return rel;
            }
        }
    }
    return {};
}

std::vector<ResolvedLayer> resolve_map_layers(const casc::Storage& store,
                                              const std::vector<std::string>& layer_names) {
    std::vector<ResolvedLayer> out;
    out.reserve(layer_names.size());
    if (layer_names.empty()) return out;

    std::vector<core::XmlNode> tex_cats = load_catalogs(store, "TerrainTexData.xml");

    for (const std::string& name : layer_names) {
        ResolvedLayer layer;
        layer.terrain_tex = name;
        for (const core::XmlNode& cat : tex_cats) {
            const core::XmlNode* tex = find_by_id(cat, "CTerrainTex", name);
            if (!tex) continue;
            std::string diffuse, normal;
            for (const core::XmlNode& c : tex->children) {
                if (diffuse.empty() && c.name == "Texture") {
                    if (const std::string* v = c.attr("value"); v && !v->empty()) {
                        std::string rel = normalise_asset_path(*v);
                        if (diffuse_decodes(store, rel)) diffuse = rel;
                    }
                } else if (normal.empty() && c.name == "Normalmap") {
                    if (const std::string* v = c.attr("value"); v && !v->empty())
                        normal = normalise_asset_path(*v);
                }
            }
            if (!diffuse.empty()) {
                layer.diffuse = std::move(diffuse);
                layer.normal = std::move(normal);
                break;
            }
        }
        out.push_back(std::move(layer));
    }
    return out;
}

void set_terrain_diffuse(formats::Model& m, const std::string& diffuse_path) {
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

GroundImage composite_ground_images(const formats::TextureInfo& info,
                                    const formats::TextureMask& mask,
                                    const std::vector<LayerImage>& images, uint32_t uv_tiles) {
    GroundImage out;
    if (mask.empty() || !mask.planar_ok() || images.empty() || uv_tiles == 0) return out;
    if (!info.empty() &&
        (mask.mask_w != info.width * 8 || mask.mask_h != info.height * 8))
        return out;

    int base = -1;
    for (size_t i = 0; i < images.size(); ++i)
        if (images[i].ok()) {
            base = int(i);
            break;
        }
    if (base < 0) return out;

    const uint32_t W = mask.mask_w, H = mask.mask_h;
    out.w = W;
    out.h = H;
    out.rgba.assign(size_t(W) * H * 4, 255);

    auto sample = [](const LayerImage& im, double u, double v, uint8_t* rgb) {
        uint32_t sx = uint32_t(u * im.w);
        uint32_t sy = uint32_t(v * im.h);
        if (sx >= im.w) sx = im.w - 1;
        if (sy >= im.h) sy = im.h - 1;
        const uint8_t* p = &im.rgba[(size_t(sy) * im.w + sx) * 4];
        rgb[0] = p[0];
        rgb[1] = p[1];
        rgb[2] = p[2];
    };

    const uint32_t nlayers = uint32_t(images.size() < 8 ? images.size() : 8);
    for (uint32_t my = 0; my < H; ++my) {
        for (uint32_t mx = 0; mx < W; ++mx) {
            double tu = (double(mx) / W) * uv_tiles;
            double tv = (double(my) / H) * uv_tiles;
            tu -= std::floor(tu);
            tv -= std::floor(tv);
            double acc[3] = {0, 0, 0};
            uint32_t wsum = 0;
            for (uint32_t L = 0; L < nlayers; ++L) {
                if (!images[L].ok()) continue;
                const uint8_t w = mask.layer_weight(L, mx, my);
                if (!w) continue;
                uint8_t ld[3];
                sample(images[L], tu, tv, ld);
                for (int c = 0; c < 3; ++c) acc[c] += double(w) * ld[c];
                wsum += w;
            }
            uint8_t diff[3];
            if (wsum == 0) {
                sample(images[base], tu, tv, diff);
            } else {
                for (int c = 0; c < 3; ++c) diff[c] = uint8_t(acc[c] / wsum + 0.5);
            }
            uint8_t* o = &out.rgba[(size_t(my) * W + mx) * 4];
            o[0] = diff[0];
            o[1] = diff[1];
            o[2] = diff[2];
            o[3] = 255;
        }
    }
    return out;
}

GroundImage composite_map_ground(const casc::Storage& store, const formats::TextureInfo& info,
                                 const formats::TextureMask& mask,
                                 const std::vector<ResolvedLayer>& layers, uint32_t uv_tiles) {
    std::vector<LayerImage> imgs(layers.size());
    for (size_t i = 0; i < layers.size(); ++i)
        if (!layers[i].diffuse.empty())
            decode_asset(store, layers[i].diffuse, imgs[i].w, imgs[i].h, imgs[i].rgba);
    return composite_ground_images(info, mask, imgs, uv_tiles);
}

bool set_terrain_splat_gpu(formats::Model& m, const casc::Storage& store,
                           const std::vector<std::string>& layer_names,
                           const formats::TextureMask& mask, float extent_x, float extent_y) {
    if (!mask.planar_ok() || layer_names.empty() || extent_x <= 0 || extent_y <= 0)
        return false;
    std::vector<ResolvedLayer> layers = resolve_map_layers(store, layer_names);
    std::vector<formats::Model::SplatLayerImage> imgs(layers.size() < 8 ? layers.size() : 8);
    bool any = false;
    for (size_t i = 0; i < imgs.size(); ++i)
        if (!layers[i].diffuse.empty() &&
            decode_asset(store, layers[i].diffuse, imgs[i].w, imgs[i].h, imgs[i].rgba))
            any = true;
    if (!any) return false;
    std::vector<uint8_t> planes(size_t(mask.mask_w) * mask.mask_h * 8);
    for (uint32_t L = 0; L < 8; ++L) {
        uint8_t* dst = planes.data() + size_t(L) * mask.mask_w * mask.mask_h;
        for (uint32_t y = 0; y < mask.mask_h; ++y)
            for (uint32_t x = 0; x < mask.mask_w; ++x)
                dst[size_t(y) * mask.mask_w + x] = mask.layer_weight(L, x, y);
    }
    m.splat_layers = std::move(imgs);
    m.splat_w = mask.mask_w;
    m.splat_h = mask.mask_h;
    m.splat_weights = std::move(planes);
    m.splat_extent_x = extent_x;
    m.splat_extent_y = extent_y;
    return true;
}

void set_terrain_diffuse_rgba(formats::Model& m, uint32_t w, uint32_t h,
                              std::vector<uint8_t> rgba) {
    m.materials.clear();
    m.material_refs.clear();
    if (rgba.empty() || w == 0 || h == 0 || w > 16384 || h > 16384 ||
        rgba.size() < size_t(w) * h * 4) {
        m.region_material.assign(m.regions.size(), -1);
        return;
    }
    formats::M3Layer layer;
    layer.usage = "Diffuse";
    layer.inline_w = w;
    layer.inline_h = h;
    layer.inline_rgba = std::move(rgba);
    formats::M3Material mat;
    mat.name = "terrain_splat";
    mat.layers.push_back(std::move(layer));
    m.materials.push_back(std::move(mat));
    formats::M3MaterialRef ref;
    ref.type = 1;
    ref.index = 0;
    m.material_refs.push_back(ref);
    m.region_material.assign(m.regions.size(), 0);
}

void apply_grid_uvs(formats::Model& m, float uv_tiles) {
    if (m.vertex_count == 0 || m.positions.size() < size_t(m.vertex_count) * 3) return;
    float minx = 1e30f, miny = 1e30f, maxx = -1e30f, maxy = -1e30f;
    for (uint32_t i = 0; i < m.vertex_count; ++i) {
        float x = m.positions[i * 3], y = m.positions[i * 3 + 1];
        minx = x < minx ? x : minx;
        maxx = x > maxx ? x : maxx;
        miny = y < miny ? y : miny;
        maxy = y > maxy ? y : maxy;
    }
    float dx = maxx - minx, dy = maxy - miny;
    m.uvs.assign(size_t(m.vertex_count) * 2, 0.0f);
    for (uint32_t i = 0; i < m.vertex_count; ++i) {
        float x = m.positions[i * 3], y = m.positions[i * 3 + 1];
        m.uvs[i * 2] = dx > 1e-6f ? (x - minx) / dx * uv_tiles : 0.0f;
        m.uvs[i * 2 + 1] = dy > 1e-6f ? (y - miny) / dy * uv_tiles : 0.0f;
    }
}

formats::Model build_terrain_model(float extent, uint32_t divs, float uv_tiles,
                                   const std::string& diffuse_path,
                                   const std::vector<float>& heights) {
    if (divs < 1) divs = 1;
    if (divs > 250) divs = 250;
    const uint32_t n = divs + 1;
    const float half = extent * 0.5f;
    const float step = extent / float(divs);
    const bool have_h = heights.size() == size_t(n) * n;

    formats::Model m;
    m.vertex_count = n * n;
    m.stride = 0;
    m.positions.reserve(size_t(m.vertex_count) * 3);
    m.uvs.reserve(size_t(m.vertex_count) * 2);
    float zmin = 1e30f, zmax = -1e30f;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            float wx = -half + float(x) * step;
            float wy = -half + float(y) * step;
            float wz = have_h ? heights[size_t(y) * n + x] : 0.0f;
            m.positions.push_back(wx);
            m.positions.push_back(wy);
            m.positions.push_back(wz);
            m.uvs.push_back(float(x) / float(divs) * uv_tiles);
            m.uvs.push_back(float(y) / float(divs) * uv_tiles);
            zmin = wz < zmin ? wz : zmin;
            zmax = wz > zmax ? wz : zmax;
        }

    m.indices.reserve(size_t(divs) * divs * 6);
    for (uint32_t y = 0; y < divs; ++y)
        for (uint32_t x = 0; x < divs; ++x) {
            uint16_t i0 = uint16_t(y * n + x);
            uint16_t i1 = uint16_t(y * n + x + 1);
            uint16_t i2 = uint16_t((y + 1) * n + x + 1);
            uint16_t i3 = uint16_t((y + 1) * n + x);
            m.indices.push_back(i0);
            m.indices.push_back(i1);
            m.indices.push_back(i2);
            m.indices.push_back(i0);
            m.indices.push_back(i2);
            m.indices.push_back(i3);
        }

    formats::M3Region r;
    r.first_vertex = 0;
    r.num_vertices = m.vertex_count;
    r.first_index = 0;
    r.num_indices = uint32_t(m.indices.size());
    m.regions.push_back(r);

    m.bounds_min[0] = -half;
    m.bounds_min[1] = -half;
    m.bounds_min[2] = zmin;
    m.bounds_max[0] = half;
    m.bounds_max[1] = half;
    m.bounds_max[2] = zmax;

    set_terrain_diffuse(m, diffuse_path);
    return m;
}

SceneInstance terrain_instance(const formats::Model& model) {
    SceneInstance inst;
    inst.model = &model;
    inst.world = Mat4::identity();
    inst.team = {0.0f, 0.0f, 0.0f, -1.0f};
    inst.seq = -1;
    inst.time_ms = 0.0f;
    return inst;
}

std::vector<uint8_t> make_checker_rgba(uint32_t w, uint32_t h, uint32_t cells) {
    if (w == 0) w = 1;
    if (h == 0) h = 1;
    if (cells == 0) cells = 1;
    std::vector<uint8_t> out(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t cx = x * cells / w, cy = y * cells / h;
            bool on = ((cx + cy) & 1u) != 0;
            float g = (float(x) + float(y)) / float(w + h);
            uint8_t hi = uint8_t(60.0f + 195.0f * g);
            uint8_t lo = uint8_t(20.0f + 80.0f * (1.0f - g));
            size_t i = (size_t(y) * w + x) * 4;
            out[i + 0] = on ? hi : lo;
            out[i + 1] = on ? uint8_t(255 - hi) : uint8_t(lo / 2 + 30);
            out[i + 2] = on ? lo : hi;
            out[i + 3] = 255;
        }
    return out;
}

}
