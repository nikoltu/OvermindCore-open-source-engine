#include <cstdlib>
#include <cstdio>
#include <cctype>
#include "omc/formats/m3.hpp"

#include <cmath>
#include <cstring>
#include <stdexcept>

#include "omc/core/byte_reader.hpp"

namespace omc::formats {

using core::ByteReader;

namespace {
uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}
uint16_t rd16(const uint8_t* p) {
    return uint16_t(uint16_t(p[0]) | uint16_t(p[1]) << 8);
}
float rd_f32(const uint8_t* p) {
    uint32_t u = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
                 uint32_t(p[3]) << 24;
    float f;
    __builtin_memcpy(&f, &u, 4);
    return f;
}
}

std::string m3_tag(uint32_t tag) {
    char c[4] = {char((tag >> 24) & 0xFF), char((tag >> 16) & 0xFF),
                 char((tag >> 8) & 0xFF), char(tag & 0xFF)};
    std::string s;
    for (char ch : c)
        s += (ch >= 32 && ch < 127) ? ch : '?';
    return s;
}

M3 M3::parse(std::vector<uint8_t> bytes) {
    ByteReader r(bytes);
    if (bytes.size() < 24 || r.u8() != '4' || r.u8() != '3' || r.u8() != 'D' ||
        r.u8() != 'M')
        throw std::runtime_error("m3: not an MD34 model");

    M3 m;
    m.index_offset = r.u32le();
    m.index_entries = r.u32le();
    r.u32le();
    m.modl_index = r.u32le();

    const uint64_t table_end =
        uint64_t(m.index_offset) + uint64_t(m.index_entries) * 16;
    if (m.index_offset < 24 || table_end > bytes.size())
        throw std::runtime_error("m3: section table out of range");

    m.sections.reserve(m.index_entries);
    r.seek(m.index_offset);
    for (uint32_t i = 0; i < m.index_entries; ++i) {
        M3Section s;
        s.tag = r.u32le();
        s.offset = r.u32le();
        s.count = r.u32le();
        s.version = r.u32le();
        m.sections.push_back(s);
    }
    if (const M3Section* modl = m.section_by_index(m.modl_index))
        m.modl_version = modl->version;
    m.data = std::move(bytes);
    return m;
}

M3Ref M3::read_ref(size_t offset) const {
    M3Ref ref;
    if (offset + 12 > data.size()) return ref;
    ByteReader r(data.data(), data.size());
    r.seek(offset);
    ref.count = r.u32le();
    ref.index = r.u32le();
    ref.flags = r.u32le();
    return ref;
}

std::string M3::char_ref(const M3Ref& r) const {
    const M3Section* cs = section_by_index(r.index);
    if (!cs || r.count == 0 || cs->offset > data.size() || r.count > data.size() - cs->offset) return {};
    std::string s(reinterpret_cast<const char*>(&data[cs->offset]), r.count);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

uint32_t M3::first_section_with_tag(const char* tag4) const {
    for (uint32_t i = 0; i < sections.size(); ++i)
        if (m3_tag(sections[i].tag) == tag4) return i;
    return UINT32_MAX;
}

std::vector<uint32_t> M3::u32_array(const M3Ref& r) const {
    std::vector<uint32_t> out;
    const M3Section* s = section_by_index(r.index);
    if (!s) return out;
    for (uint32_t i = 0; i < r.count; ++i) {
        size_t o = s->offset + size_t(i) * 4;
        if (o + 4 > data.size()) break;
        out.push_back(rd32(&data[o]));
    }
    return out;
}

std::vector<uint8_t> M3::section_bytes(uint32_t index) const {
    const M3Section* s = section_by_index(index);
    if (!s) return {};
    size_t start = s->offset;
    size_t end = index_offset;
    for (const M3Section& o : sections)
        if (o.offset > start && o.offset < end) end = o.offset;
    if (start > data.size() || end > data.size() || end < start) return {};
    return std::vector<uint8_t>(data.begin() + start, data.begin() + end);
}

Model M3::decode_model() const {
    const M3Section* modl = section_by_index(modl_index);
    if (!modl) throw std::runtime_error("m3: no MODL section");
    size_t o = modl->offset;
    if (o + 164 > data.size()) throw std::runtime_error("m3: MODL truncated");

    Model m;
    auto ref = [&](size_t off) { return read_ref(off); };
    m.name = model_name();
    M3Ref bones = ref(o + 80);
    m.vertex_flags = rd32(&data[o + 96]);
    M3Ref vertices = ref(o + 100);
    M3Ref divisions = ref(o + 112);
    size_t bnds = o + 136;
    for (int i = 0; i < 3; ++i) m.bounds_min[i] = rd_f32(&data[bnds + i * 4]);
    for (int i = 0; i < 3; ++i) m.bounds_max[i] = rd_f32(&data[bnds + 12 + i * 4]);
    m.bounds_radius = rd_f32(&data[bnds + 24]);

    m.bone_count = bones.count;
    m.particles = decode_particles();
    m.lights = decode_lights();
    m.ribbons = decode_ribbons();
    for (uint32_t si = 0; si < sections.size(); ++si) {
        if (m3_tag(sections[si].tag) != "BBSC" || sections[si].version != 0) continue;
        const M3Section& s = sections[si];
        for (uint32_t i = 0; i < s.count; ++i) {
            const size_t base = size_t(s.offset) + size_t(i) * 48;
            if (base + 48 > data.size()) break;
            Model::Billboard bb;
            bb.bone = uint16_t(data[base + 12] | (data[base + 13] << 8));
            bb.type = uint16_t(data[base + 14] | (data[base + 15] << 8));
            if (bb.bone < bones.count) m.billboards.push_back(bb);
        }
    }
    m.projectors = decode_projectors();

    const M3Section* vsec = section_by_index(vertices.index);
    if (!vsec) throw std::runtime_error("m3: bad vertices ref");
    uint32_t vbytes = vertices.count;
    if (vsec->offset + size_t(vbytes) > data.size())
        throw std::runtime_error("m3: vertices out of range");
    const uint8_t* vbuf = &data[vsec->offset];

    auto meshless = [&]() {
        decode_materials(m);
        decode_animation(m);
        return m;
    };
    const M3Section* dsec = section_by_index(divisions.index);
    if (!dsec || divisions.count < 1) return meshless();
    M3Ref faces = read_ref(dsec->offset);
    M3Ref regions = read_ref(dsec->offset + 12);
    const M3Section* rsec = section_by_index(regions.index);
    if (!rsec || regions.count == 0) return meshless();
    if (rsec->version < 3)
        throw std::runtime_error("m3: REGN version <3 unsupported");
    uint32_t rstride = rsec->version == 5 ? 48 : rsec->version == 4 ? 40 : 36;

    uint64_t total_verts = 0, total_indices = 0;
    for (uint32_t i = 0; i < regions.count; ++i) {
        size_t r = rsec->offset + size_t(i) * rstride;
        if (r + 24 > data.size()) throw std::runtime_error("m3: REGN truncated");
        M3Region reg;
        reg.first_vertex = rd32(&data[r + 8]);
        reg.num_vertices = rd32(&data[r + 12]);
        reg.first_index = rd32(&data[r + 16]);
        reg.num_indices = rd32(&data[r + 20]);
        if (r + 28 <= data.size()) reg.first_bone_lookup = rd16(&data[r + 26]);
        if (rsec->version == 5 && r + 48 <= data.size()) {
            const float mu = rd_f32(&data[r + 40]), of = rd_f32(&data[r + 44]);
            if (std::isfinite(mu) && std::isfinite(of) && mu != 0.0f) { reg.uv_multiply = mu; reg.uv_offset = of; }
        }
        total_verts += reg.num_vertices;
        total_indices += reg.num_indices;
        m.regions.push_back(reg);
    }
    if (total_indices != faces.count)
        throw std::runtime_error("m3: Σ region indices != face count");

    if (total_verts == 0 || vbytes % total_verts != 0)
        throw std::runtime_error("m3: vertex bytes not divisible by vertex count");
    uint32_t stride = uint32_t(vbytes / total_verts);
    if (stride < 32 || stride > 64 || stride % 4 != 0)
        throw std::runtime_error("m3: implausible vertex stride");
    m.stride = stride;
    m.vertex_count = uint32_t(total_verts);

    for (const M3Region& reg : m.regions)
        if (uint64_t(reg.first_vertex) + reg.num_vertices > m.vertex_count ||
            uint64_t(reg.first_index) + reg.num_indices > faces.count)
            throw std::runtime_error("m3: region range out of bounds");

    const M3Section* fsec = section_by_index(faces.index);
    if (!fsec || fsec->offset + size_t(faces.count) * 2 > data.size())
        throw std::runtime_error("m3: bad faces (U16_) ref");
    m.indices.reserve(faces.count);
    for (uint32_t i = 0; i < faces.count; ++i)
        m.indices.push_back(rd16(&data[fsec->offset + size_t(i) * 2]));

    m.positions.reserve(size_t(m.vertex_count) * 3);
    const bool has_vertex_color = (m.vertex_flags & 0x200u) != 0;
    const uint32_t uv_off = has_vertex_color ? 28u : 24u;
    bool has_uv = stride >= uv_off + 4;
    if (has_uv) m.uvs.reserve(size_t(m.vertex_count) * 2);
    const bool has_uv1 = has_uv && (m.vertex_flags & 0x40000u) != 0 && stride >= uv_off + 12;
    float mn[3] = {1e30f, 1e30f, 1e30f}, mx[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < m.vertex_count; ++i) {
        const uint8_t* p = vbuf + size_t(i) * stride;
        for (int a = 0; a < 3; ++a) {
            float v = rd_f32(p + a * 4);
            m.positions.push_back(v);
            if (v < mn[a]) mn[a] = v;
            if (v > mx[a]) mx[a] = v;
        }
        if (has_uv) {
            int16_t u = int16_t(rd16(p + uv_off));
            int16_t vv = int16_t(rd16(p + uv_off + 2));
            m.uvs.push_back(float(u) / 2048.0f);
            m.uvs.push_back(float(vv) / 2048.0f);
        }
        if (has_uv1) {
            m.uvs1.push_back(float(int16_t(rd16(p + uv_off + 4))) / 2048.0f);
            m.uvs1.push_back(float(int16_t(rd16(p + uv_off + 6))) / 2048.0f);
        }
        if (has_vertex_color && stride >= 28)
            for (int a = 0; a < 4; ++a) m.vertex_colors.push_back(p[24 + a]);
        {
            for (int a = 0; a < 3; ++a) m.normals.push_back(float(p[20 + a]) / 255.0f * 2.0f - 1.0f);
            m.normals_w.push_back(p[23]);
            for (int a = 0; a < 4; ++a) m.tangents.push_back(float(p[stride - 4 + a]) / 255.0f * 2.0f - 1.0f);
        }
    }
    if (has_uv && !std::getenv("OMC_NO_REGN_UV_SCALE")) {
        std::vector<char> done(m.vertex_count, 0);
        for (const M3Region& reg : m.regions) {
            if (reg.uv_multiply == 16.0f && reg.uv_offset == 0.0f) continue;
            for (uint32_t i = reg.first_vertex; i < reg.first_vertex + reg.num_vertices; ++i) {
                if (done[i]) continue;
                done[i] = 1;
                const uint8_t* p = vbuf + size_t(i) * stride;
                auto tr = [&](uint32_t off) { return float(int16_t(rd16(p + off))) / 32768.0f * reg.uv_multiply + reg.uv_offset; };
                m.uvs[size_t(i) * 2] = tr(uv_off);
                m.uvs[size_t(i) * 2 + 1] = tr(uv_off + 2);
                if (has_uv1) {
                    m.uvs1[size_t(i) * 2] = tr(uv_off + 4);
                    m.uvs1[size_t(i) * 2 + 1] = tr(uv_off + 6);
                }
            }
        }
    }
    for (int a = 0; a < 3; ++a)
        if (!(mn[a] == mn[a]) || !(mx[a] == mx[a]) ||
            mn[a] < -1e6f || mx[a] > 1e6f)
            throw std::runtime_error("m3: implausible vertex positions");

    decode_materials(m);
    split_terrain_regions(m);
    decode_animation(m);
    return m;
}

void M3::split_terrain_regions(Model& m) const {
    const uint32_t matm = first_section_with_tag("MATM"), bat = first_section_with_tag("BAT_");
    if (matm == UINT32_MAX || bat == UINT32_MAX || m.regions.empty()) return;
    const M3Section* ms = section_by_index(matm);
    const M3Section* bs = section_by_index(bat);
    if (!ms || !bs) return;
    std::vector<char> terrain(m.regions.size(), 0), other(m.regions.size(), 0);
    for (uint32_t i = 0; i < bs->count; ++i) {
        const size_t r = bs->offset + size_t(i) * 14;
        if (r + 14 > data.size()) break;
        const uint32_t region = rd16(&data[r + 4]), mref = rd16(&data[r + 10]);
        if (region >= m.regions.size()) continue;
        const size_t mo = ms->offset + size_t(mref) * 8;
        if (mref < ms->count && mo + 8 <= data.size() && rd32(&data[mo]) == 4) terrain[region] = 1;
        else other[region] = 1;
    }
    bool any = false;
    for (size_t i = 0; i < terrain.size(); ++i)
        if (terrain[i] && !other[i]) any = true;
    if (!any) return;
    const bool sized = m.region_material.size() == m.regions.size();
    std::vector<M3Region> keep;
    std::vector<int> keep_mat;
    const bool ext = m.region_composite.size() == m.regions.size() && m.region_matref.size() == m.regions.size() &&
                     m.region_batches.size() == m.regions.size();
    std::vector<int> keep_cmp, keep_ref;
    std::vector<uint16_t> keep_bat;
    const bool has_bone = m.region_bone.size() == m.regions.size();
    std::vector<int> keep_bone;
    for (size_t i = 0; i < m.regions.size(); ++i) {
        if (terrain[i] && !other[i]) {
            m.terrain_regions.push_back(m.regions[i]);
        } else {
            keep.push_back(m.regions[i]);
            if (has_bone) keep_bone.push_back(m.region_bone[i]);
            if (sized) keep_mat.push_back(m.region_material[i]);
            if (ext) {
                keep_cmp.push_back(m.region_composite[i]);
                keep_ref.push_back(m.region_matref[i]);
                keep_bat.push_back(m.region_batches[i]);
            }
        }
    }
    m.regions = std::move(keep);
    if (has_bone) m.region_bone = std::move(keep_bone);
    if (sized) m.region_material = std::move(keep_mat);
    if (ext) {
        m.region_composite = std::move(keep_cmp);
        m.region_matref = std::move(keep_ref);
        m.region_batches = std::move(keep_bat);
    }
}

namespace {
struct LayerSlot {
    const char* usage;
    int min_ver;
};
const LayerSlot kLayerSlots[] = {
    {"Diffuse", 0},    {"Decal", 0},          {"Specular", 0},
    {"Gloss", 15},     {"Emissive", 0},       {"Emissive2", 0},
    {"EnvironmentVIO", 0}, {"EnvironmentVIOMask", 0}, {"AlphaMask", 0},
    {"AlphaMask2", 0}, {"Normal", 0},         {"Height", 0},
    {"LightMap", 0},   {"AmbientOcclusion", 0}, {"Unknown0", 18},
    {"Unknown1", 18},  {"Unknown2", 18},      {"Unknown3", 18},
};
}

void M3::decode_materials(Model& m) const {
    const M3Section* modl = section_by_index(modl_index);
    if (!modl) return;
    size_t mo = modl->offset;

    auto read_char = [&](const M3Ref& r) { return char_ref(r); };
    auto find_tag = [&](const char* tag4) { return first_section_with_tag(tag4); };
    size_t modl_span = section_bytes(modl_index).size();
    auto find_ref_to = [&](uint32_t idx) -> M3Ref {
        if (idx == UINT32_MAX || modl_span < 12) return {};
        for (size_t off = mo + 4; off + 12 <= mo + modl_span; off += 4) {
            M3Ref r = read_ref(off);
            if (r.index == idx && r.count == sections[idx].count && r.count > 0)
                return r;
        }
        return {};
    };
    auto layer_from = [&](const M3Ref& lr, const std::string& usage, M3Layer& layer) -> bool {
        if (lr.count == 0 || lr.index >= sections.size()) return false;
        const M3Section* ls = section_by_index(lr.index);
        if (!ls || m3_tag(ls->tag) != "LAYR") return false;
        M3Ref img = read_ref(ls->offset + 4);
        std::string path = read_char(img);
        static const bool no_const = std::getenv("OMC_NO_CONST_COLOUR_LAYERS") != nullptr;
        const bool const_colour = path.empty() && !no_const && ls->offset + 40 <= data.size() &&
                                  (rd32(&data[ls->offset + 36]) & 0x400u) != 0;
        if (path.empty() && !const_colour) return false;
        layer.usage = usage;
        layer.image_path = path;
        if (const_colour) {
            const uint8_t* c = &data[ls->offset + 24];
            layer.inline_rgba = {c[2], c[1], c[0], c[3]};
            layer.inline_w = layer.inline_h = 1;
            layer.const_colour = true;
        }
        if (ls->offset + 48 <= data.size()) {
            layer.word36 = rd32(&data[ls->offset + 36]);
            layer.word40 = rd32(&data[ls->offset + 40]);
            layer.word44 = rd32(&data[ls->offset + 44]);
        }
        layer.layr_version = ls->version;
        {
            const size_t base = ls->offset;
            const size_t rec = ls->count ? section_bytes(lr.index).size() / ls->count : 0;
            auto ref = [&](size_t off, uint16_t& interp, uint16_t& flags, uint32_t& id, float* init, int n) {
                if (off + 8 + size_t(n) * 4 > rec || base + off + 8 + size_t(n) * 4 > data.size()) return;
                interp = rd16(&data[base + off]);
                flags = rd16(&data[base + off + 2]);
                id = rd32(&data[base + off + 4]);
                for (int i = 0; i < n; ++i) init[i] = rd_f32(&data[base + off + 8 + size_t(i) * 4]);
            };
            M3Layer::AnimRefs& a = layer.anim;
            if (rec >= 28 && base + 28 <= data.size())
                for (int i = 0; i < 4; ++i) a.color[i] = float(data[base + 24 + size_t(i)]) / 255.0f;
            ref(48, a.bright_mult_interp, a.bright_mult_flags, a.bright_mult_id, &a.bright_mult, 1);
            ref(68, a.midtone_interp, a.midtone_flags, a.midtone_id, &a.midtone_offset, 1);
            const size_t d = ls->version > 23 ? 8 : 0;
            ref(180 + d, a.uv_offset_interp, a.uv_offset_flags, a.uv_offset_id, a.uv_offset, 2);
            ref(208 + d, a.uv_angle_interp, a.uv_angle_flags, a.uv_angle_id, a.uv_angle, 3);
            ref(244 + d, a.uv_tiling_interp, a.uv_tiling_flags, a.uv_tiling_id, a.uv_tiling, 2);
            ref(312 + d, a.brightness_interp, a.brightness_flags, a.brightness_id, &a.brightness, 1);
            auto null1 = [&](size_t off, float& out) {
                if (off + 16 <= rec && base + off + 16 <= data.size()) out = rd_f32(&data[base + off + 12]);
            };
            null1(48, a.bright_mult_null);
            null1(312 + d, a.brightness_null);
            static const bool kFlipbook = std::getenv("OMC_NO_FLIPBOOK") == nullptr;
            const size_t fb = ls->version <= 23 ? 156 : 164;
            if (kFlipbook && ls->version <= 26 && rec >= fb + 20 && base + fb + 20 <= data.size()) {
                const uint32_t rows = rd32(&data[base + fb]), cols = rd32(&data[base + fb + 4]);
                if (rows >= 1 && rows <= 32 && cols >= 1 && cols <= 32 && rows * cols > 1) {
                    a.flipbook_rows = rows;
                    a.flipbook_cols = cols;
                    a.flipbook_frame_interp = rd16(&data[base + fb + 8]);
                    a.flipbook_frame_flags = rd16(&data[base + fb + 10]);
                    a.flipbook_frame_id = rd32(&data[base + fb + 12]);
                    a.flipbook_frame = float(rd32(&data[base + fb + 16]));
                }
            }
        }
        {
            const size_t rec = ls->count ? section_bytes(lr.index).size() / ls->count : 0;
            if (rec && ls->offset + rec <= data.size())
                layer.raw_record.assign(data.begin() + ls->offset, data.begin() + ls->offset + rec);
        }
        return true;
    };

    uint32_t matm_idx = find_tag("MATM");
    uint32_t mat_idx = find_tag("MAT_");
    uint32_t layr_idx = find_tag("LAYR");
    if (mat_idx == UINT32_MAX) return;

    M3Ref matm_ref = find_ref_to(matm_idx);
    if (matm_ref.count) {
        const M3Section* ms = section_by_index(matm_idx);
        for (uint32_t i = 0; i < matm_ref.count; ++i) {
            size_t r = ms->offset + size_t(i) * 8;
            if (r + 8 > data.size()) break;
            m.material_refs.push_back({rd32(&data[r]), rd32(&data[r + 4])});
        }
    }

    const M3Section* mats = section_by_index(mat_idx);
    M3Ref mat_ref = find_ref_to(mat_idx);
    uint32_t mat_count = mat_ref.count ? mat_ref.count : mats->count;
    if (mat_count == 0) return;
    size_t mat_span = section_bytes(mat_idx).size();

    std::vector<const char*> slots;
    for (const LayerSlot& s : kLayerSlots)
        if (s.min_ver == 0 || int(mats->version) > s.min_ver) slots.push_back(s.usage);
    const size_t L = slots.size();

    auto is_layer_like = [&](const M3Ref& r) {
        if (r.count == 0 && r.index == 0) return true;
        return r.index < sections.size() && m3_tag(sections[r.index].tag) == "LAYR";
    };
    auto scan_block = [&](size_t base, size_t limit) -> size_t {
        for (size_t off = base + 12; off + L * 12 <= limit; off += 4) {
            bool all = true, any_real = false;
            for (size_t k = 0; k < L; ++k) {
                M3Ref r = read_ref(off + k * 12);
                if (!is_layer_like(r)) { all = false; break; }
                if (r.count != 0 && layr_idx != UINT32_MAX) any_real = true;
            }
            if (all && any_real) return off;
        }
        return 0;
    };

    uint32_t mat_rec = 0;
    if (size_t b0 = scan_block(mats->offset, mats->offset + mat_span)) {
        if (size_t b1 = scan_block(b0 + L * 12, mats->offset + mat_span))
            mat_rec = uint32_t(b1 - b0);
    }
    if (mat_rec == 0 || size_t(mat_count) * mat_rec > mat_span ||
        mat_span - size_t(mat_count) * mat_rec >= 16)
        mat_rec = uint32_t(mat_span / mat_count);
    if (mat_rec < 24) return;

    for (uint32_t mi = 0; mi < mat_count; ++mi) {
        size_t rec = mats->offset + size_t(mi) * mat_rec;
        if (rec + mat_rec > data.size()) break;
        M3Material mat;
        mat.name = read_char(read_ref(rec));
        mat.raw_record.assign(data.begin() + std::ptrdiff_t(rec), data.begin() + std::ptrdiff_t(rec + mat_rec));
        if (rec + 36 <= data.size() && mat_rec >= 36) {
            mat.flags = rd32(&data[rec + 16]);
            mat.blend_mode = rd32(&data[rec + 20]);
            mat.priority = int32_t(rd32(&data[rec + 24]));
            const uint32_t sp = rd32(&data[rec + 32]);
            std::memcpy(&mat.specularity, &sp, 4);
        }
        if (rec + 44 <= data.size() && mat_rec >= 44) mat.cutout_thresh = data[rec + 40];
        if (rec + 40 <= data.size() && mat_rec >= 40) {
            const float db = rd_f32(&data[rec + 36]);
            if (db == db && db >= 0.0f && db <= 64.0f) mat.depth_blend = db;
        }
        if (rec + 52 <= data.size() && mat_rec >= 52) {
            const float sm = rd_f32(&data[rec + 44]), em = rd_f32(&data[rec + 48]);
            if (sm == sm && sm >= 0.0f && sm <= 64.0f) mat.spec_mult = sm;
            if (em == em && em >= 0.0f && em <= 64.0f) mat.emis_mult = em;
        }
        size_t block = scan_block(rec, rec + mat_rec);
        if (block) {
            auto name_score = [&](size_t b) {
                static const char* const kw[][2] = {{"Diffuse", "diff"}, {"Normal", "norm"}, {"Specular", "spec"},
                                                    {"Emissive", "emis"}};
                int sc = 0;
                for (size_t k = 0; k < L; ++k) {
                    M3Ref lr = read_ref(b + k * 12);
                    if (lr.count == 0 || lr.index >= sections.size()) continue;
                    const M3Section* ls = section_by_index(lr.index);
                    if (!ls || m3_tag(ls->tag) != "LAYR") continue;
                    std::string path = read_char(read_ref(ls->offset + 4));
                    for (char& c : path) c = char(std::tolower(static_cast<unsigned char>(c)));
                    for (const auto& w : kw) {
                        if (path.find(w[1]) == std::string::npos) continue;
                        sc += std::string(slots[k]) == w[0] ? 1 : -1;
                    }
                }
                return sc;
            };
            size_t best = block;
            int best_sc = name_score(block);
            for (size_t alt = block + 12; alt + L * 12 <= rec + mat_rec && alt <= block + 24; alt += 12) {
                bool ok = true;
                for (size_t k = 0; k < L && ok; ++k) ok = is_layer_like(read_ref(alt + k * 12));
                if (!ok) break;
                const int sc = name_score(alt);
                if (sc > best_sc) { best_sc = sc; best = alt; }
            }
            block = best;
        }
        if (block) {
            if (block + L * 12 + 12 <= rec + mat_rec && block + L * 12 + 12 <= data.size()) {
                const uint32_t op = rd32(&data[block + L * 12 + 8]);
                if (op <= 8) mat.emis_op = op;
                if (block + L * 12 + 16 <= rec + mat_rec && block + L * 12 + 16 <= data.size()) {
                    const uint32_t op2 = rd32(&data[block + L * 12 + 12]);
                    if (op2 <= 8) mat.emis2_op = op2;
                }
            }
            for (size_t k = 0; k < L; ++k) {
                M3Layer layer;
                if (layer_from(read_ref(block + k * 12), slots[k], layer)) mat.layers.push_back(std::move(layer));
            }
        }
        m.materials.push_back(std::move(mat));
    }

    std::vector<int> dis_material;
    static const bool kDisplace = std::getenv("OMC_NO_DISPLACEMENT") == nullptr;
    if (const uint32_t di = find_tag("DIS_"); kDisplace && di != UINT32_MAX) {
        const M3Section* ds = section_by_index(di);
        if (ds && ds->version == 4 && section_bytes(di).size() >= size_t(ds->count) * 68)
            for (uint32_t k = 0; k < ds->count; ++k) {
                const size_t b = ds->offset + size_t(k) * 68;
                if (b + 68 > data.size()) break;
                M3Material dm;
                dm.name = read_char(read_ref(b));
                dm.blend_mode = kBlendAlphaBlend;
                dm.displace_interp = rd16(&data[b + 16]);
                dm.displace_flags = rd16(&data[b + 18]);
                dm.displace_id = rd32(&data[b + 20]);
                dm.displace = rd_f32(&data[b + 24]);
                M3Layer nl, sl;
                if (layer_from(read_ref(b + 36), "Normal", nl)) dm.layers.push_back(std::move(nl));
                if (layer_from(read_ref(b + 48), "AlphaMask", sl)) dm.layers.push_back(std::move(sl));
                if (!(dm.displace > 0.0f)) dm.displace = 1e-3f;
                dis_material.push_back(int(m.materials.size()));
                m.materials.push_back(std::move(dm));
            }
    }

    auto composite_first_standard = [&](uint32_t cmp_index) -> int {
        const uint32_t ci = find_tag("CMP_");
        if (ci == UINT32_MAX) return -1;
        const M3Section* cs = section_by_index(ci);
        if (!cs || cmp_index >= cs->count) return -1;
        const size_t span = section_bytes(ci).size(), rec = span / std::max<uint32_t>(cs->count, 1);
        const size_t base = cs->offset + size_t(cmp_index) * rec;
        if (std::getenv("OMC_BAT_TRACE")) {
            std::fprintf(stderr, "CMP_ count %u rec %zu ver %u refs:", cs->count, rec, cs->version);
            for (size_t off = base; off + 12 <= base + rec; off += 4) { const M3Ref r = read_ref(off); if (r.index < sections.size() && r.count) std::fprintf(stderr, " @%zu->%s(%u)", off - base, m3_tag(sections[r.index].tag).c_str(), r.count); }
            std::fprintf(stderr, "\n");
        }
        for (size_t off = base; off + 12 <= base + rec; off += 4) {
            const M3Ref r = read_ref(off);
            if (r.count == 0 || r.index >= sections.size() || m3_tag(sections[r.index].tag) != "CMS_") continue;
            const M3Section* ks = section_by_index(r.index);
            const size_t kspan = section_bytes(r.index).size();
            const size_t krec = kspan >= size_t(ks->count) * 24 ? size_t(24) : kspan / std::max<uint32_t>(ks->count, 1);
            for (uint32_t k = 0; k < ks->count; ++k) {
                const size_t ko = ks->offset + size_t(k) * krec;
                if (ko + 4 > data.size()) break;
                const uint32_t mref = rd32(&data[ko]);
                if (mref < m.material_refs.size() && m.material_refs[mref].type == 1 &&
                    m.material_refs[mref].index < m.materials.size())
                    return int(m.material_refs[mref].index);
            }
            return -1;
        }
        return -1;
    };
    if (const uint32_t ci = find_tag("CMP_"); ci != UINT32_MAX) {
        const M3Section* cs = section_by_index(ci);
        const size_t span = section_bytes(ci).size();
        const size_t rec = cs && span >= size_t(cs->count) * 28 ? size_t(28) : 0;
        for (uint32_t c = 0; rec && c < cs->count; ++c) {
            const size_t base = cs->offset + size_t(c) * rec;
            if (base + rec > data.size()) break;
            M3Composite cmp;
            cmp.name = read_char(read_ref(base));
            cmp.word12 = rd32(&data[base + 12]);
            const M3Ref kr = read_ref(base + 16);
            const M3Section* ks = kr.count && kr.index < sections.size() ? section_by_index(kr.index) : nullptr;
            if (ks && m3_tag(ks->tag) == "CMS_" && section_bytes(kr.index).size() >= size_t(ks->count) * 24) {
                for (uint32_t k = 0; k < ks->count && k < kr.count; ++k) {
                    const size_t ko = ks->offset + size_t(k) * 24;
                    if (ko + 24 > data.size()) break;
                    M3CompositeChild ch;
                    ch.matref = rd32(&data[ko]);
                    ch.alpha_interp = rd16(&data[ko + 4]);
                    ch.alpha_flags = rd16(&data[ko + 6]);
                    ch.alpha_id = rd32(&data[ko + 8]);
                    ch.alpha = rd_f32(&data[ko + 12]);
                    if (ch.matref < m.material_refs.size()) {
                        ch.type = m.material_refs[ch.matref].type;
                        ch.index = m.material_refs[ch.matref].index;
                        ch.resolved = true;
                    }
                    cmp.children.push_back(ch);
                }
            }
            m.composites.push_back(std::move(cmp));
        }
    }
    uint32_t bat_idx = find_tag("BAT_");
    if (bat_idx != UINT32_MAX && !m.regions.empty()) {
        m.region_material.assign(m.regions.size(), -1);
        m.region_composite.assign(m.regions.size(), -1);
        m.region_matref.assign(m.regions.size(), -1);
        m.region_batches.assign(m.regions.size(), 0);
        const M3Section* bs = section_by_index(bat_idx);
        for (uint32_t i = 0; i < bs->count; ++i) {
            size_t r = bs->offset + size_t(i) * 14;
            if (r + 14 > data.size()) break;
            uint32_t region = rd16(&data[r + 4]);
            uint32_t mref = rd16(&data[r + 10]);
            if (region >= m.regions.size() || mref >= m.material_refs.size()) continue;
            const M3MaterialRef& mr = m.material_refs[mref];
            if (std::getenv("OMC_BAT_TRACE")) std::fprintf(stderr, "bat region %u mref %u type %u index %u\n", region, mref, mr.type, mr.index);
            if (mr.type == 1 && mr.index < m.materials.size())
                m.region_material[region] = int(mr.index);
            if (mr.type == 2 && mr.index < dis_material.size()) m.region_material[region] = dis_material[mr.index];
            if (mr.type == 3) {
                const int child = composite_first_standard(mr.index);
                if (child >= 0) m.region_material[region] = child;
            }
            m.region_matref[region] = int(mref);
            m.region_composite[region] = mr.type == 3 && mr.index < m.composites.size() ? int(mr.index) : -1;
            if (m.region_batches[region] < 0xFFFFu) ++m.region_batches[region];
            if (m.region_bone.size() != m.regions.size()) m.region_bone.assign(m.regions.size(), -1);
            m.region_bone[region] = int(int16_t(rd16(&data[r + 12])));
        }
    }
}

void M3::decode_animation(Model& m) const {
    const M3Section* modl = section_by_index(modl_index);
    if (!modl) return;
    size_t mo = modl->offset;

    auto read_char = [&](const M3Ref& r) { return char_ref(r); };
    auto find_tag = [&](const char* tag4) { return first_section_with_tag(tag4); };
    auto read_u32_array = [&](const M3Ref& r) { return u32_array(r); };
    auto read_anim_ref = [&](size_t off, int comps) {
        M3AnimRef a;
        a.interp = rd16(&data[off]);
        a.anim_flags = rd16(&data[off + 2]);
        a.anim_id = rd32(&data[off + 4]);
        for (int i = 0; i < comps && i < 4; ++i)
            a.init[i] = rd_f32(&data[off + 8 + i * 4]);
        return a;
    };
    auto is_printable = [](const std::string& s) {
        if (s.empty()) return false;
        for (unsigned char ch : s)
            if (ch < 32 || ch > 126) return false;
        return true;
    };

    uint32_t bone_idx = find_tag("BONE");
    if (bone_idx != UINT32_MAX) {
        const M3Section* bs = section_by_index(bone_idx);
        for (uint32_t i = 0; i < bs->count; ++i) {
            size_t r = bs->offset + size_t(i) * 160;
            if (r + 160 > data.size()) break;
            M3Bone b;
            b.name = read_char(read_ref(r + 4));
            b.flags = rd32(&data[r + 16]);
            b.parent = int16_t(rd16(&data[r + 20]));
            b.location = read_anim_ref(r + 24, 3);
            b.rotation = read_anim_ref(r + 60, 4);
            b.scale = read_anim_ref(r + 104, 3);
            b.visibility = read_anim_ref(r + 140, 0);
            b.visibility.init[0] = float(rd32(&data[r + 148]));
            m.bones.push_back(std::move(b));
        }
    }

    uint32_t iref_idx = find_tag("IREF");
    if (iref_idx != UINT32_MAX) {
        const M3Section* is = section_by_index(iref_idx);
        const size_t iref_room = is->offset < data.size() ? (data.size() - is->offset) / 64 : 0;
        uint32_t n = is->count > iref_room ? uint32_t(iref_room) : is->count;
        m.inverse_bind.reserve(size_t(n) * 16);
        for (uint32_t i = 0; i < n; ++i)
            for (int k = 0; k < 16; ++k) {
                size_t o = is->offset + (size_t(i) * 16 + k) * 4;
                m.inverse_bind.push_back(o + 4 <= data.size() ? rd_f32(&data[o]) : 0.0f);
            }
    }

    uint32_t seqs_idx = find_tag("SEQS");
    if (seqs_idx != UINT32_MAX) {
        const M3Section* ss = section_by_index(seqs_idx);
        uint32_t rec = ss->version >= 2 ? 92 : 96;
        for (uint32_t i = 0; i < ss->count; ++i) {
            size_t r = ss->offset + size_t(i) * rec;
            if (r + 40 > data.size()) break;
            M3Sequence sq;
            sq.name = read_char(read_ref(r + 8));
            sq.start_ms = rd32(&data[r + 20]);
            sq.end_ms = rd32(&data[r + 24]);
            sq.movement_speed = rd_f32(&data[r + 28]);
            sq.flags = rd32(&data[r + 32]);
            sq.frequency = rd32(&data[r + 36]);
            m.sequences.push_back(std::move(sq));
        }
    }

    uint32_t stg_idx = find_tag("STG_");
    if (stg_idx != UINT32_MAX) {
        const M3Section* gs = section_by_index(stg_idx);
        for (uint32_t i = 0; i < gs->count; ++i) {
            size_t r = gs->offset + size_t(i) * 24;
            if (r + 24 > data.size()) break;
            M3STG g;
            g.stc_indices = read_u32_array(read_ref(r + 12));
            m.stg.push_back(std::move(g));
        }
    }

    static const int kSlotComps[13] = {0, 2, 3, 4, 4, 1, 1, 1, 1, 1, 1, 1, 0};
    uint32_t stc_idx = find_tag("STC_");
    if (stc_idx != UINT32_MAX) {
        const M3Section* cs = section_by_index(stc_idx);
        for (uint32_t i = 0; i < cs->count; ++i) {
            size_t r = cs->offset + size_t(i) * 204;
            if (r + 204 > data.size()) break;
            M3STC c;
            c.name = read_char(read_ref(r));
            c.anim_ids = read_u32_array(read_ref(r + 20));
            c.anim_refs = read_u32_array(read_ref(r + 32));
            for (int slot = 0; slot < 13; ++slot) {
                M3Ref sdref = read_ref(r + 48 + slot * 12);
                const M3Section* sd = section_by_index(sdref.index);
                if (!sd || sdref.count == 0) continue;
                int comps = kSlotComps[slot];
                for (uint32_t e = 0; e < sdref.count; ++e) {
                    size_t b = sd->offset + size_t(e) * 32;
                    if (b + 32 > data.size()) break;
                    M3Track tr;
                    tr.comps = uint8_t(comps);
                    M3Ref kref = read_ref(b);
                    M3Ref vref = read_ref(b + 20);
                    const M3Section* ks = section_by_index(kref.index);
                    if (ks)
                        for (uint32_t j = 0; j < kref.count; ++j) {
                            size_t o = ks->offset + size_t(j) * 4;
                            if (o + 4 <= data.size()) tr.keys.push_back(int32_t(rd32(&data[o])));
                        }
                    const M3Section* vs = section_by_index(vref.index);
                    tr.slot = uint8_t(slot);
                    if (vs && comps > 0) {
                        const std::string vt = m3_tag(vs->tag);
                        const auto rd = [&](size_t o, size_t n) { return o + n <= data.size(); };
                        for (uint32_t j = 0; j < vref.count; ++j) {
                            if (vt.rfind("COL", 0) == 0) {
                                const size_t o = vs->offset + size_t(j) * 4;
                                if (!rd(o, 4)) break;
                                tr.values.push_back(data[o + 2] / 255.0f);
                                tr.values.push_back(data[o + 1] / 255.0f);
                                tr.values.push_back(data[o] / 255.0f);
                                tr.values.push_back(data[o + 3] / 255.0f);
                            } else if (vt == "U8__") {
                                const size_t o = vs->offset + size_t(j);
                                if (!rd(o, 1)) break;
                                tr.values.push_back(float(data[o]));
                            } else if (vt == "I16_" || vt == "U16_") {
                                const size_t o = vs->offset + size_t(j) * 2;
                                if (!rd(o, 2)) break;
                                const uint16_t u = rd16(&data[o]);
                                tr.values.push_back(vt == "I16_" ? float(int16_t(u)) : float(u));
                            } else if (vt == "I32_" || vt == "U32_" || vt == "FLAG") {
                                const size_t o = vs->offset + size_t(j) * 4;
                                if (!rd(o, 4)) break;
                                const uint32_t u = rd32(&data[o]);
                                tr.values.push_back(vt == "I32_" ? float(int32_t(u)) : float(u));
                            } else {
                                for (int k = 0; k < comps; ++k) {
                                    const size_t o = vs->offset + (size_t(j) * size_t(comps) + size_t(k)) * 4;
                                    if (rd(o, 4)) tr.values.push_back(rd_f32(&data[o]));
                                }
                            }
                        }
                    }
                    if (slot == 0 && vs && vref.count > 0) {
                        const uint32_t erec =
                            vs->version >= 2 ? 108 : (vs->version == 1 ? 104 : 100);
                        for (uint32_t e = 0; e < vref.count; ++e) {
                            size_t base = vs->offset + size_t(e) * erec;
                            if (base + erec > data.size()) break;
                            M3Event ev;
                            std::string nm = char_ref(read_ref(base));
                            if (!is_printable(nm)) nm = char_ref(read_ref(base + 4));
                            ev.name = nm;
                            int bi = int16_t(rd16(&data[base + 18]));
                            if (bi < 0 || bi >= int(m.bones.size())) {
                                int bi2 = int16_t(rd16(&data[base + 16]));
                                if (bi2 >= 0 && bi2 < int(m.bones.size())) bi = bi2;
                            }
                            ev.bone = (bi >= 0 && bi < int(m.bones.size())) ? bi : -1;
                            for (int k = 0; k < 16; ++k)
                                ev.matrix[k] = rd_f32(&data[base + 20 + size_t(k) * 4]);
                            ev.time_ms = e < tr.keys.size() ? tr.keys[e] : 0;
                            tr.events.push_back(std::move(ev));
                        }
                    }
                    c.sd[slot].push_back(std::move(tr));
                }
            }
            m.stc.push_back(std::move(c));
        }
    }

    if (m.bones.size() > 0 && m.vertex_count > 0 && m.stride >= 20) {
        M3Ref vref = read_ref(mo + 100);
        const M3Section* vsec = section_by_index(vref.index);
        if (vsec && vsec->offset + size_t(m.vertex_count) * m.stride <= data.size()) {
            const uint8_t* vbuf = &data[vsec->offset];
            m.vertex_bone_weights.resize(size_t(m.vertex_count) * 4);
            m.vertex_bone_indices.resize(size_t(m.vertex_count) * 4);
            for (uint32_t i = 0; i < m.vertex_count; ++i) {
                const uint8_t* p = vbuf + size_t(i) * m.stride;
                for (int k = 0; k < 4; ++k) {
                    m.vertex_bone_weights[i * 4 + k] = p[12 + k];
                    m.vertex_bone_indices[i * 4 + k] = p[16 + k];
                }
            }
        }
        M3Ref bl = read_ref(mo + 124);
        const M3Section* bls = section_by_index(bl.index);
        const size_t bl_room = (bls && bls->offset < data.size()) ? (data.size() - bls->offset) / 2 : 0;
        if (bls)
            for (uint32_t i = 0; i < bl.count && i < bl_room; ++i) {
                size_t o = bls->offset + size_t(i) * 2;
                m.bone_lookup.push_back(o + 2 <= data.size() ? rd16(&data[o]) : 0);
            }
    }

    uint32_t att_idx = find_tag("ATT_");
    if (att_idx != UINT32_MAX) {
        const M3Section* as = section_by_index(att_idx);
        for (uint32_t i = 0; i < as->count; ++i) {
            size_t r = as->offset + size_t(i) * 20;
            if (r + 20 > data.size()) break;
            M3Attachment a;
            a.name = read_char(read_ref(r + 4));
            a.bone = rd32(&data[r + 16]);
            m.attachments.push_back(std::move(a));
        }
        if (!m.attachments.empty()) {
            uint32_t bl_index = read_ref(mo + 124).index;
            for (uint32_t si = 0; si < sections.size(); ++si) {
                if (si == bl_index || m3_tag(sections[si].tag) != "U16_") continue;
                if (sections[si].count != uint32_t(m.attachments.size())) continue;
                const M3Section* us = &sections[si];
                for (uint32_t k = 0; k < us->count; ++k) {
                    size_t o = us->offset + size_t(k) * 2;
                    m.attachment_lookup.push_back(o + 2 <= data.size() ? rd16(&data[o]) : 0);
                }
                break;
            }
        }
    }

    {
        auto read_shape = [&](size_t b, M3HitShape& s) -> bool {
            if (b + 108 > data.size()) return false;
            s.shape = rd32(&data[b]);
            s.bone = int(int16_t(rd16(&data[b + 4])));
            for (int i = 0; i < 16; ++i) s.m[i] = rd_f32(&data[b + 8 + size_t(i) * 4]);
            for (int i = 0; i < 3; ++i) s.size[i] = rd_f32(&data[b + 96 + size_t(i) * 4]);
            return s.shape <= 2 && s.bone >= 0 && size_t(s.bone) < m.bones.size() && std::isfinite(s.size[0]) &&
                   s.size[0] > 0.0f;
        };
        const uint32_t ssgs = find_tag("SSGS");
        size_t fref = 0;
        const size_t modl_span = section_bytes(modl_index).size();
        if (ssgs != UINT32_MAX) {
            for (size_t off = mo + 4; off + 12 <= mo + modl_span; off += 4) {
                const M3Ref r = read_ref(off);
                if (r.index == ssgs && r.count == sections[ssgs].count && r.count > 0) {
                    fref = off;
                    break;
                }
            }
        } else {
            const uint32_t v = modl->version;
            const size_t slot = v == 23 ? 696 : v == 25 ? 720 : v == 29 ? 768 : 0;
            if (slot && slot + 12 <= modl_span) fref = mo + slot;
        }
        if (fref >= mo + 108) {
            M3HitShape t;
            if (read_shape(fref - 108, t)) {
                m.hit_tight = t;
                m.has_tight_hit = true;
            }
            if (ssgs != UINT32_MAX) {
                const M3Section* fs = section_by_index(ssgs);
                for (uint32_t i = 0; fs && i < fs->count; ++i) {
                    M3HitShape s;
                    if (read_shape(size_t(fs->offset) + size_t(i) * 108, s)) m.hit_fuzzy.push_back(s);
                }
            }
        }
    }
}

std::vector<M3Event> Model::events_for_sequence(size_t seq) const {
    std::vector<M3Event> out;
    if (seq >= stg.size()) return out;
    for (uint32_t j : stg[seq].stc_indices) {
        if (j >= stc.size()) continue;
        for (const M3Track& tr : stc[j].sd[0])
            for (const M3Event& ev : tr.events) out.push_back(ev);
    }
    return out;
}

namespace {
uint32_t par_record_size(uint32_t version) {
    switch (version) {
        case 12: return 1316;
        case 17: return 1460;
        case 18: return 1464;
        case 19: return 1464;
        case 21: return 1464;
        case 22: return 1484;
        case 23: return 1492;
        case 24: return 1496;
        default: return 0;
    }
}

struct ParFields {
    uint32_t mat_ref, speed_init, lifetime_init, size_start, size_end, rate_init;
    uint32_t rate_ref, max_particles, lifetime_ref;
    uint32_t add_flags;
    uint32_t color_init[3], size_mid, size_mid_at, color_mid_at, alpha_mid_at;
    uint32_t angle_x, angle_y, spread_x, spread_y, z_accel, slowdown, mass, shape, area_size, area_radius;
    uint32_t rot_init, rot_mid_at, trailing, tail2;
    uint32_t flip, flags;
    uint32_t hold, mass2, force, noise, cutout_size, cutout_radius;
    uint32_t rot_flags, smoothing, wind;
};
ParFields par_fields(uint32_t v) {
    uint32_t o = 0;
    auto add = [&](uint32_t sz) { uint32_t at = o; o += sz; return at; };
    ParFields f{};
    add(4);
    f.mat_ref = add(4);
    f.add_flags = 0xFFFFFFFFu;
    if (v >= 17) f.add_flags = add(4);
    uint32_t speed = add(20);
    add(20);
    if (v < 17) add(4);
    f.angle_x = add(20) + 8;
    f.angle_y = add(20) + 8;
    f.spread_x = add(20) + 8;
    f.spread_y = add(20) + 8;
    uint32_t life = add(20);
    add(20);
    if (v < 17) add(4);
    add(4);
    add(8);
    f.z_accel = add(4);
    f.size_mid_at = add(4);
    f.color_mid_at = add(4);
    f.alpha_mid_at = add(4);
    f.rot_mid_at = add(4);
    f.hold = 0;
    if (v >= 17) f.hold = add(16);
    uint32_t sizes = add(36);
    f.rot_init = add(36) + 8;
    for (int k = 0; k < 3; ++k)
        f.color_init[k] = add(20) + 8;
    f.slowdown = add(4);
    if (v < 17) add(4);
    f.mass = add(4);
    f.mass2 = add(4); add(4);
    f.trailing = 0;
    if (v < 17) f.trailing = add(4);
    f.force = add(8);
    if (v >= 24) add(4);
    f.noise = add(4); add(4); add(4); add(4);
    add(4);
    uint32_t maxp = add(4);
    uint32_t rate = add(20);
    f.shape = add(4);
    f.area_size = add(36) + 8;
    f.cutout_size = add(36) + 8;
    f.area_radius = add(20) + 8;
    f.cutout_radius = add(20) + 8;
    f.tail2 = o + (v >= 17 ? 12u : 0u);
    f.flip = o + 176 + (v >= 17 ? 12u : 0u);
    f.flags = v == 12 ? f.flip + 480
              : (v == 17 || v == 18 || v == 19 || v == 21) ? f.flip + 484
              : (v == 22 || v == 23 || v == 24) ? f.flip + 504 : 0u;
    f.rot_flags = f.flags && v >= 18 ? f.flags + 4 : 0u;
    f.smoothing = f.flags && v >= 17 ? f.flags + (v >= 18 ? 8u : 4u) : 0u;
    f.wind = f.flags && v >= 17 ? f.flags + (v >= 18 ? 144u : 140u) : 0u;
    f.speed_init = speed + 8;
    f.lifetime_init = life + 8;
    f.size_start = sizes + 8;
    f.size_mid = sizes + 12;
    f.size_end = sizes + 16;
    f.rate_init = rate + 8;
    f.rate_ref = rate;
    f.lifetime_ref = life;
    f.max_particles = maxp;
    return f;
}
}

int particle_cap(const formats::M3Particle& p) {
    if (p.max_particles > 0) return int(std::min<uint32_t>(p.max_particles, 2048u));
    return 64;
}

int particle_count_at_rate(const formats::M3Particle& p, float rate) {
    if (rate > 0.0f) {
        long r = std::lround(double(rate) * double(p.lifetime));
        if (r < 1) r = 1;
        if (r > particle_cap(p)) r = particle_cap(p);
        return int(r);
    }
    return 0;
}

int particle_count(const formats::M3Particle& p) {
    if (p.emission_rate > 0.0f) {
        long r = std::lround(double(p.emission_rate) * double(p.lifetime));
        if (r < 1) r = 1;
        if (r > particle_cap(p)) r = particle_cap(p);
        return int(r);
    }
    return 0;
}

namespace {
std::string flag_names(uint32_t flags, const char* const* names, int count) {
    std::string out;
    for (int b = 0; b < 32; ++b) {
        if (!((flags >> b) & 1u)) continue;
        if (!out.empty()) out += ' ';
        if (b < count) {
            out += names[b];
        } else {
            char buf[16];
            std::snprintf(buf, sizeof buf, "0x%x", 1u << b);
            out += buf;
        }
    }
    return out;
}
}

std::string particle_flag_names(uint32_t flags) {
    static const char* const k[] = {
        "sort", "collideTerrain", "collideObjects", "spawnOnBounce", "cutoutEmissionArea", "inheritEmissionParams",
        "inheritParentVel", "sortByZHeight", "reverseIteration", "smoothRotation", "bezSmoothRotation", "smoothSize",
        "bezSmoothSize", "smoothColor", "bezSmoothColor", "litParts", "randFlipBookStart", "multiplyByGravity",
        "clampTailParts", "spawnTrailingParts", "fixLengthTailParts", "useVertexAlpha", "modelParts",
        "swapYZonModelParts", "scaleTimeByParent", "useLocalTime", "simulateOnInit", "copy"};
    return flag_names(flags, k, int(sizeof k / sizeof k[0]));
}

std::string light_flag_names(uint32_t flags) {
    static const char* const k[] = {"shadowCast", "specular", "affectedByAO", "lightOpaqueObjects",
                                    "lightTransparentObjects", "useTeamColor"};
    return flag_names(flags, k, int(sizeof k / sizeof k[0]));
}

std::vector<M3Light> M3::decode_lights() const {
    std::vector<M3Light> out;
    for (uint32_t si = 0; si < sections.size(); ++si) {
        if (m3_tag(sections[si].tag) != "LITE") continue;
        const M3Section& s = sections[si];
        if (s.count == 0) continue;
        const size_t span = section_bytes(si).size();
        if (span < s.count) continue;
        uint32_t rec = s.version == 7 ? 212u : 0u;
        if (rec == 0 || size_t(s.count) * rec > span || span - size_t(s.count) * rec >= 16)
            rec = uint32_t((span + s.count / 2) / s.count);
        if (rec < 164) continue;
        auto rf = [&](size_t base, uint32_t off) -> float {
            if (off + 4 > rec || base + off + 4 > data.size()) return 0.0f;
            return rd_f32(&data[base + off]);
        };
        for (uint32_t i = 0; i < s.count; ++i) {
            const size_t base = size_t(s.offset) + size_t(i) * rec;
            if (base + rec > data.size()) break;
            M3Light l;
            l.version = s.version;
            l.type = data[base];
            l.bone = int16_t(uint16_t(data[base + 2]) | (uint16_t(data[base + 3]) << 8));
            l.flags = rd32(&data[base + 4]);
            l.color[0] = rf(base, 24);
            l.color[1] = rf(base, 28);
            l.color[2] = rf(base, 32);
            l.intensity = rf(base, 60);
            if (rec >= 64 && base + 64 <= data.size()) {
                l.color_interp = rd16(&data[base + 16]);
                l.color_flags = rd16(&data[base + 18]);
                l.color_id = rd32(&data[base + 20]);
                l.intensity_interp = rd16(&data[base + 52]);
                l.intensity_flags = rd16(&data[base + 54]);
                l.intensity_id = rd32(&data[base + 56]);
            }
            l.attenuation_far = rf(base, 136);
            l.attenuation_near = rf(base, 160);
            l.hotspot = rf(base, 180);
            l.falloff = rf(base, 200);
            if (s.version == 7 && rec == 212) {
                auto hdr = [&](uint32_t off, uint16_t& interp, uint16_t& flags, uint32_t& id) {
                    interp = rd16(&data[base + off]);
                    flags = rd16(&data[base + off + 2]);
                    id = rd32(&data[base + off + 4]);
                };
                l.lod_cut = rd32(&data[base + 8]);
                l.shadow_lod_cut = int32_t(rd32(&data[base + 12]));
                hdr(72, l.spec_color_interp, l.spec_color_flags, l.spec_color_id);
                for (uint32_t k = 0; k < 3; ++k) l.spec_color[k] = rf(base, 80 + 4 * k);
                hdr(108, l.spec_intensity_interp, l.spec_intensity_flags, l.spec_intensity_id);
                l.spec_intensity = rf(base, 116);
                hdr(128, l.far_interp, l.far_flags, l.far_id);
                l.unknown148 = rf(base, 148);
                hdr(152, l.near_interp, l.near_flags, l.near_id);
                hdr(172, l.hotspot_interp, l.hotspot_flags, l.hotspot_id);
                hdr(192, l.falloff_interp, l.falloff_flags, l.falloff_id);
                l.has_ext = true;
            }
            out.push_back(l);
        }
    }
    return out;
}

std::vector<M3Projector> M3::decode_projectors() const {
    std::vector<M3Projector> out;
    const uint32_t matm = first_section_with_tag("MATM");
    const uint32_t stbm = first_section_with_tag("STBM");
    for (uint32_t si = 0; si < sections.size(); ++si) {
        if (m3_tag(sections[si].tag) != "PROJ") continue;
        const M3Section& s = sections[si];
        if (s.count == 0 || (s.version != 4 && s.version != 5)) continue;
        const size_t span = section_bytes(si).size();
        const size_t rec = 388;
        if (size_t(s.count) * rec > span) continue;
        for (uint32_t i = 0; i < s.count; ++i) {
            const size_t base = size_t(s.offset) + size_t(i) * rec;
            if (base + rec > data.size()) break;
            M3Projector p;
            p.version = s.version;
            p.type = rd32(&data[base]);
            p.bone = rd32(&data[base + 4]);
            p.material_ref_index = rd32(&data[base + 8]);
            for (int a = 0; a < 3; ++a) p.offset[a] = rd_f32(&data[base + 20 + size_t(a) * 4]);
            p.z_bottom = rd_f32(&data[base + 196]);
            p.z_top = rd_f32(&data[base + 216]);
            p.x_left = rd_f32(&data[base + 236]);
            p.x_right = rd_f32(&data[base + 256]);
            p.y_front = rd_f32(&data[base + 276]);
            p.y_back = rd_f32(&data[base + 296]);
            p.layer = rd32(&data[base + 372]);
            if (const M3Section* ms = matm == UINT32_MAX ? nullptr : section_by_index(matm);
                ms && p.material_ref_index < ms->count && ms->offset + size_t(p.material_ref_index) * 8 + 8 <= data.size()) {
                p.material_type = rd32(&data[ms->offset + size_t(p.material_ref_index) * 8]);
                p.material_index = rd32(&data[ms->offset + size_t(p.material_ref_index) * 8 + 4]);
            }
            if (const M3Section* ss = stbm == UINT32_MAX ? nullptr : section_by_index(stbm);
                p.material_type == 9 && ss && p.material_index < ss->count) {
                const size_t sb = size_t(ss->offset) + size_t(p.material_index) * 48;
                std::string* dst[3] = {&p.diffuse, &p.normal, &p.specular};
                for (int k = 0; k < 3; ++k) {
                    if (sb + 12 + size_t(k) * 12 + 12 > data.size()) break;
                    const M3Ref lr = read_ref(sb + 12 + size_t(k) * 12);
                    if (lr.count == 0 || lr.index >= sections.size()) continue;
                    const M3Section* ls = section_by_index(lr.index);
                    if (!ls || m3_tag(ls->tag) != "LAYR" || ls->offset + 16 > data.size()) continue;
                    *dst[k] = char_ref(read_ref(ls->offset + 4));
                }
            }
            out.push_back(std::move(p));
        }
    }
    return out;
}

std::vector<M3Ribbon> M3::decode_ribbons() const {
    std::vector<M3Ribbon> out;
    for (uint32_t si = 0; si < sections.size(); ++si) {
        if (m3_tag(sections[si].tag) != "RIB_") continue;
        const M3Section& s = sections[si];
        if (s.count == 0) continue;
        const size_t span = section_bytes(si).size();
        struct Lay { uint32_t rec, speed, width, col[3], div, sides, srib; };
        Lay L{};
        bool known = true;
        if (s.version == 6) L = {748, 8, 208, {280, 300, 320}, 400, 404, 436};
        else if (s.version == 9) L = {0, 12, 220, {292, 312, 332}, 408, 412, 440};
        else if (s.version == 8) L = {756, 12, 220, {292, 312, 332}, 404, 408, 436};
        else known = false;
        uint32_t rec = L.rec;
        if (rec == 0 || size_t(rec) * s.count > span) {
            if (span < s.count) continue;
            rec = uint32_t(span / s.count) & ~3u;
        }
        if (rec < 8) continue;
        for (uint32_t i = 0; i < s.count; ++i) {
            const size_t base = size_t(s.offset) + size_t(i) * rec;
            if (base + rec > data.size()) break;
            M3Ribbon r;
            r.version = s.version;
            r.bone = rd32(&data[base]);
            r.material_ref_index = rd32(&data[base + 4]);
            r.raw.assign(data.begin() + long(base), data.begin() + long(base + rec));
            if (known && L.srib + 12 <= rec) {
                auto fin = [](float v) { return std::isfinite(v) ? v : 0.0f; };
                r.speed = fin(rd_f32(&data[base + L.speed + 8]));
                r.width_interp = rd16(&data[base + L.width]);
                r.width_flags = rd16(&data[base + L.width + 2]);
                r.width_id = rd32(&data[base + L.width + 4]);
                for (int k = 0; k < 3; ++k) r.width[k] = std::max(0.0f, fin(rd_f32(&data[base + L.width + 8 + 4 * k])));
                for (int k = 0; k < 3; ++k) {
                    r.color_interp[k] = rd16(&data[base + L.col[k]]);
                    r.color_flags[k] = rd16(&data[base + L.col[k] + 2]);
                    r.color_id[k] = rd32(&data[base + L.col[k] + 4]);
                    const uint8_t* c = &data[base + L.col[k] + 8];
                    r.color[k][0] = float(c[2]) / 255.0f;
                    r.color[k][1] = float(c[1]) / 255.0f;
                    r.color[k][2] = float(c[0]) / 255.0f;
                    r.color[k][3] = float(c[3]) / 255.0f;
                }
                r.divisions = fin(rd_f32(&data[base + L.div]));
                r.sides = rd32(&data[base + L.sides]);
                const M3Ref sref = read_ref(base + L.srib);
                if (sref.count >= 1 && sref.index < sections.size() && m3_tag(sections[sref.index].tag) == "SRIB") {
                    const size_t sb = sections[sref.index].offset;
                    if (sb + 52 <= data.size()) r.end_bone = int32_t(rd32(&data[sb + 48]));
                    if (sb + 272 <= data.size()) r.srib_raw.assign(data.begin() + long(sb), data.begin() + long(sb + 272));
                }
                r.has_fields = true;
            }
            out.push_back(std::move(r));
        }
    }
    return out;
}

std::vector<M3Particle> M3::decode_particles() const {
    std::vector<M3Particle> out;
    for (uint32_t si = 0; si < sections.size(); ++si) {
        if (m3_tag(sections[si].tag) != "PAR_") continue;
        const M3Section& s = sections[si];
        if (s.count == 0) continue;

        uint32_t rec = par_record_size(s.version);
        if (rec == 0) {
            size_t span = section_bytes(si).size();
            if (span < s.count) continue;
            rec = uint32_t((span + s.count / 2) / s.count);
        }
        if (rec < 8) continue;

        ParFields f = par_fields(s.version);
        auto rf = [&](size_t base, uint32_t off) -> float {
            if (off + 4 > rec || base + off + 4 > data.size()) return 0.0f;
            return rd_f32(&data[base + off]);
        };
        for (uint32_t i = 0; i < s.count; ++i) {
            size_t base = size_t(s.offset) + size_t(i) * rec;
            if (base + 8 > data.size()) break;
            M3Particle p;
            p.version = s.version;
            p.bone = rd32(&data[base]);
            p.material_ref_index =
                base + f.mat_ref + 4 <= data.size() ? rd32(&data[base + f.mat_ref]) : 0;
            p.emission_speed = rf(base, f.speed_init);
            p.lifetime = rf(base, f.lifetime_init);
            p.emission_speed2 = rf(base, f.speed_init + 20);
            p.lifetime2 = rf(base, f.lifetime_init + 20);
            if (s.version < 17) {
                if (f.speed_init + 36 <= rec && base + f.speed_init + 36 <= data.size()) p.rand_speed2 = rd32(&data[base + f.speed_init + 32]) != 0;
                if (f.lifetime_init + 36 <= rec && base + f.lifetime_init + 36 <= data.size()) p.rand_life2 = rd32(&data[base + f.lifetime_init + 32]) != 0;
            }
            p.emission_rate = rf(base, f.rate_init);
            if (std::getenv("OMC_PAR_OFFSETS"))
                std::fprintf(stderr, "[paroff] v%u base %zu rate %zu life %zu size %zu %zu %zu color0 %zu color1 %zu color2 %zu maxp %zu\n", unsigned(s.version), std::size_t(base),
                             std::size_t(base + f.rate_init), std::size_t(base + f.lifetime_init), std::size_t(base + f.size_start), std::size_t(base + f.size_mid), std::size_t(base + f.size_end),
                             std::size_t(base + f.color_init[0]), std::size_t(base + f.color_init[1]), std::size_t(base + f.color_init[2]), std::size_t(base + f.max_particles));
            if (f.rate_ref + 8 <= rec && base + f.rate_ref + 8 <= data.size()) {
                p.rate_interp = rd16(&data[base + f.rate_ref]);
                p.rate_anim_flags = rd16(&data[base + f.rate_ref + 2]);
                p.rate_anim_id = rd32(&data[base + f.rate_ref + 4]);
                p.life_anim_flags = rd16(&data[base + f.lifetime_ref + 2]);
                p.life_anim_id = rd32(&data[base + f.lifetime_ref + 4]);
            }
            if (f.max_particles + 4 <= rec && base + f.max_particles + 4 <= data.size())
                p.max_particles = rd32(&data[base + f.max_particles]);
            if (f.add_flags != 0xFFFFFFFFu && f.add_flags + 4 <= rec &&
                base + f.add_flags + 4 <= data.size())
                p.additional_flags = rd32(&data[base + f.add_flags]);
            if (s.version >= 17) {
                p.rand_speed2 = (p.additional_flags & 0x1u) != 0;
                p.rand_life2 = (p.additional_flags & 0x2u) != 0;
            }
            if (base + rec <= data.size())
                p.raw.assign(data.begin() + long(base), data.begin() + long(base + rec));
            static const bool kBurst = std::getenv("OMC_NO_PAR_BURST") == nullptr;
            if (kBurst && s.version == 21 && rec >= 712 && base + 712 <= data.size()) {
                p.burst_interp = rd16(&data[base + 700]);
                p.burst_flags = rd16(&data[base + 702]);
                p.burst_id = rd32(&data[base + 704]);
            }
            p.size_start = rf(base, f.size_start);
            p.size_end = rf(base, f.size_end);
            if (f.color_init[2] + 4 <= rec && base + f.color_init[2] + 4 <= data.size()) {
                for (int k = 0; k < 3; ++k) {
                    const uint8_t* c = &data[base + f.color_init[k]];
                    p.color[k][0] = float(c[2]) / 255.0f;
                    p.color[k][1] = float(c[1]) / 255.0f;
                    p.color[k][2] = float(c[0]) / 255.0f;
                    p.color[k][3] = float(c[3]) / 255.0f;
                }
                p.size_mid = rf(base, f.size_mid);
                auto frac = [&](uint32_t at) {
                    float v = rf(base, at);
                    return (v > 0.0f && v < 1.0f) ? v : 0.5f;
                };
                p.size_mid_at = frac(f.size_mid_at);
                p.color_mid_at = frac(f.color_mid_at);
                p.alpha_mid_at = frac(f.alpha_mid_at);
                p.has_color = true;
            }
            if (f.area_radius + 4 <= rec && base + f.area_radius + 4 <= data.size()) {
                auto finite = [](float v) { return std::isfinite(v) ? v : 0.0f; };
                p.angle[0] = finite(rf(base, f.angle_x));
                p.angle[1] = finite(rf(base, f.angle_y));
                p.spread[0] = finite(rf(base, f.spread_x));
                p.spread[1] = finite(rf(base, f.spread_y));
                p.z_accel = finite(rf(base, f.z_accel));
                p.slowdown = std::max(0.0f, finite(rf(base, f.slowdown)));
                p.mass = finite(rf(base, f.mass));
                p.shape = rd32(&data[base + f.shape]);
                for (int k = 0; k < 3; ++k) p.area_size[k] = std::max(0.0f, finite(rf(base, f.area_size + 4 * k)));
                p.area_radius = std::max(0.0f, finite(rf(base, f.area_radius)));
                p.has_shape = p.shape <= 7;
            }
            if (p.has_color && f.tail2 + 148 <= rec && base + f.tail2 + 148 <= data.size()) {
                auto fin = [](float v) { return std::isfinite(v) ? v : 0.0f; };
                for (int k = 0; k < 3; ++k) p.rot[k] = fin(rf(base, f.rot_init + 4 * uint32_t(k)));
                const float rm = rf(base, f.rot_mid_at);
                p.rot_mid_at = (rm > 0.0f && rm < 1.0f) ? rm : 0.5f;
                const uint32_t t2 = f.tail2;
                p.emission_type = rd32(&data[base + t2]);
                p.rand_size2 = rd32(&data[base + t2 + 4]) != 0;
                for (int k = 0; k < 3; ++k) p.size2[k] = std::max(0.0f, fin(rf(base, t2 + 16 + 4 * uint32_t(k))));
                static const bool kRot2Old = std::getenv("OMC_PAR_ROT2_OLD") != nullptr;
                for (int k = 0; k < 3; ++k) p.rot2[k] = fin(rf(base, t2 + (kRot2Old ? 52u : 56u) + 4 * uint32_t(k)));
                p.rand_rot2 = rd32(&data[base + t2 + (kRot2Old ? 80u : 44u)]) != 0;
                p.rand_color2 = rd32(&data[base + t2 + 84]) != 0;
                for (int k = 0; k < 3; ++k) {
                    const uint8_t* c = &data[base + t2 + 88 + 20 * size_t(k) + 8];
                    p.color2[k][0] = float(c[2]) / 255.0f;
                    p.color2[k][1] = float(c[1]) / 255.0f;
                    p.color2[k][2] = float(c[0]) / 255.0f;
                    p.color2[k][3] = float(c[3]) / 255.0f;
                }
                p.has_tail2 = p.emission_type <= 8;
                if (!p.has_tail2) { p.emission_type = 0; p.rand_size2 = p.rand_rot2 = p.rand_color2 = false; }
                if (f.trailing && f.trailing + 4 <= rec) p.trailing = rd32(&data[base + f.trailing]) != 0;
            }
            if (f.flip >= 8 && f.flip + 12 <= rec && base + f.flip + 12 <= data.size()) {
                const uint16_t cols = rd16(&data[base + f.flip]), rows = rd16(&data[base + f.flip + 2]);
                const float cw = rd_f32(&data[base + f.flip + 4]), rh = rd_f32(&data[base + f.flip + 8]);
                if (cols >= 1 && cols <= 64 && rows >= 1 && rows <= 64 && std::fabs(cw - 1.0f / float(cols)) < 1e-5f &&
                    std::fabs(rh - 1.0f / float(rows)) < 1e-5f) {
                    p.flip_cols = cols;
                    p.flip_rows = rows;
                    for (int k = 0; k < 3; ++k) p.flip_phase[k] = data[base + f.flip - 8 + size_t(k)];
                    const float len = rd_f32(&data[base + f.flip - 4]);
                    p.flip_phase1_len = (std::isfinite(len) && len > 0.0f && len <= 1.0f) ? len : 1.0f;
                    if (f.flip + 52 <= rec && base + f.flip + 52 <= data.size()) {
                        const uint32_t ty = rd32(&data[base + f.flip + 44]);
                        const float lw = rd_f32(&data[base + f.flip + 48]);
                        if (ty <= 12) p.ptype = ty;
                        if (std::isfinite(lw) && lw > 0.0f && lw < 1000.0f) p.length_width = lw;
                    }
                }
            }
            if (f.flags && f.flags + 4 <= rec && base + f.flags + 4 <= data.size()) {
                p.flags = rd32(&data[base + f.flags]);
                p.has_flags = true;
                if (s.version == 12 || s.version == 21) p.flip_random_start = (p.flags & 0x10000u) != 0;
            }
            {
                auto fin = [](float v) { return std::isfinite(v) ? v : 0.0f; };
                auto in = [&](uint32_t off, uint32_t n) { return off && off + n <= rec && base + off + n <= data.size(); };
                if (in(f.wind, 12) && in(f.smoothing, 12)) {
                    if (in(f.rot_flags, 4)) p.rotation_flags = rd32(&data[base + f.rot_flags]);
                    for (uint32_t k = 0; k < 3; ++k) p.smoothing[k] = rd32(&data[base + f.smoothing + 4 * k]);
                    p.wind_multiplier = fin(rd_f32(&data[base + f.wind]));
                    p.lod_reduce = rd32(&data[base + f.wind + 4]);
                    p.lod_cut = rd32(&data[base + f.wind + 8]);
                    p.has_ext = true;
                }
                if (in(f.mass2, 4)) p.mass2 = fin(rd_f32(&data[base + f.mass2]));
                if (in(f.hold, 16))
                    for (uint32_t k = 0; k < 4; ++k) p.hold_time[k] = fin(rd_f32(&data[base + f.hold + 4 * k]));
                if (in(f.force, 8))
                    for (uint32_t k = 0; k < 4; ++k) p.force_channels[k] = rd16(&data[base + f.force + 2 * k]);
                if (in(f.noise, 16))
                    for (uint32_t k = 0; k < 4; ++k) p.noise[k] = fin(rd_f32(&data[base + f.noise + 4 * k]));
                if (in(f.cutout_size, 12))
                    for (uint32_t k = 0; k < 3; ++k) p.cutout_size[k] = fin(rd_f32(&data[base + f.cutout_size + 4 * k]));
                if (in(f.cutout_radius, 4)) p.cutout_radius = fin(rd_f32(&data[base + f.cutout_radius]));
                if (s.version >= 17) {
                    p.rand_mass2 = (p.additional_flags & 0x4u) != 0;
                    p.trailing_flag = (p.additional_flags & 0x8u) != 0;
                } else {
                    p.trailing_flag = p.trailing;
                }
            }
            out.push_back(p);
        }
    }
    return out;
}

uint32_t M3::bone_count() const {
    const M3Section* modl = section_by_index(modl_index);
    if (!modl) return 0;
    return read_ref(modl->offset + 80).count;
}

std::string M3::model_name() const {
    const M3Section* modl = section_by_index(modl_index);
    if (!modl) return {};
    M3Ref name = read_ref(modl->offset);
    const M3Section* cs = section_by_index(name.index);
    if (!cs) return {};
    size_t n = name.count;
    if (cs->offset + n > data.size()) return {};
    std::string s(reinterpret_cast<const char*>(data.data() + cs->offset), n);
    if (!s.empty() && s.back() == '\0') s.pop_back();
    return s;
}

const M3Section* M3::section_by_tag(const std::string& tag4) const {
    for (const M3Section& s : sections)
        if (m3_tag(s.tag) == tag4) return &s;
    return nullptr;
}

}
