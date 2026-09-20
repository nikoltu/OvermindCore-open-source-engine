#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc2::formats {

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
    uint32_t version = 0;
};

enum M3LightType : uint32_t { kLightPoint = 1, kLightSpot = 2 };

struct M3Particle {
    uint32_t bone = 0;
    uint32_t material_ref_index = 0;
    float emission_speed = 0;
    float lifetime = 0;
    float emission_rate = 0;
    float size_start = 0;
    float size_end = 0;
    uint32_t version = 0;
    std::vector<uint8_t> raw;

    uint32_t additional_flags = 0;
    uint32_t rate_anim_id = 0;

    uint32_t life_anim_flags = 0;
    uint32_t life_anim_id = 0;
    uint16_t rate_anim_flags = 0;
    uint16_t rate_interp = 0;
    uint32_t max_particles = 0;
};

int particle_count(const M3Particle& p);

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

    void decode_animation(Model& m) const;

    std::vector<M3Particle> decode_particles() const;
    std::vector<M3Light> decode_lights() const;

    uint32_t bone_count() const;
};

struct M3Region {
    uint32_t first_vertex = 0;
    uint32_t num_vertices = 0;
    uint32_t first_index = 0;
    uint32_t num_indices = 0;
    uint32_t first_bone_lookup = 0;
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
    } anim;
    std::vector<uint8_t> inline_rgba{};
    uint32_t inline_w = 0, inline_h = 0;
};

struct M3Material {
    std::string name;
    std::vector<M3Layer> layers;
    std::uint32_t flags = 0;
    std::uint32_t blend_mode = 0;
    std::int32_t priority = 0;
    float specularity = 0.0f;
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

inline int material_alpha_mask_channel(const M3Material& m) {
    const M3Layer* L = material_alpha_mask(m);
    if (!L) return 0;
    switch (L->word44) {
        case 3: return 2;
        case 4: return 3;
        case 5: return 4;
        default: return 1;
    }
}

inline bool material_is_alpha_blended(const M3Material& m) {
    return m.blend_mode == kBlendAlphaBlend && material_alpha_mask(m) != nullptr;
}

struct M3MaterialRef {
    uint32_t type = 0;
    uint32_t index = 0;
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
    std::vector<uint16_t> indices;
    std::vector<M3Region> regions;
    std::vector<M3Material> materials;
    std::vector<M3MaterialRef> material_refs;
    std::vector<int> region_material;

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

    std::vector<M3Particle> particles;

    std::vector<M3Light> lights;

    std::vector<M3Event> events_for_sequence(size_t seq) const;

    struct SplatLayerImage {
        uint32_t w = 0, h = 0;
        std::vector<uint8_t> rgba;
    };
    std::vector<SplatLayerImage> splat_layers;
    uint32_t splat_w = 0, splat_h = 0;
    std::vector<uint8_t> splat_weights;
    float splat_extent_x = 0, splat_extent_y = 0;
    bool has_splat_payload() const {
        return splat_w && splat_h && !splat_layers.empty() &&
               splat_weights.size() >= size_t(splat_w) * splat_h * 8;
    }
    bool cliff_skin_opaque = false;
};

std::string m3_tag(uint32_t tag);

constexpr int kUntexturedRegion = -2;

inline int particle_material(const Model& m, const M3Particle& pe) {
    if (pe.material_ref_index >= m.material_refs.size()) return -1;
    const M3MaterialRef& r = m.material_refs[pe.material_ref_index];
    if (r.type != 1 || r.index >= m.materials.size()) return -1;
    return int(r.index);
}

}
