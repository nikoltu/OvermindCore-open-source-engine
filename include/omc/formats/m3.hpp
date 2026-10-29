#pragma once

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace omc::formats {

struct M3Section {
    uint32_t tag;
    uint32_t offset;
    uint32_t count;
    uint32_t version;
};

struct M3Ref {
    uint32_t count = 0;
    uint32_t index = 0;
    uint32_t flags = 0;
    bool empty() const { return count == 0; }
};

struct M3Light {
    uint32_t type = 0;
    int32_t bone = -1;
    uint32_t flags = 0;
    float color[3] = {0, 0, 0};
    float intensity = 0;
    uint16_t intensity_interp = 0, intensity_flags = 0;
    uint32_t intensity_id = 0;
    uint16_t color_interp = 0, color_flags = 0;
    uint32_t color_id = 0;
    float attenuation_far = 0;
    float attenuation_near = 0;
    float hotspot = 0;
    float falloff = 0;
    uint32_t version = 0;
    uint32_t lod_cut = 0;
    int32_t shadow_lod_cut = 0;
    float spec_color[3] = {1, 1, 1};
    uint16_t spec_color_interp = 0, spec_color_flags = 0;
    uint32_t spec_color_id = 0;
    float spec_intensity = 1;
    uint16_t spec_intensity_interp = 0, spec_intensity_flags = 0;
    uint32_t spec_intensity_id = 0;
    float unknown148 = 0;
    uint16_t far_interp = 0, far_flags = 0, near_interp = 0, near_flags = 0;
    uint16_t hotspot_interp = 0, hotspot_flags = 0, falloff_interp = 0, falloff_flags = 0;
    uint32_t far_id = 0, near_id = 0, hotspot_id = 0, falloff_id = 0;
    bool has_ext = false;
};

enum M3LightType : uint32_t { kLightPoint = 1, kLightSpot = 2 };

enum M3LightFlag : uint32_t {
    kLightShadowCast = 0x1,
    kLightSpecular = 0x2,
    kLightAffectedByAO = 0x4,
    kLightOpaqueObjects = 0x8,
    kLightTransparentObjects = 0x10,
    kLightTeamColor = 0x20,
};

struct M3Projector {
    uint32_t type = 0;
    uint32_t bone = 0;
    uint32_t material_ref_index = 0;
    uint32_t material_type = 0;
    uint32_t material_index = 0;
    float offset[3] = {0, 0, 0};
    float z_bottom = 0, z_top = 0, x_left = 0, x_right = 0, y_front = 0, y_back = 0;
    uint32_t layer = 0;
    std::string diffuse, normal, specular;
    uint32_t version = 0;
};

struct M3Ribbon {
    uint32_t bone = 0;
    uint32_t material_ref_index = 0;
    int32_t end_bone = -1;
    float speed = 0.0f;
    float width[3] = {0, 0, 0};
    uint16_t width_interp = 0, width_flags = 0;
    uint32_t width_id = 0;
    float color[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    uint16_t color_interp[3] = {0, 0, 0}, color_flags[3] = {0, 0, 0};
    uint32_t color_id[3] = {0, 0, 0};
    float divisions = 0.0f;
    uint32_t sides = 0;
    bool has_fields = false;
    uint32_t version = 0;
    std::vector<uint8_t> raw;
    std::vector<uint8_t> srib_raw;
};

struct M3Particle {
    uint32_t bone = 0;
    uint32_t material_ref_index = 0;
    float emission_speed = 0;
    float lifetime = 0;
    float emission_speed2 = 0;
    float lifetime2 = 0;
    bool rand_speed2 = false;
    bool rand_life2 = false;
    float emission_rate = 0;
    float size_start = 0;
    float size_end = 0;
    float size_mid = 0;
    float color[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    float size_mid_at = 0.5f, color_mid_at = 0.5f, alpha_mid_at = 0.5f;
    bool has_color = false;
    float angle[2] = {0, 0};
    float spread[2] = {0, 0};
    float z_accel = 0;
    float slowdown = 0;
    float mass = 0;
    uint32_t shape = 0;
    float area_size[3] = {0, 0, 0};
    float area_radius = 0;
    bool has_shape = false;
    float rot[3] = {0, 0, 0};
    float rot_mid_at = 0.5f;
    uint32_t emission_type = 0;
    bool rand_size2 = false, rand_rot2 = false, rand_color2 = false;
    float size2[3] = {0, 0, 0};
    float rot2[3] = {0, 0, 0};
    float color2[3][4] = {{1, 1, 1, 1}, {1, 1, 1, 1}, {1, 1, 1, 1}};
    bool has_tail2 = false;
    bool trailing = true;
    uint32_t ptype = 0;
    float length_width = 1.0f;
    uint16_t flip_cols = 1, flip_rows = 1;
    uint8_t flip_phase[3] = {0, 0, 0};
    float flip_phase1_len = 1.0f;
    bool flip_random_start = false;
    uint32_t flags = 0;
    bool has_flags = false;
    uint32_t version = 0;
    std::vector<uint8_t> raw;

    uint32_t additional_flags = 0;
    uint32_t rate_anim_id = 0;

    uint32_t life_anim_flags = 0;
    uint32_t life_anim_id = 0;
    uint16_t rate_anim_flags = 0;
    uint16_t rate_interp = 0;
    uint32_t max_particles = 0;
    uint16_t burst_interp = 0, burst_flags = 0;
    uint32_t burst_id = 0;

    uint32_t rotation_flags = 0;
    uint32_t smoothing[3] = {0, 0, 0};
    float wind_multiplier = 0;
    uint32_t lod_reduce = 0, lod_cut = 0;
    bool has_ext = false;
    float mass2 = 0;
    bool rand_mass2 = false;
    bool trailing_flag = true;
    float hold_time[4] = {0, 0, 0, 0};
    float noise[4] = {0, 0, 0, 0};
    uint16_t force_channels[4] = {0, 0, 0, 0};
    float cutout_size[3] = {0, 0, 0};
    float cutout_radius = 0;
};

enum M3ParticleFlag : uint32_t {
    kParSort = 0x1,
    kParCollideTerrain = 0x2,
    kParCollideObjects = 0x4,
    kParSpawnOnBounce = 0x8,
    kParCutoutEmissionArea = 0x10,
    kParInheritEmissionParams = 0x20,
    kParInheritParentVel = 0x40,
    kParSortByZHeight = 0x80,
    kParReverseIteration = 0x100,
    kParSmoothRotation = 0x200,
    kParBezSmoothRotation = 0x400,
    kParSmoothSize = 0x800,
    kParBezSmoothSize = 0x1000,
    kParSmoothColor = 0x2000,
    kParBezSmoothColor = 0x4000,
    kParLitParts = 0x8000,
    kParRandFlipBookStart = 0x10000,
    kParMultiplyByGravity = 0x20000,
    kParClampTailParts = 0x40000,
    kParSpawnTrailingParts = 0x80000,
    kParFixLengthTailParts = 0x100000,
    kParUseVertexAlpha = 0x200000,
    kParModelParts = 0x400000,
    kParSwapYZonModelParts = 0x800000,
    kParScaleTimeByParent = 0x1000000,
    kParUseLocalTime = 0x2000000,
    kParSimulateOnInit = 0x4000000,
    kParCopy = 0x8000000,
};
std::string particle_flag_names(uint32_t flags);
std::string light_flag_names(uint32_t flags);

int particle_count(const M3Particle& p);
int particle_cap(const M3Particle& p);

int particle_count_at_rate(const M3Particle& p, float rate);

struct Model;

struct M3 {
    uint32_t index_offset = 0;
    uint32_t index_entries = 0;
    uint32_t modl_index = 0;
    uint32_t modl_version = 0;
    std::vector<M3Section> sections;
    std::vector<uint8_t> data;

    static M3 parse(std::vector<uint8_t> bytes);

    const M3Section* section_by_index(uint32_t i) const {
        return i < sections.size() ? &sections[i] : nullptr;
    }
    const M3Section* section_by_tag(const std::string& tag4) const;

    M3Ref read_ref(size_t offset) const;
    std::vector<uint8_t> section_bytes(uint32_t index) const;

    std::string char_ref(const M3Ref& r) const;
    uint32_t first_section_with_tag(const char* tag4) const;
    std::vector<uint32_t> u32_array(const M3Ref& r) const;

    std::string model_name() const;

    Model decode_model() const;

    void decode_materials(Model& m) const;
    void split_terrain_regions(Model& m) const;

    void decode_animation(Model& m) const;

    std::vector<M3Particle> decode_particles() const;
    std::vector<M3Light> decode_lights() const;
    std::vector<M3Ribbon> decode_ribbons() const;
    std::vector<M3Projector> decode_projectors() const;

    uint32_t bone_count() const;
};

struct M3Region {
    uint32_t first_vertex = 0;
    uint32_t num_vertices = 0;
    uint32_t first_index = 0;
    uint32_t num_indices = 0;
    uint32_t first_bone_lookup = 0;
    float uv_multiply = 16.0f;
    float uv_offset = 0.0f;
};

struct M3AnimRef;

struct M3Layer {
    std::string usage;
    std::string image_path;
    std::uint32_t word36 = 0, word40 = 0, word44 = 0;
    std::vector<std::uint8_t> raw_record;
    std::uint32_t layr_version = 0;
    struct AnimRefs {
        float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
        std::uint16_t bright_mult_interp = 0, bright_mult_flags = 0;
        std::uint32_t bright_mult_id = 0;
        float bright_mult = 1.0f;
        float bright_mult_null = 1.0f;
        std::uint16_t midtone_interp = 0, midtone_flags = 0;
        std::uint32_t midtone_id = 0;
        float midtone_offset = 0.0f;
        std::uint16_t uv_offset_interp = 0, uv_offset_flags = 0;
        std::uint32_t uv_offset_id = 0;
        float uv_offset[2] = {0.0f, 0.0f};
        std::uint16_t uv_angle_interp = 0, uv_angle_flags = 0;
        std::uint32_t uv_angle_id = 0;
        float uv_angle[3] = {0.0f, 0.0f, 0.0f};
        std::uint16_t uv_tiling_interp = 0, uv_tiling_flags = 0;
        std::uint32_t uv_tiling_id = 0;
        float uv_tiling[2] = {1.0f, 1.0f};
        std::uint16_t brightness_interp = 0, brightness_flags = 0;
        std::uint32_t brightness_id = 0;
        float brightness = 1.0f;
        float brightness_null = 1.0f;
        std::uint32_t flipbook_rows = 0, flipbook_cols = 0;
        std::uint16_t flipbook_frame_interp = 0, flipbook_frame_flags = 0;
        std::uint32_t flipbook_frame_id = 0;
        float flipbook_frame = 0.0f;
    } anim;
    std::vector<uint8_t> inline_rgba{};
    uint32_t inline_w = 0, inline_h = 0;
    bool const_colour = false;
};

struct M3Material {
    std::string name;
    std::vector<M3Layer> layers;
    std::uint32_t flags = 0;
    std::uint32_t blend_mode = 0;
    std::int32_t priority = 0;
    float specularity = 0.0f;
    std::uint8_t cutout_thresh = 0;
    float spec_mult = 1.0f;
    bool spec_undimmed = false;
    bool spec_luma = false;
    float emis_mult = 1.0f;
    float displace = 0.0f;
    std::uint16_t displace_interp = 0, displace_flags = 0;
    std::uint32_t displace_id = 0;
    std::uint32_t emis_op = 2;
    std::uint32_t emis2_op = 2;
    float depth_blend = 0.0f;
    std::vector<std::uint8_t> raw_record;
};

enum M3Blend : std::uint32_t {
    kBlendOpaque = 0,
    kBlendAlphaBlend = 1,
    kBlendAdd = 2,
    kBlendAlphaAdd = 3,
};

inline bool material_is_additive(const M3Material& m) {
    return m.blend_mode == kBlendAdd || m.blend_mode == kBlendAlphaAdd;
}

inline const M3Layer* material_alpha_mask(const M3Material& m) {
    for (const M3Layer& L : m.layers)
        if (L.usage == "AlphaMask") return &L;
    return nullptr;
}

inline const M3Layer* material_const_alpha_layer(const M3Material& m) {
    if (m.blend_mode != kBlendAlphaBlend || material_alpha_mask(m)) return nullptr;
    for (const M3Layer& L : m.layers) {
        if (L.usage != "Diffuse") continue;
        if (L.raw_record.empty() || L.word44 != 0) return nullptr;
        const float a = L.anim.bright_mult + L.anim.midtone_offset;
        return a > 0.0f && a < 0.999f ? &L : nullptr;
    }
    return nullptr;
}

inline int material_alpha_mask_channel(const M3Material& m) {
    const M3Layer* L = material_alpha_mask(m);
    if (!L) return material_const_alpha_layer(m) ? 5 : 0;
    if (L->word44 == 0 && !L->raw_record.empty()) return 0;
    if (!L->raw_record.empty() && L->anim.midtone_offset >= 1.0f && L->anim.bright_mult >= 0.0f) return 0;
    switch (L->word44) {
        case 3: return 2;
        case 4: return 3;
        case 5: return 4;
        default: return 1;
    }
}

inline bool material_is_alpha_blended(const M3Material& m) {
    return m.blend_mode == kBlendAlphaBlend && (material_alpha_mask(m) != nullptr || material_const_alpha_layer(m) != nullptr);
}

struct M3MaterialRef {
    uint32_t type = 0;
    uint32_t index = 0;
};

struct M3CompositeChild {
    uint32_t matref = 0;
    uint32_t type = 0;
    uint32_t index = 0;
    bool resolved = false;
    float alpha = 1.0f;
    uint16_t alpha_interp = 0, alpha_flags = 0;
    uint32_t alpha_id = 0;
};

inline bool layer_fresnel(const M3Layer& L, std::uint32_t& mode, float& exponent, float& bias, float& scale) {
    static const bool kNoV26 = std::getenv("OMC_FRESNEL_NO_V26") != nullptr;
    const std::size_t base = (L.raw_record.size() == 480 || L.raw_record.size() == 448 || (!kNoV26 && L.raw_record.size() == 464)) ? 416
                                                                                                                                       : (L.raw_record.size() == 368 ? 336 : 0);
    if (!base) return false;
    auto u32 = [&](std::size_t o) {
        return std::uint32_t(L.raw_record[o]) | (std::uint32_t(L.raw_record[o + 1]) << 8) | (std::uint32_t(L.raw_record[o + 2]) << 16) |
               (std::uint32_t(L.raw_record[o + 3]) << 24);
    };
    auto f32 = [&](std::size_t o) {
        const std::uint32_t u = u32(o);
        float f = 0.0f;
        std::memcpy(&f, &u, 4);
        return f;
    };
    mode = u32(base);
    if (mode != 1 && mode != 2) return false;
    exponent = f32(base + 4);
    bias = f32(base + 8);
    scale = f32(base + 12);
    static const bool kRawScale = std::getenv("OMC_FRESNEL_SCALE_RAW") != nullptr;
    if (!kRawScale) scale -= bias;
    return exponent == exponent && exponent > 0.0f && exponent < 64.0f && bias == bias && scale == scale;
}

struct M3Composite {
    std::string name;
    uint32_t word12 = 0;
    std::vector<M3CompositeChild> children;
};

struct M3Sequence {
    std::string name;
    uint32_t start_ms = 0;
    uint32_t end_ms = 0;
    float movement_speed = 0;
    uint32_t flags = 0;
    uint32_t frequency = 0;
    uint32_t length_ms() const { return end_ms - start_ms; }
};

struct M3AnimRef {
    uint16_t interp = 0;
    uint16_t anim_flags = 0;
    uint32_t anim_id = 0;
    float init[4] = {0, 0, 0, 0};
};

struct M3Bone {
    std::string name;
    int parent = -1;
    uint32_t flags = 0;
    M3AnimRef location;
    M3AnimRef rotation;
    M3AnimRef scale;
    M3AnimRef visibility;
};

struct M3Event {
    std::string name;
    int32_t bone = -1;
    int32_t time_ms = 0;
    float matrix[16] = {};
};

struct M3Track {
    std::vector<int32_t> keys;
    std::vector<float> values;
    std::vector<M3Event> events;
    uint8_t comps = 0;
    uint8_t slot = 0;
};

struct M3STC {
    std::string name;
    std::vector<uint32_t> anim_ids;
    std::vector<uint32_t> anim_refs;
    std::vector<M3Track> sd[13];
};

struct M3STG {
    std::vector<uint32_t> stc_indices;
};

struct M3Attachment {
    std::string name;
    uint32_t bone = 0;
};

struct M3HitShape {
    uint32_t shape = 0;
    int bone = -1;
    float m[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    float size[3] = {0, 0, 0};
};

struct Model {
    std::string name;
    uint32_t vertex_flags = 0;
    uint32_t stride = 0;
    uint32_t vertex_count = 0;
    uint32_t bone_count = 0;
    float bounds_min[3] = {0, 0, 0};
    float bounds_max[3] = {0, 0, 0};
    float bounds_radius = 0;
    std::vector<float> positions;
    std::vector<float> uvs;
    std::vector<float> uvs1;
    std::vector<uint8_t> vertex_colors;
    std::vector<uint16_t> indices;
    std::vector<M3Region> regions;
    std::vector<M3Material> materials;
    std::vector<M3MaterialRef> material_refs;
    std::vector<int> region_material;
    std::vector<M3Composite> composites;
    std::vector<int> region_composite;
    std::vector<int> region_matref;
    std::vector<uint16_t> region_batches;
    std::vector<int> region_bone;
    std::vector<M3Region> terrain_regions;
    std::uint32_t caster_region_first = 0, caster_region_end = 0;

    std::vector<M3Bone> bones;
    std::vector<float> inverse_bind;
    std::vector<uint8_t> vertex_bone_weights;
    std::vector<uint8_t> vertex_bone_indices;
    std::vector<float> normals;
    std::vector<float> tangents;
    std::vector<uint8_t> normals_w;
    std::vector<uint16_t> bone_lookup;
    std::vector<M3Sequence> sequences;
    std::vector<M3STG> stg;
    std::vector<M3STC> stc;

    std::vector<M3Attachment> attachments;
    std::vector<uint16_t> attachment_lookup;
    std::vector<M3HitShape> hit_fuzzy;
    M3HitShape hit_tight;
    bool has_tight_hit = false;

    std::vector<M3Particle> particles;

    std::vector<M3Light> lights;
    std::vector<M3Ribbon> ribbons;
    struct Billboard { std::uint16_t bone = 0, type = 0; };
    std::vector<Billboard> billboards;
    std::vector<M3Projector> projectors;

    std::vector<M3Event> events_for_sequence(size_t seq) const;

    struct SplatLayerImage {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    std::vector<SplatLayerImage> splat_layers;
    std::vector<SplatLayerImage> splat_normals;
    std::vector<std::array<float, 3>> splat_height;
    uint32_t splat_w = 0, splat_h = 0;
    std::vector<uint8_t> splat_weights;
    float splat_extent_x = 0, splat_extent_y = 0;
    bool has_splat_payload() const {
        return splat_w && splat_h && !splat_layers.empty() &&
               splat_weights.size() >= size_t(splat_w) * splat_h * 8;
    }
    bool cliff_skin_opaque = false;
    bool vertex_alpha_coverage = false;
    bool placement_tint = false;
    std::vector<std::uint8_t> vertex_shade;
    std::vector<std::uint8_t> vertex_ao;
};

std::string m3_tag(uint32_t tag);

constexpr int kUntexturedRegion = -2;

inline bool fx_material_old() {
    static const bool kOld = std::getenv("OMC_FX_MATERIAL_OLD") != nullptr;
    return kOld;
}
inline int fx_standard_material(const Model& m, std::uint32_t ref) {
    if (ref >= m.material_refs.size()) return -1;
    const M3MaterialRef& r = m.material_refs[ref];
    if (r.type == 1) return r.index < m.materials.size() ? int(r.index) : -1;
    if (r.type != 3 || fx_material_old() || r.index >= m.composites.size()) return -1;
    for (const M3CompositeChild& ch : m.composites[r.index].children)
        if (ch.resolved && ch.type == 1 && ch.index < m.materials.size()) return int(ch.index);
    return -1;
}
inline bool fx_material_drawable(const Model& m, std::uint32_t ref) {
    return fx_material_old() || fx_standard_material(m, ref) >= 0;
}
inline int ribbon_material(const Model& m, const M3Ribbon& rb) {
    return fx_standard_material(m, rb.material_ref_index);
}
inline int particle_material(const Model& m, const M3Particle& pe) {
    return fx_standard_material(m, pe.material_ref_index);
}

}
