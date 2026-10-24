#include "sc2/data/environment.hpp"
#include "sc2/render/scene.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/dds.hpp"
#include "sc2/render/anim.hpp"
#include "sc2/render/device.hpp"
#include "sc2/render/frame_prof.hpp"
#include "sc2/render/scene_renderer.hpp"
#include "sc2/render/texture_decode.hpp"
#include "sc2/render/vk_helpers.hpp"
#include "sc2/render/vk_raii.hpp"
#include "mesh_build.hpp"

static const uint32_t kMeshVertSpv[] =
#include "scene.vert.spv.h"
    ;
static const uint32_t kMeshFragSpv[] =
#include "scene.frag.spv.h"
    ;
static const uint32_t kShadowVertSpv[] =
#include "shadow.vert.spv.h"
    ;
static const uint32_t kParticleVertSpv[] =
#include "particle.vert.spv.h"
    ;
static const uint32_t kParticleFragSpv[] =
#include "particle.frag.spv.h"
    ;
static const uint32_t kOverlayVertSpv[] =
#include "overlay.vert.spv.h"
    ;
static const uint32_t kTonemapFragSpv[] =
#include "tonemap.frag.spv.h"
    ;

namespace sc2::render {

namespace {

}

bool project(const Mat4& mvp, float x, float y, float z, uint32_t width,
             uint32_t height, float& px, float& py) {
    const auto& m = mvp.m;
    float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
    float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
    float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
    if (cw <= 1e-6f) return false;
    px = (cx / cw * 0.5f + 0.5f) * float(width);
    py = (cy / cw * 0.5f + 0.5f) * float(height);
    return true;
}

RenderResult render_scene(const std::vector<SceneInstance>& instances,
                          uint32_t width, uint32_t height, const Camera* cam_in,
                          const casc::Storage* storage,
                          const std::string& texture_root, bool enable_particles,
                          const data::EnvLight* env, const FogField* fog, uint32_t resize_from,
                          const float* window_shadow_clip) {
    std::vector<const SceneInstance*> live;
    for (const auto& inst : instances)
        if (inst.model && inst.model->vertex_count && !inst.model->indices.empty())
            live.push_back(&inst);
    if (live.empty()) throw std::runtime_error("render_scene: no drawable instances");

    Device& gpu = Device::shared_headless("render_scene");
    const bool marks = mem_lap_marks();
    if (marks) mem_mark(Device::shared_is_adopted() ? "render_scene: device (the window's)" : "render_scene: device (its own)");
    const VkPhysicalDevice pd = gpu.physical();
    const VkDevice dev = gpu.device();
    const VkQueue queue = gpu.graphics_queue();
    const uint32_t gfx = gpu.graphics_family();

    SceneRenderer::Tunables tun;
    if (window_shadow_clip) {
        tun.camera_fit_shadow = true;
        tun.shadow_clip = *window_shadow_clip;
        tun.clear_color[0] = tun.clear_color[1] = tun.clear_color[2] = 0.0f;
    }
    tun.upload_batch_mb = upload_batch_budget_mb();
    SceneRenderer sr(pd, dev, queue, gfx, resize_from ? resize_from : width, resize_from ? resize_from : height,
                     VK_FORMAT_R8G8B8A8_UNORM, storage, texture_root, tun);
    if (resize_from) sr.resize(width, height);
    if (env) sr.set_environment(*env);
    if (fog) sr.set_fog(*fog);
    if (marks) mem_mark("render_scene: renderer (pipelines, targets)");
    std::vector<const SceneInstance*> light_only;
    for (const auto& inst : instances)
        if (inst.model && !(inst.model->vertex_count && !inst.model->indices.empty()) && !inst.model->lights.empty())
            light_only.push_back(&inst);
    sr.prepare(live, &light_only);
    if (marks) mem_mark("render_scene: prepare (meshes, textures)");

    Camera cam;
    if (cam_in) {
        cam = *cam_in;
    } else {
        Vec3 wmn = sr.aabb_min(), wmx = sr.aabb_max();
        Vec3 center{(wmn.x + wmx.x) * 0.5f, (wmn.y + wmx.y) * 0.5f, (wmn.z + wmx.z) * 0.5f};
        float radius = 0.5f * std::sqrt(dot(sub(wmx, wmn), sub(wmx, wmn)));
        if (radius < 1e-4f) radius = 1.0f;
        float dist = radius / std::tan(cam.fovy * 0.5f) * 1.5f;
        Vec3 dir = normalize({0.9f, -1.3f, 0.7f});
        cam.center = center;
        cam.eye = {center.x + dir.x * dist, center.y + dir.y * dist, center.z + dir.z * dist};
        cam.up = {0, 0, 1};
        cam.znear = dist * 0.02f;
        cam.zfar = dist + radius * 4.0f;
    }

    std::vector<SceneRenderer::ParticleVertex> pverts;
    std::vector<SceneRenderer::ParticleGroup> pgroups;
    std::vector<const SceneInstance*> emitting = live;
    for (const auto& inst : instances)
        if (inst.model && !(inst.model->vertex_count && !inst.model->indices.empty()) &&
            (!inst.model->particles.empty() || !inst.model->ribbons.empty()))
            emitting.push_back(&inst);
    if (enable_particles) build_particles(sr, emitting, cam, pverts, pgroups);

    VkDeviceSize rbytes = VkDeviceSize(width) * height * 4;
    Buffer readback = make_buffer(pd, dev, rbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr);

    VkCommandPoolCreateInfo cpi{};

    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.queueFamilyIndex = gfx;
    VkCommandPool rawCmdPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev, &cpi, nullptr, &rawCmdPool), "vkCreateCommandPool");
    Unique<VkCommandPool> cmdPool(rawCmdPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = rawCmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bbi), "vkBeginCommandBuffer");

    sr.record(cmd, live, cam, pverts, pgroups);

    VkImageMemoryBarrier lb{};
    lb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    lb.oldLayout = lb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    lb.srcQueueFamilyIndex = lb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lb.image = sr.ldr_image();
    lb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    lb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    lb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lb);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, sr.ldr_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buf.get(), 1, &region);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkFenceCreateInfo fi{};

    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence rawFence = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &fi, nullptr, &rawFence), "vkCreateFence");
    Unique<VkFence> fence(rawFence, [dev](VkFence f) { vkDestroyFence(dev, f, nullptr); });
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(queue, 1, &si, rawFence), "vkQueueSubmit");
    check(vkWaitForFences(dev, 1, &rawFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    RenderResult out;
    out.width = width;
    out.height = height;
    out.mvp = cam.view_proj(float(width) / float(height));
    out.lit_drawn = sr.lit_drawn();
    out.lit_culled = sr.lit_culled();
    out.rgba.resize(size_t(rbytes));
    void* mapped = nullptr;
    check(vkMapMemory(dev, readback.mem.get(), 0, rbytes, 0, &mapped), "vkMapMemory(readback)");
    std::memcpy(out.rgba.data(), mapped, size_t(rbytes));
    vkUnmapMemory(dev, readback.mem.get());
    if (marks) mem_mark("render_scene: drawn + read back");
    return out;
}

static bool (*g_session_find)(const std::string&, uint32_t, uint32_t) = nullptr;
static void (*g_session_release)() = nullptr;

void release_scene_sessions() {
    if (g_session_release) g_session_release();
}

bool render_scene_session_exists(const std::string& key, uint32_t width, uint32_t height) {
    return g_session_find && g_session_find(key, width, height);
}

uint32_t render_scene_session_texture_max() {
    static const uint32_t v = std::getenv("SC2_PORTRAIT_TEX_MAX") ? uint32_t(std::max(0, std::atoi(std::getenv("SC2_PORTRAIT_TEX_MAX")))) : 1024u;
    return v;
}

RenderResult render_scene_session(const std::string& key, const std::vector<SceneInstance>& instances, uint32_t width, uint32_t height,
                                  const Camera& cam, const casc::Storage* storage, const data::EnvLight* env,
                                  std::size_t preload_budget, bool* preload_done) {
    std::vector<const SceneInstance*> live;
    for (const auto& inst : instances)
        if (inst.model && inst.model->vertex_count && !inst.model->indices.empty()) live.push_back(&inst);
    if (live.empty()) throw std::runtime_error("render_scene_session: no drawable instances");

    Device& gpu = Device::shared_headless("render_scene");
    const VkPhysicalDevice pd = gpu.physical();
    const VkDevice dev = gpu.device();
    const VkQueue queue = gpu.graphics_queue();
    const uint32_t gfx = gpu.graphics_family();

    struct Session {
        std::string key;
        uint32_t w = 0, h = 0;
        std::unique_ptr<SceneRenderer> sr;
        Buffer readback;
        Unique<VkCommandPool> pool;
        VkCommandBuffer cmd = VK_NULL_HANDLE;
        Unique<VkFence> fence;
    };
    static const std::size_t kKeep = std::getenv("SC2_PORTRAIT_SESSION_ONE") ? 1u : 24u;
    static const bool kOwnShadow = std::getenv("SC2_PORTRAIT_SHADOW_OWN") != nullptr;
    static std::shared_ptr<Image> shared_shadow;
    static std::vector<std::unique_ptr<Session>> sessions;
    struct KeysOf {
        static bool find(const std::string& k, uint32_t w, uint32_t h) {
            for (const auto& s : sessions)
                if (s->key == k && s->w == w && s->h == h) return true;
            return false;
        }
    };
    g_session_find = &KeysOf::find;
    struct ReleaseAll {
        static void run() {
            if (!sessions.empty() || shared_shadow) {
                vkDeviceWaitIdle(Device::shared_headless("render_scene").device());
                sessions.clear();
                shared_shadow.reset();
            }
        }
    };
    g_session_release = &ReleaseAll::run;
    auto sessions_bytes = [&]() {
        std::size_t b = 0;
        if (shared_shadow)
            for (const auto& s : sessions) { b += std::size_t(s->sr->tunables().shadow_dim) * s->sr->tunables().shadow_dim * 4; break; }
        for (const auto& s : sessions) b += s->sr->gpu_bytes_estimate();
        return b;
    };
    const std::size_t bytes_before = sessions_bytes();
    bool made = false;
    std::size_t hit = sessions.size();
    for (std::size_t i = 0; i < sessions.size(); ++i)
        if (sessions[i]->key == key && sessions[i]->w == width && sessions[i]->h == height) { hit = i; break; }
    if (hit < sessions.size()) {
        std::rotate(sessions.begin(), sessions.begin() + std::ptrdiff_t(hit), sessions.begin() + std::ptrdiff_t(hit) + 1);
    } else {
        SC2_PROF_SCOPE("ps.new");
        made = true;
        if (sessions.size() >= kKeep) {
            vkDeviceWaitIdle(dev);
            sessions.resize(kKeep - 1);
        }
        auto s = std::make_unique<Session>();
        s->key = key;
        s->w = width;
        s->h = height;
        SceneRenderer::Tunables tun;
        tun.max_texture_dim = render_scene_session_texture_max();
        if (!kOwnShadow) {
            uint32_t dim = tun.shadow_dim;
            if (const char* sd = std::getenv("SC2_SHADOW_DIM")) {
                const int v = std::atoi(sd);
                if (v >= 1024 && v <= 8192) dim = uint32_t(v);
            }
            tun.shadow_dim = dim;
            if (!shared_shadow)
                shared_shadow = std::make_shared<Image>(make_image(pd, dev, VK_FORMAT_D32_SFLOAT,
                                                                   VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                                   VK_IMAGE_ASPECT_DEPTH_BIT, dim, dim));
            tun.shared_shadow = shared_shadow;
        }
        s->sr = std::make_unique<SceneRenderer>(pd, dev, queue, gfx, width, height, VK_FORMAT_R8G8B8A8_UNORM, storage, std::string(), tun);
        if (env) s->sr->set_environment(*env);
        s->readback = make_buffer(pd, dev, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr);
        VkCommandPoolCreateInfo cpi{};
        cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpi.queueFamilyIndex = gfx;
        VkCommandPool rawPool = VK_NULL_HANDLE;
        check(vkCreateCommandPool(dev, &cpi, nullptr, &rawPool), "vkCreateCommandPool(session)");
        s->pool = Unique<VkCommandPool>(rawPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });
        VkCommandBufferAllocateInfo cbi{};
        cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
        cbi.commandPool = rawPool;
        cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        cbi.commandBufferCount = 1;
        check(vkAllocateCommandBuffers(dev, &cbi, &s->cmd), "vkAllocateCommandBuffers(session)");
        VkFenceCreateInfo fi{};
        fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
        VkFence rawFence = VK_NULL_HANDLE;
        check(vkCreateFence(dev, &fi, nullptr, &rawFence), "vkCreateFence(session)");
        s->fence = Unique<VkFence>(rawFence, [dev](VkFence f) { vkDestroyFence(dev, f, nullptr); });
        sessions.insert(sessions.begin(), std::move(s));
    }
    Session& S = *sessions.front();
    SceneRenderer& sr = *S.sr;
    if (preload_done) {
        SC2_PROF_SCOPE("ps.preload");
        bool done = true;
        for (const SceneInstance* inst : live) done = sr.preload_model_textures(*inst->model, preload_budget) && done;
        *preload_done = done;
        return RenderResult{};
    }
    { SC2_PROF_SCOPE("ps.prepare"); sr.prepare(live, nullptr); }
    if (made) {
        const double d = (double(sessions_bytes()) - double(bytes_before)) / (1024.0 * 1024.0);
        frame_prof_count("ps.mb_add", d);
        if (std::getenv("SC2_PORTRAIT_MEM_TRACE"))
            std::fprintf(stderr, "portrait sessions: %zu, %.1f MB (+%.1f)\n", sessions.size(), double(sessions_bytes()) / (1024.0 * 1024.0), d);
    }

    const VkDeviceSize rbytes = VkDeviceSize(width) * height * 4;
    check(vkResetCommandBuffer(S.cmd, 0), "vkResetCommandBuffer(session)");
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(S.cmd, &bbi), "vkBeginCommandBuffer(session)");
    const std::vector<SceneRenderer::ParticleVertex> pverts;
    const std::vector<SceneRenderer::ParticleGroup> pgroups;
    sr.record(S.cmd, live, cam, pverts, pgroups);
    VkImageMemoryBarrier lb{};
    lb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    lb.oldLayout = lb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    lb.srcQueueFamilyIndex = lb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    lb.image = sr.ldr_image();
    lb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    lb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    lb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(S.cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lb);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(S.cmd, sr.ldr_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, S.readback.buf.get(), 1, &region);
    check(vkEndCommandBuffer(S.cmd), "vkEndCommandBuffer(session)");

    const VkFence fence = S.fence.get();
    check(vkResetFences(dev, 1, &fence), "vkResetFences(session)");
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &S.cmd;
    {
        SC2_PROF_SCOPE("ps.gpu");
        check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit(session)");
        check(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(session)");
    }

    RenderResult out;
    out.width = width;
    out.height = height;
    out.mvp = cam.view_proj(float(width) / float(height));
    out.lit_drawn = sr.lit_drawn();
    out.lit_culled = sr.lit_culled();
    out.rgba.resize(size_t(rbytes));
    void* mapped = nullptr;
    check(vkMapMemory(dev, S.readback.mem.get(), 0, rbytes, 0, &mapped), "vkMapMemory(session readback)");
    std::memcpy(out.rgba.data(), mapped, size_t(rbytes));
    vkUnmapMemory(dev, S.readback.mem.get());
    return out;
}

}
