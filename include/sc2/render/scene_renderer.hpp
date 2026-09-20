#pragma once

#include <vulkan/vulkan.h>

#include <array>
#include <cstdint>
#include <functional>
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
    };
    struct ParticleGroup {
        int tex;
        uint32_t first;
        uint32_t count;
    };

    struct Tunables {
        float exposure = 1.0f;
        int32_t curve = 0;
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
    void resize(uint32_t w, uint32_t h);
    uint32_t lit_drawn() const { return lit_drawn_; }
    uint32_t lit_culled() const { return lit_culled_; }
    static void apply_environment(Tunables& tun, const data::EnvLight& env);
    const Tunables& tunables() const { return tun_; }

    int resolve_texture(const formats::Model& m, int material, const char* usage, int def);
    int default_white() const { return kDefaultWhite; }
    size_t lights_seen() const { return lights_seen_; }
    size_t lights_drawn() const { return lights_drawn_; }
    int default_normal() const { return kDefaultNormal; }
    int default_emissive() const { return kDefaultEmissive; }
    int default_specular() const { return kDefaultSpecular; }

    void prepare(const std::vector<const SceneInstance*>& live);

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
        std::vector<std::array<int, 5>> draw_tex;
        std::vector<VkDescriptorSet> draw_set;
        std::uint64_t version = 0;
    };

    void one_time(const std::function<void(VkCommandBuffer)>& rec);
    int upload_texture(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba);
    Mesh& ensure_mesh(const formats::Model& m, std::uint64_t version = 0);
    std::vector<VkDescriptorSet> free_mat_sets_;

    struct TerrainSplat {
        Image diffuse_arr, weight_arr;
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
    int kDefaultWhite = 0, kDefaultNormal = 1, kDefaultEmissive = 2, kDefaultSpecular = 3;

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
    float fog_world_w_ = 0.0f, fog_world_h_ = 0.0f;
    bool fog_present_ = false;
    void upload_fog_image(uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba);
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
    struct FrameLight {
        Vec3 pos, col;
        float att_near = 0.0f, att_far = 0.0f;
    };
    std::vector<FrameLight> frame_lights_;
    size_t lights_seen_ = 0, lights_drawn_ = 0;
    Unique<VkPipeline> shadowPipeline_;
    Unique<VkPipeline> partPipeline_;
    Unique<VkPipeline> tmPipeline_;
    Unique<VkPipeline> splatPipeline_;
    Unique<VkShaderModule> vs_, fs_, svs_, pvs_, pfs_, tmvs_, tmfs_, splatfs_;

    Unique<VkRenderPass> litPass_;
    Unique<VkRenderPass> shadowPass_;
    Unique<VkRenderPass> tmPass_;
    Unique<VkFramebuffer> litFb_;
    Unique<VkFramebuffer> shadowFb_;
    Unique<VkFramebuffer> tmFb_;

    Image hdrColor_, ldrColor_, depth_, shadowMap_;

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
};

void build_particles(SceneRenderer& sr, const std::vector<const SceneInstance*>& live,
                     const Camera& cam,
                     std::vector<SceneRenderer::ParticleVertex>& pverts,
                     std::vector<SceneRenderer::ParticleGroup>& pgroups);

}
