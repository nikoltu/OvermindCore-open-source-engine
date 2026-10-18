#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/render/fog_field.hpp"
#include "sc2/render/light_rig.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/render/vk_helpers.hpp"
#include "sc2/render/vk_raii.hpp"
#include "mesh_build.hpp"

namespace sc2::casc {
class Storage;
}
namespace sc2::data {
struct EnvLight;
}

namespace sc2::render {

class SceneRenderer {
public:
    struct ParticleVertex {
        float pos[3];
        float uv[2];
        float color[4];
        float fog[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    };
    struct ParticleGroup {
        int tex;
        uint32_t first;
        uint32_t count;
        uint32_t blend = 3;
        int mask_tex = -1;
        float mask_code = 0.0f;
        float mask_mul = 1.0f, mask_add = 0.0f;
        int mask2_tex = -1;
        float mask2_code = 0.0f, mask2_mul = 1.0f, mask2_add = 0.0f;
        bool glow = false;
        float emis[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        int emis_tex = -1;
        bool mask_alone = false;
        float depth_blend = 0.0f;
    };

    struct Tunables {
        float exposure = 1.0f;
        int32_t curve = 0;
        float white_point = 1.85f;
        float col_blend = 0.0f;
        float col_contrast = 1.0f;
        float col_hue = 0.0f;
        float col_saturation = 1.0f;
        float col_lightness = 0.0f;
        float col_brightness = 1.0f, col_input_low = 0.0f, col_input_high = 1.0f, col_input_gamma = 1.0f;
        bool col_levels = false;
        float bloom_threshold = 0.0f, bloom_gain = 0.0f, bloom_scale = 0.0f;
        Vec3 light_dir{-0.4f, -0.6f, -0.7f};
        uint32_t shadow_dim = 2048;
        float depth_bias = 0.0015f;
        float shadow_strength = 1.0f;
        float raster_const = 1.25f;
        float raster_slope = 1.75f;
        float fit_pad = 0.5f;
        float gloss = 24.0f;
        float spec_strength = 1.0f;
        LightRig rig;
        float rig_shadow_floor = 0.51f;
        bool camera_fit_shadow = false;
        float shadow_clip = 0.0f;
        float clear_color[3] = {0.06f, 0.07f, 0.09f};
        float shadow_penumbra = 0.0f;
        float creep_spec_mult = 0.0f, creep_spec_exp = 0.0f;
        float terrain_diffuse = 1.0f, terrain_spec = 0.0f, terrain_spec_exp = 100.0f;
        bool gamma_space = false;
        bool spec_no_energy_dim = false;
        int32_t hdr_operator = 0;
        float hdr_ambient_mult = 1.0f, hdr_diffuse_mult = 1.0f, hdr_emis_mult = 1.0f;
        float ao_radius = 0.1f, ao_power = 1.97f, ao_no_occlusion = 0.02f;
        float ao_detail_radius = 0.023f, ao_detail_power = 3.61f, ao_detail_no_occlusion = 0.015f;
        float hdr_spec_mult = 1.0f;
        bool height_fog = false;
        float fog_color[3] = {0, 0, 0};
        float fog_density = 0.0f, fog_falloff = 0.0f, fog_start = 0.0f;
    };

    SceneRenderer(VkPhysicalDevice pd, VkDevice dev, VkQueue queue, uint32_t gfx_family,
                  uint32_t width, uint32_t height, VkFormat ldr_format,
                  const casc::Storage* storage, std::string texture_root,
                  const Tunables& tun);
    SceneRenderer(VkPhysicalDevice pd, VkDevice dev, VkQueue queue, uint32_t gfx_family,
                  uint32_t width, uint32_t height, VkFormat ldr_format,
                  const casc::Storage* storage, std::string texture_root)
        : SceneRenderer(pd, dev, queue, gfx_family, width, height, ldr_format, storage,
                        std::move(texture_root), Tunables()) {}

    void set_environment(const data::EnvLight& env);
    void set_fog(const FogField& fog);
    float fog_alpha_at(float x, float y) const;
    bool fog_field_contains(float x, float y) const {
        return !fog_present_ || fog_world_w_ <= 0.0f || fog_world_h_ <= 0.0f || (x >= 0.0f && y >= 0.0f && x <= fog_world_w_ && y <= fog_world_h_);
    }
    void flush_pending_fog(VkCommandBuffer cmd);
    void resize(uint32_t w, uint32_t h);
    uint32_t lit_drawn() const { return lit_drawn_; }
    uint32_t lit_culled() const { return lit_culled_; }
    static void apply_environment(Tunables& tun, const data::EnvLight& env);
    const Tunables& tunables() const { return tun_; }

    int resolve_texture(const formats::Model& m, int material, const char* usage, int def);
    int resolve_env_texture(const formats::Model& m, int material);
    int default_white() const { return kDefaultWhite; }
    Vec3 trail_origin(std::uint64_t key, float now_s, float age_s, const Vec3& current);
    void trail_note(std::uint64_t key, float now_s, const Vec3& current);
    size_t lights_seen() const { return lights_seen_; }
    size_t lights_drawn() const { return lights_drawn_; }
    int default_normal() const { return kDefaultNormal; }
    int default_emissive() const { return kDefaultEmissive; }
    int default_specular() const { return kDefaultSpecular; }

    void prepare(const std::vector<const SceneInstance*>& live, const std::vector<const SceneInstance*>* light_only = nullptr);

    void record(VkCommandBuffer cmd, const std::vector<const SceneInstance*>& live,
                const Camera& cam,
                const std::vector<ParticleVertex>& pverts = {},
                const std::vector<ParticleGroup>& pgroups = {});

    Vec3 aabb_min() const { return aabb_min_; }
    Vec3 aabb_max() const { return aabb_max_; }

    VkImage ldr_image() const { return ldrColor_.img.get(); }
    VkImageView ldr_view() const { return ldrColor_.view.get(); }
    VkFormat ldr_format() const { return ldr_format_; }
    uint32_t width() const { return width_; }
    uint32_t height() const { return height_; }

private:
    struct Mesh {
        Buffer vbuf;
        Buffer ibuf;
        Vec3 lmin{0, 0, 0}, lmax{0, 0, 0};
        std::vector<Draw> draws;
        std::vector<std::array<int, 11>> draw_tex;
        std::vector<std::array<float, 16>> draw_maskx;
        bool any_maskx = false;
        std::vector<std::array<float, 24>> draw_fres;
        bool any_fres = false;
        std::vector<std::array<float, 4>> draw_env;
        bool any_env = false;
        bool any_displace = false;
        std::vector<VkDescriptorSet> draw_set;
        std::uint64_t version = 0;
        std::vector<std::uint8_t> draw_uvx;
        bool any_uvx = false;
        std::vector<std::array<float, 2>> draw_masklv;
        bool any_masklv = false;
        std::vector<float> draw_specm;
        bool any_specm = false;
        std::vector<std::array<float, 3>> draw_specl;
        std::vector<std::uint8_t> draw_specluma;
        std::vector<std::array<float, 3>> draw_tri;
        std::vector<std::uint16_t> draw_uvsrc;
        bool any_uvsrc = false;
    };

    void one_time(const std::function<void(VkCommandBuffer)>& rec);
    int upload_texture(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba, bool srgb = false);
    Mesh& ensure_mesh(const formats::Model& m, std::uint64_t version = 0);
    std::vector<VkDescriptorSet> free_mat_sets_;

    struct TerrainSplat {
        Image diffuse_arr, weight_arr;
        Image normal_arr;
        VkDescriptorSet set = VK_NULL_HANDLE;
        float extent[2] = {0, 0};
    };
    TerrainSplat* ensure_splat(const formats::Model& m);

    VkPhysicalDevice pd_;
    VkDevice dev_;
    VkQueue queue_;
    uint32_t gfx_family_;
    uint32_t width_, height_;
    std::vector<std::array<Vec3, 2>> inst_aabb_;
    uint32_t lit_drawn_ = 0, lit_culled_ = 0;
    void create_targets();
    VkFormat ldr_format_;
    const casc::Storage* storage_;
    std::string texture_root_;
    Tunables tun_;

    static constexpr VkFormat kHdr = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr VkFormat kDepthF = VK_FORMAT_D32_SFLOAT;
    static constexpr VkFormat kTexColor = VK_FORMAT_R8G8B8A8_UNORM;
    static constexpr VkFormat kTexColorSrgb = VK_FORMAT_R8G8B8A8_SRGB;
    int kDefaultWhite = 0, kDefaultNormal = 1, kDefaultEmissive = 2, kDefaultSpecular = 3, kDefaultClear = 4;

    std::vector<Image> textures_;
    std::unordered_map<std::string, int> tex_index_;
    std::unordered_map<const void*, int> inline_index_;
    std::unordered_map<const formats::Model*, Mesh> mesh_cache_;

    Unique<VkCommandPool> cmdPool_;
    Unique<VkSampler> sampler_;
    Unique<VkSampler> shadowSampler_;
    Unique<VkSampler> fogSampler_;
    Image fogImg_;
    std::vector<uint8_t> fog_last_;
    uint32_t fog_w_ = 0, fog_h_ = 0;
    std::vector<uint8_t> fog_pending_;
    uint32_t fog_pending_w_ = 0, fog_pending_h_ = 0;
    Buffer fog_stage_;
    float fog_world_w_ = 0.0f, fog_world_h_ = 0.0f;
    bool fog_present_ = false;
    uint32_t fog_cpu_w_ = 0, fog_cpu_h_ = 0;
    void upload_fog_image(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba);
    Image creepImg_;
    std::vector<uint8_t> creep_last_;
    uint32_t creep_w_ = 0, creep_h_ = 0;
    void upload_creep_image(uint32_t w, uint32_t h, const std::vector<uint8_t>& r8);
    Unique<VkDescriptorSetLayout> texLayout_;
    Unique<VkDescriptorSetLayout> boneLayout_;
    Unique<VkDescriptorSetLayout> globalLayout_;
    Unique<VkDescriptorSetLayout> partLayout_;
    Unique<VkDescriptorSetLayout> tmLayout_;
    Unique<VkDescriptorSetLayout> splatLayout_;

    Unique<VkPipelineLayout> layout_;
    Unique<VkPipelineLayout> partPipeLayout_;
    Unique<VkPipelineLayout> tmPipeLayout_;
    Unique<VkPipelineLayout> splatPipeLayout_;
    Unique<VkPipeline> litPipeline_;
    Unique<VkPipeline> addPipeline_, alphaAddPipeline_;
    Unique<VkPipeline> alphaBlendPipeline_;
    std::set<int> env_sphere_tex_;
    Unique<VkPipeline> depthPrepassPipeline_;
    Unique<VkPipeline> alphaBlendEqualPipeline_;
    struct FrameLight {
        Vec3 pos, col;
        float att_near = 0.0f, att_far = 0.0f;
        Vec3 spot_dir{0, 0, 0};
        float cos_outer = 0.0f, cone_scale = 0.0f;
    };
    std::vector<FrameLight> frame_lights_;
    size_t lights_seen_ = 0, lights_drawn_ = 0;
    Unique<VkPipeline> shadowPipeline_;
    Unique<VkPipeline> shadowCutoutPipeline_;
    Unique<VkShaderModule> sfs_;
    Unique<VkPipeline> partPipeline_;
    Unique<VkPipeline> partAddPipeline_;
    Unique<VkPipeline> partAlphaBlendPipeline_;
    Unique<VkPipeline> partGlowPipeline_, partGlowAddPipeline_, partGlowAlphaBlendPipeline_;
    Unique<VkPipeline> addGlowPipeline_, alphaAddGlowPipeline_;
    Unique<VkPipeline> tmPipeline_;
    Unique<VkPipeline> aoPipelineA_, aoPipelineB_;
    Unique<VkShaderModule> aofs_;
    Unique<VkRenderPass> aoPass_;
    Unique<VkFramebuffer> aoFb_;
    Image bloomA_, bloomB_;
    uint32_t bloom_w_ = 0, bloom_h_ = 0;
    Unique<VkRenderPass> bloomPass_;
    Unique<VkFramebuffer> bloomFbA_, bloomFbB_;
    Unique<VkDescriptorPool> bloomPool_;
    VkDescriptorSet bloom_set_[3] = {VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    Unique<VkPipeline> bloomPipeline_;
    Unique<VkShaderModule> bloomfs_;
    Unique<VkPipeline> splatPipeline_;
    Unique<VkShaderModule> vs_, fs_, svs_, pvs_, pfs_, tmvs_, tmfs_, splatfs_;

    Unique<VkRenderPass> litPass_;
    Unique<VkRenderPass> shadowPass_;
    Unique<VkRenderPass> tmPass_;
    Unique<VkFramebuffer> litFb_;
    Unique<VkFramebuffer> shadowFb_;
    Unique<VkFramebuffer> tmFb_;

    Image hdrColor_, ldrColor_, depth_, shadowMap_;
    Buffer fxBuf_;
    uint32_t fx_cap_ = 0;
    void ensure_fx_capacity(uint32_t records);
    Image sceneCopy_;
    Image depthCopy_;
    uint32_t copy_w_ = 0, copy_h_ = 0;
    Unique<VkRenderPass> litPassA_;
    Unique<VkRenderPass> litPassB_;

    Buffer globalsBuf_;
    Unique<VkDescriptorPool> globalPool_;
    VkDescriptorSet global_set_ = VK_NULL_HANDLE;
    Unique<VkDescriptorPool> tmPool_;
    VkDescriptorSet tm_set_ = VK_NULL_HANDLE;

    Unique<VkDescriptorPool> matPool_;
    std::unordered_map<const formats::Model*, TerrainSplat> splat_cache_;
    Unique<VkDescriptorPool> splatPool_;
    Unique<VkDescriptorPool> bonePool_;
    Unique<VkDescriptorPool> partPool_;
    std::vector<Unique<VkDescriptorPool>> matPoolsExtra_, bonePoolsExtra_, partPoolsExtra_;
    VkDescriptorSet alloc_set(Unique<VkDescriptorPool>& main, std::vector<Unique<VkDescriptorPool>>& extra,
                              VkDescriptorSetLayout layout, VkDescriptorType type, uint32_t per_set,
                              uint32_t max_sets, const char* what);

    std::vector<Buffer> bonebufs_;
    std::vector<VkDeviceSize> bone_cap_;
    Buffer partbuf_;

    Vec3 aabb_min_{0, 0, 0}, aabb_max_{0, 0, 0};
    struct TrailSample { float t; Vec3 p; };
    std::unordered_map<std::uint64_t, std::vector<TrailSample>> trails_;
};

void build_particles(SceneRenderer& sr, const std::vector<const SceneInstance*>& live,
                     const Camera& cam,
                     std::vector<SceneRenderer::ParticleVertex>& pverts,
                     std::vector<SceneRenderer::ParticleGroup>& pgroups);

}
