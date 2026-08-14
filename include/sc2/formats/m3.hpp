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

struct M3Particle {
    uint32_t bone = 0;
    uint32_t material_ref_index = 0;
    float emission_speed = 0;
    float lifetime = 0;
    float emission_rate = 0;
    float size_start = 0;
    float size_end = 0;
    uint32_t version = 0;
};

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

    uint32_t bone_count() const;
};

struct M3Region {
    uint32_t first_vertex = 0;
    uint32_t num_vertices = 0;
    uint32_t first_index = 0;
    uint32_t num_indices = 0;
    uint32_t first_bone_lookup = 0;
};

struct M3Layer {
    std::string usage;
    std::string image_path;
    std::vector<uint8_t> inline_rgba;
    uint32_t inline_w = 0, inline_h = 0;
};

struct M3Material {
    std::string name;
    std::vector<M3Layer> layers;
};

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
    std::vector<uint16_t> bone_lookup;
    std::vector<M3Sequence> sequences;
    std::vector<M3STG> stg;
    std::vector<M3STC> stc;

    std::vector<M3Attachment> attachments;
    std::vector<uint16_t> attachment_lookup;

    std::vector<M3Particle> particles;

    std::vector<M3Event> events_for_sequence(size_t seq) const;
};

std::string m3_tag(uint32_t tag);

}
