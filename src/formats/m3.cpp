#include "sc2/formats/m3.hpp"

#include <cmath>
#include <stdexcept>

#include "sc2/core/byte_reader.hpp"

namespace sc2::formats {

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
    if (!cs || r.count == 0 || cs->offset + r.count > data.size()) return {};
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

    const M3Section* vsec = section_by_index(vertices.index);
    if (!vsec) throw std::runtime_error("m3: bad vertices ref");
    uint32_t vbytes = vertices.count;
    if (vsec->offset + size_t(vbytes) > data.size())
        throw std::runtime_error("m3: vertices out of range");
    const uint8_t* vbuf = &data[vsec->offset];

    const M3Section* dsec = section_by_index(divisions.index);
    if (!dsec || divisions.count < 1) return m;
    M3Ref faces = read_ref(dsec->offset);
    M3Ref regions = read_ref(dsec->offset + 12);
    const M3Section* rsec = section_by_index(regions.index);
    if (!rsec || regions.count == 0) return m;
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
    bool has_uv = stride >= 28;
    if (has_uv) m.uvs.reserve(size_t(m.vertex_count) * 2);
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
            int16_t u = int16_t(rd16(p + 24));
            int16_t vv = int16_t(rd16(p + 26));
            m.uvs.push_back(float(u) / 2048.0f);
            m.uvs.push_back(float(vv) / 2048.0f);
        }
    }
    for (int a = 0; a < 3; ++a)
        if (!(mn[a] == mn[a]) || !(mx[a] == mx[a]) ||
            mn[a] < -1e6f || mx[a] > 1e6f)
            throw std::runtime_error("m3: implausible vertex positions");

    decode_materials(m);
    decode_animation(m);
    return m;
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
    uint32_t mat_rec = uint32_t((mat_span + mat_count / 2) / mat_count);
    if (mat_rec < 24) return;

    std::vector<const char*> slots;
    for (const LayerSlot& s : kLayerSlots)
        if (s.min_ver == 0 || int(mats->version) > s.min_ver) slots.push_back(s.usage);
    const size_t L = slots.size();

    auto is_layer_like = [&](const M3Ref& r) {
        if (r.count == 0 && r.index == 0) return true;
        return r.index < sections.size() && m3_tag(sections[r.index].tag) == "LAYR";
    };

    for (uint32_t mi = 0; mi < mat_count; ++mi) {
        size_t rec = mats->offset + size_t(mi) * mat_rec;
        if (rec + mat_rec > data.size()) break;
        M3Material mat;
        mat.name = read_char(read_ref(rec));
        size_t block = 0;
        for (size_t off = rec + 12; off + L * 12 <= rec + mat_rec; off += 4) {
            bool all = true, any_real = false;
            for (size_t k = 0; k < L; ++k) {
                M3Ref r = read_ref(off + k * 12);
                if (!is_layer_like(r)) { all = false; break; }
                if (r.count != 0 && layr_idx != UINT32_MAX) any_real = true;
            }
            if (all && any_real) { block = off; break; }
        }
        if (block) {
            for (size_t k = 0; k < L; ++k) {
                M3Ref lr = read_ref(block + k * 12);
                if (lr.count == 0 || lr.index >= sections.size()) continue;
                const M3Section* ls = section_by_index(lr.index);
                if (!ls || m3_tag(ls->tag) != "LAYR") continue;
                M3Ref img = read_ref(ls->offset + 4);
                std::string path = read_char(img);
                if (!path.empty())
                    mat.layers.push_back({slots[k], path});
            }
        }
        m.materials.push_back(std::move(mat));
    }

    uint32_t bat_idx = find_tag("BAT_");
    if (bat_idx != UINT32_MAX && !m.regions.empty()) {
        m.region_material.assign(m.regions.size(), -1);
        const M3Section* bs = section_by_index(bat_idx);
        for (uint32_t i = 0; i < bs->count; ++i) {
            size_t r = bs->offset + size_t(i) * 14;
            if (r + 14 > data.size()) break;
            uint32_t region = rd16(&data[r + 4]);
            uint32_t mref = rd16(&data[r + 10]);
            if (region >= m.regions.size() || mref >= m.material_refs.size()) continue;
            const M3MaterialRef& mr = m.material_refs[mref];
            if (mr.type == 1 && mr.index < m.materials.size())
                m.region_material[region] = int(mr.index);
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
            m.bones.push_back(std::move(b));
        }
    }

    uint32_t iref_idx = find_tag("IREF");
    if (iref_idx != UINT32_MAX) {
        const M3Section* is = section_by_index(iref_idx);
        uint32_t n = is->count;
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
                    if (vs && comps > 0)
                        for (uint32_t j = 0; j < vref.count * uint32_t(comps); ++j) {
                            size_t o = vs->offset + size_t(j) * 4;
                            if (o + 4 <= data.size()) tr.values.push_back(rd_f32(&data[o]));
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
        if (bls)
            for (uint32_t i = 0; i < bl.count; ++i) {
                size_t o = bls->offset + size_t(i) * 2;
                m.bone_lookup.push_back(o + 2 <= data.size() ? rd16(&data[o]) : 0);
            }
    }
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
};
ParFields par_fields(uint32_t v) {
    uint32_t o = 0;
    auto add = [&](uint32_t sz) { uint32_t at = o; o += sz; return at; };
    ParFields f{};
    add(4);
    f.mat_ref = add(4);
    if (v >= 17) add(4);
    uint32_t speed = add(20);
    add(20);
    if (v < 17) add(4);
    add(20); add(20); add(20); add(20);
    uint32_t life = add(20);
    add(20);
    if (v < 17) add(4);
    add(4);
    add(8);
    add(4); add(4); add(4); add(4); add(4);
    if (v >= 17) add(16);
    uint32_t sizes = add(36);
    add(36);
    add(20); add(20); add(20);
    add(4); add(4); add(4); add(4);
    if (v < 17) { add(4); add(4); }
    add(8);
    add(4); add(4); add(4); add(4);
    if (v >= 24) add(4);
    add(4); add(4);
    uint32_t rate = add(20);
    f.speed_init = speed + 8;
    f.lifetime_init = life + 8;
    f.size_start = sizes + 8;
    f.size_end = sizes + 16;
    f.rate_init = rate + 8;
    return f;
}
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
            p.emission_rate = rf(base, f.rate_init);
            p.size_start = rf(base, f.size_start);
            p.size_end = rf(base, f.size_end);
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
