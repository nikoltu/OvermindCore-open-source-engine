#include "sc2/data/environment.hpp"
#include "sc2/render/scene.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
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
#include "sc2/render/scene_renderer.hpp"
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
    SceneRenderer sr(pd, dev, queue, gfx, resize_from ? resize_from : width, resize_from ? resize_from : height,
                     VK_FORMAT_R8G8B8A8_UNORM, storage, texture_root, tun);
    if (resize_from) sr.resize(width, height);
    if (env) sr.set_environment(*env);
    if (fog) sr.set_fog(*fog);
    std::vector<const SceneInstance*> light_only;
    for (const auto& inst : instances)
        if (inst.model && !(inst.model->vertex_count && !inst.model->indices.empty()) && !inst.model->lights.empty())
            light_only.push_back(&inst);
    sr.prepare(live, &light_only);

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
    return out;
}

RenderResult render_scene_session(const std::string& key, const std::vector<SceneInstance>& instances, uint32_t width, uint32_t height,
                                  const Camera& cam, const casc::Storage* storage, const data::EnvLight* env) {
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
    static std::unique_ptr<Session> session;
    if (!session || session->key != key || session->w != width || session->h != height) {
        if (session) vkDeviceWaitIdle(dev);
        session.reset();
        auto s = std::make_unique<Session>();
        s->key = key;
        s->w = width;
        s->h = height;
        SceneRenderer::Tunables tun;
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
        session = std::move(s);
    }
    Session& S = *session;
    SceneRenderer& sr = *S.sr;
    sr.prepare(live, nullptr);

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
    check(vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit(session)");
    check(vkWaitForFences(dev, 1, &fence, VK_TRUE, UINT64_MAX), "vkWaitForFences(session)");

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
