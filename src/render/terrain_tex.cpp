#include <cctype>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include "omc/render/terrain_tex.hpp"

#include "omc/render/creep_layer.hpp"

#include <cmath>
#include <exception>

#include "omc/casc/storage.hpp"
#include "omc/core/xml.hpp"
#include "omc/formats/dds.hpp"
#include "omc/formats/map_archive.hpp"

namespace omc::render {

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

bool decode_layer_rgba(const casc::Storage& store, const std::string& diffuse_rel, uint32_t& w,
                       uint32_t& h, std::vector<uint8_t>& out) {
    return decode_asset(store, diffuse_rel, w, h, out);
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
            std::string diffuse, normal, heightmap;
            float hscale = 1.0f, hoffset = 0.0f;
            for (const core::XmlNode& c : tex->children) {
                if (c.name == "Heightmap") {
                    if (const std::string* v = c.attr("value"); v && !v->empty()) heightmap = normalise_asset_path(*v);
                } else if (c.name == "HeightMapScale") {
                    if (const std::string* v = c.attr("value")) hscale = std::strtof(v->c_str(), nullptr);
                } else if (c.name == "HeightMapOffset") {
                    if (const std::string* v = c.attr("value")) hoffset = std::strtof(v->c_str(), nullptr);
                }
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
                layer.heightmap = std::move(heightmap);
                layer.height_scale = hscale;
                layer.height_offset = hoffset;
                break;
            }
        }
        if (const char* ov = std::getenv("OMC_TERRAIN_TEX_OVERRIDE"); ov && !layer.diffuse.empty()) {
            const std::string rel = normalise_asset_path(ov);
            if (diffuse_decodes(store, rel)) layer.diffuse = rel;
        }
        out.push_back(std::move(layer));
    }
    return out;
}

void set_terrain_diffuse(formats::Model& m, const std::string& diffuse_path) {
    set_model_diffuse(m, diffuse_path);
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
                           const formats::TextureMask& mask, float extent_x, float extent_y,
                           const formats::VertexTint* tint) {
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
    std::vector<formats::Model::SplatLayerImage> nrm(imgs.size());
    for (size_t i = 0; i < nrm.size(); ++i)
        if (!layers[i].normal.empty() && decode_asset(store, layers[i].normal, nrm[i].w, nrm[i].h, nrm[i].rgba))
            dxt5nm_to_rgb(nrm[i].rgba);
    const char* nflip = std::getenv("OMC_TERRAIN_NFLIP");
    if (const char* fl = nflip ? nflip : "y") {
        const bool fx = std::strchr(fl, 'x') != nullptr, fy = std::strchr(fl, 'y') != nullptr, sw = std::strchr(fl, 's') != nullptr;
        for (auto& im : nrm)
            for (size_t p = 0; p + 3 < im.rgba.size(); p += 4) {
                if (sw) std::swap(im.rgba[p], im.rgba[p + 1]);
                if (fx) im.rgba[p] = uint8_t(255 - im.rgba[p]);
                if (fy) im.rgba[p + 1] = uint8_t(255 - im.rgba[p + 1]);
            }
    }
    m.splat_height.assign(nrm.size(), {1.0f, 0.0f, 0.0f});
    static const bool kHeightBlend = std::getenv("OMC_HEIGHT_BLEND") != nullptr && std::getenv("OMC_NO_HEIGHT_BLEND") == nullptr;
    for (size_t i = 0; i < nrm.size(); ++i) {
        if (layers[i].heightmap.empty() || nrm[i].rgba.empty() || !kHeightBlend) continue;
        auto lower = [](std::string s) { for (char& ch : s) ch = char(std::tolower(static_cast<unsigned char>(ch))); return s; };
        if (lower(layers[i].heightmap) != lower(layers[i].normal)) {
            uint32_t hw = 0, hh = 0;
            std::vector<uint8_t> hr;
            if (!decode_asset(store, layers[i].heightmap, hw, hh, hr) || hw == 0 || hh == 0) continue;
            for (uint32_t y = 0; y < nrm[i].h; ++y)
                for (uint32_t x = 0; x < nrm[i].w; ++x) {
                    const uint32_t sx = uint32_t(uint64_t(x) * hw / nrm[i].w), sy = uint32_t(uint64_t(y) * hh / nrm[i].h);
                    nrm[i].rgba[(size_t(y) * nrm[i].w + x) * 4 + 3] = hr[(size_t(sy) * hw + sx) * 4];
                }
        }
        m.splat_height[i] = {layers[i].height_scale, layers[i].height_offset, 1.0f};
    }
    m.splat_normals = std::move(nrm);
    std::vector<uint8_t> planes(size_t(mask.mask_w) * mask.mask_h * 9, 255);
    for (uint32_t L = 0; L < 8; ++L) {
        uint8_t* dst = planes.data() + size_t(L) * mask.mask_w * mask.mask_h;
        for (uint32_t y = 0; y < mask.mask_h; ++y)
            for (uint32_t x = 0; x < mask.mask_w; ++x)
                dst[size_t(y) * mask.mask_w + x] = mask.layer_weight(L, x, y);
    }
    if (const char* only = std::getenv("OMC_SPLAT_ONLY")) {
        const uint32_t keep = uint32_t(std::atoi(only));
        for (uint32_t L = 0; L < 8; ++L)
            std::memset(planes.data() + size_t(L) * mask.mask_w * mask.mask_h, L == keep ? 255 : 0, size_t(mask.mask_w) * mask.mask_h);
    }
    if (tint) {
        uint8_t* dst = planes.data() + size_t(8) * mask.mask_w * mask.mask_h;
        const float sx = float(mask.mask_w) / extent_x, sy = float(mask.mask_h) / extent_y;
        for (const formats::VertexTint::Patch& p : tint->patches) {
            if (!p.plain()) continue;
            const int tx0 = std::max(0, int(p.x0 * sx)), tx1 = std::min(int(mask.mask_w) - 1, int(p.x1 * sx));
            const int ty0 = std::max(0, int(p.y0 * sy)), ty1 = std::min(int(mask.mask_h) - 1, int(p.y1 * sy));
            for (int ty = ty0; ty <= ty1; ++ty)
                for (int tx = tx0; tx <= tx1; ++tx) {
                    const float u = std::clamp(((float(tx) + 0.5f) / sx - p.x0) / (p.x1 - p.x0), 0.0f, 1.0f) * 4.0f;
                    const float v = std::clamp(((float(ty) + 0.5f) / sy - p.y0) / (p.y1 - p.y0), 0.0f, 1.0f) * 4.0f;
                    const int cx = std::min(3, int(u)), cy = std::min(3, int(v));
                    const float fx = u - float(cx), fy = v - float(cy);
                    const float a = float(p.at(cx, cy)) * (1 - fx) + float(p.at(cx + 1, cy)) * fx;
                    const float b = float(p.at(cx, cy + 1)) * (1 - fx) + float(p.at(cx + 1, cy + 1)) * fx;
                    dst[size_t(ty) * mask.mask_w + size_t(tx)] = uint8_t(a * (1 - fy) + b * fy + 0.5f);
                }
        }
    }
    if (std::getenv("OMC_TINT_TRACE")) {
        std::size_t dark = 0;
        const uint8_t* t8 = planes.data() + size_t(8) * mask.mask_w * mask.mask_h;
        for (size_t i = 0; i < size_t(mask.mask_w) * mask.mask_h; ++i) dark += t8[i] < 250;
        std::fprintf(stderr, "terrain tint: %zu patches, %zu of %zu texels shaded\n", tint ? tint->patches.size() : 0, dark, size_t(mask.mask_w) * mask.mask_h);
    }
    if (const char* fg = std::getenv("OMC_TERRAIN_FLAT_GREY")) {
        int r = 0, gch = 0, b = 0;
        if (std::sscanf(fg, "%d,%d,%d", &r, &gch, &b) != 3) r = gch = b = std::atoi(fg);
        const uint8_t v[3] = {uint8_t(std::clamp(r, 0, 255)), uint8_t(std::clamp(gch, 0, 255)), uint8_t(std::clamp(b, 0, 255))};
        for (auto& im : imgs)
            for (size_t i = 0; i + 3 < im.rgba.size(); i += 4) { im.rgba[i] = v[0]; im.rgba[i + 1] = v[1]; im.rgba[i + 2] = v[2]; im.rgba[i + 3] = 255; }
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

void apply_grid_uvs(formats::Model& m, float uv_tiles) { apply_grid_uvs(m, uv_tiles, uv_tiles); }

void apply_grid_uvs(formats::Model& m, float uv_tiles_u, float uv_tiles_v) {
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
    const char* fl = std::getenv("OMC_TERRAIN_UV_FLIP");
    if (!fl) fl = "v";
    const bool flip_u = std::strchr(fl, 'u') != nullptr, flip_v = std::strchr(fl, 'v') != nullptr;
    for (uint32_t i = 0; i < m.vertex_count; ++i) {
        float x = m.positions[i * 3], y = m.positions[i * 3 + 1];
        m.uvs[i * 2] = dx > 1e-6f ? (flip_u ? (maxx - x) : (x - minx)) / dx * uv_tiles_u : 0.0f;
        m.uvs[i * 2 + 1] = dy > 1e-6f ? (flip_v ? (maxy - y) : (y - miny)) / dy * uv_tiles_v : 0.0f;
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

void texture_map_ground(formats::Model& ground, const casc::Storage& store, const formats::LoadedMap& lm) {
    const std::vector<std::string>& palette = lm.has_splat ? lm.texture_info.layer_names : lm.tileset_textures;
    const float uv_tiles = lm.uv_tiling.found ? lm.uv_tiling.u : float(lm.width) / 8.0f;
    const float uv_tiles_v = lm.uv_tiling.found ? lm.uv_tiling.v : uv_tiles;
    if (lm.has_splat || lm.has_retail_splat) {
        const bool splat_vertices = std::getenv("OMC_SPLAT_EXTENT_VERTICES") != nullptr;
        const float splat_w = splat_vertices ? float(lm.width) : float(lm.cell_width());
        const float splat_h = splat_vertices ? float(lm.height) : float(lm.cell_height());
        if (set_terrain_splat_gpu(ground, store, palette, lm.texture_mask, splat_w, splat_h, &lm.vertex_tint)) {
            apply_grid_uvs(ground, uv_tiles, uv_tiles_v);
            return;
        }
        std::vector<ResolvedLayer> layers = resolve_map_layers(store, palette);
        GroundImage g = composite_map_ground(store, lm.has_splat ? lm.texture_info : formats::TextureInfo{}, lm.texture_mask, layers,
                                             std::max(1u, uint32_t(uv_tiles)));
        if (!g.empty()) {
            apply_grid_uvs(ground, 1.0f);
            set_terrain_diffuse_rgba(ground, g.w, g.h, std::move(g.rgba));
            return;
        }
    }
    std::string diff = resolve_map_tileset_diffuse(store, lm.tileset, lm.tileset_textures);
    if (diff.empty()) diff = resolve_tileset_diffuse(store);
    if (!diff.empty()) {
        apply_grid_uvs(ground, uv_tiles, uv_tiles_v);
        set_terrain_diffuse(ground, diff);
    }
}

}
