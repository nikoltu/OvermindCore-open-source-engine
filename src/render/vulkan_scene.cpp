#include "sc2/render/scene.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
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
                          const std::string& texture_root, bool enable_particles) {
    std::vector<const SceneInstance*> live;
    for (const auto& inst : instances)
        if (inst.model && inst.model->vertex_count && !inst.model->indices.empty())
            live.push_back(&inst);
    if (live.empty()) throw std::runtime_error("render_scene: no drawable instances");

    Device gpu = Device::create_headless("render_scene");
    const VkPhysicalDevice pd = gpu.physical();
    const VkDevice dev = gpu.device();
    const VkQueue queue = gpu.graphics_queue();
    const uint32_t gfx = gpu.graphics_family();

    SceneRenderer sr(pd, dev, queue, gfx, width, height, VK_FORMAT_R8G8B8A8_UNORM, storage,
                     texture_root);
    sr.prepare(live);

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
    if (enable_particles) build_particles(sr, live, cam, pverts, pgroups);

    VkDeviceSize rbytes = VkDeviceSize(width) * height * 4;
    Buffer readback = make_buffer(pd, dev, rbytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr);

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = gfx;
    VkCommandPool rawCmdPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev, &cpi, nullptr, &rawCmdPool), "vkCreateCommandPool");
    Unique<VkCommandPool> cmdPool(rawCmdPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = rawCmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bbi), "vkBeginCommandBuffer");

    sr.record(cmd, live, cam, pverts, pgroups);

    VkImageMemoryBarrier lb{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
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

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence rawFence = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &fi, nullptr, &rawFence), "vkCreateFence");
    Unique<VkFence> fence(rawFence, [dev](VkFence f) { vkDestroyFence(dev, f, nullptr); });
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(queue, 1, &si, rawFence), "vkQueueSubmit");
    check(vkWaitForFences(dev, 1, &rawFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    RenderResult out;
    out.width = width;
    out.height = height;
    out.mvp = cam.view_proj(float(width) / float(height));
    out.rgba.resize(size_t(rbytes));
    void* mapped = nullptr;
    check(vkMapMemory(dev, readback.mem.get(), 0, rbytes, 0, &mapped), "vkMapMemory(readback)");
    std::memcpy(out.rgba.data(), mapped, size_t(rbytes));
    vkUnmapMemory(dev, readback.mem.get());
    return out;
}

}
