#include "omc/render/game_window.hpp"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <cstring>
#include <unordered_map>
#include <vector>

#include "omc/casc/storage.hpp"
#include "omc/render/device.hpp"
#include "omc/render/frame_prof.hpp"
#include "omc/render/overlay_pass.hpp"
#include "omc/render/quality.hpp"
#include "omc/render/scene_renderer.hpp"
#include "omc/render/texture_decode.hpp"
#include "omc/render/vk_helpers.hpp"
#include "omc/render/vk_raii.hpp"

namespace omc::render {

WindowResult run_game_offscreen(const GameFrameFn& frame, const casc::Storage* storage, uint32_t width, uint32_t height, uint32_t frames,
                                const std::function<data::EnvLight()>& env_provider, float shadow_clip,
                                const std::function<const FogField*()>& fog_provider, const OffscreenDumpFn& dump_last_frame,
                                const std::function<bool()>& dump_this_frame) {
    Device& gpu = Device::shared_headless("game_offscreen");
    const VkPhysicalDevice pd = gpu.physical();
    const VkDevice dev = gpu.device();
    const VkQueue queue = gpu.graphics_queue();
    const uint32_t gfx = gpu.graphics_family();
    const VkFormat target_format = VK_FORMAT_B8G8R8A8_UNORM;

    SceneRenderer::Tunables tun;
    tun.camera_fit_shadow = true;
    tun.shadow_clip = shadow_clip;
    tun.clear_color[0] = tun.clear_color[1] = tun.clear_color[2] = 0.0f;
    tun.gpu_timing = gpu_prof_switch() ? 1 : 0;
    tun.live_quality = true;
    SceneRenderer sr(pd, dev, queue, gfx, scaled_extent(width, render_quality().scale_pct), scaled_extent(height, render_quality().scale_pct),
                     target_format, storage, "", tun);

    const VkImageUsageFlags dump_usage = dump_last_frame ? VK_IMAGE_USAGE_TRANSFER_SRC_BIT : 0;
    Image target = make_image(pd, dev, target_format, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | dump_usage,
                              VK_IMAGE_ASPECT_COLOR_BIT, width, height);
    Image ovl_tex = make_image(pd, dev, VK_FORMAT_R8G8B8A8_UNORM, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | dump_usage,
                               VK_IMAGE_ASPECT_COLOR_BIT, width, height);
    Buffer dump_buf;
    if (dump_last_frame)
        dump_buf = make_buffer(pd, dev, VkDeviceSize(width) * height * 8, VK_BUFFER_USAGE_TRANSFER_DST_BIT, nullptr);
    bool ovl_uploaded = false;
    std::vector<uint8_t> ovl_shown;
    Buffer ovl_stage = make_buffer(pd, dev, VkDeviceSize(width) * height * 4, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr);

    VkCommandPoolCreateInfo cpi{};
    cpi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = gfx;
    VkCommandPool raw_pool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev, &cpi, nullptr, &raw_pool), "vkCreateCommandPool(game_offscreen)");
    Unique<VkCommandPool> pool(raw_pool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });
    VkCommandBufferAllocateInfo cbi{};
    cbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cbi.commandPool = raw_pool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers(game_offscreen)");
    VkFenceCreateInfo fi{};
    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence raw_fence = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &fi, nullptr, &raw_fence), "vkCreateFence(game_offscreen)");
    Unique<VkFence> fence(raw_fence, [dev](VkFence f) { vkDestroyFence(dev, f, nullptr); });

    WindowResult result;
    result.created = true;
    result.width = width;
    result.height = height;
    std::vector<SceneInstance> scene;
    std::vector<uint8_t> overlay;
    Camera cam;
    for (uint32_t f = 0; f < frames; ++f) {
        OMC_PROF_SCOPE("loop_iter");
        InputState in;
        in.view_w = width;
        in.view_h = height;
        in.mouse_x = float(width) * 0.5f;
        in.mouse_y = float(height) * 0.4f;
        scene.clear();
        overlay.clear();
        bool cont = true;
        { OMC_PROF_SCOPE("frame_cb"); cont = frame(in, 1000.0f / 60.0f, scene, cam, overlay); }
        const bool has_overlay = overlay.size() == size_t(width) * size_t(height) * 4;
        if (env_provider) sr.set_environment(env_provider());
        { OMC_PROF_SCOPE("fence_wait");
          vkWaitForFences(dev, 1, &raw_fence, VK_TRUE, UINT64_MAX); }
        OMC_PROF_SCOPE("submit_half");
        sr.resize(scaled_extent(width, render_quality().scale_pct), scaled_extent(height, render_quality().scale_pct));
        if (fog_provider) {
            const FogField* fog = fog_provider();
            sr.set_fog(fog ? *fog : FogField{});
        }
        std::vector<const SceneInstance*> live;
        for (const SceneInstance& s : scene)
            if (s.model && s.model->vertex_count && !s.model->indices.empty()) live.push_back(&s);
        std::unordered_map<const formats::Model*, bool> model_ready;
        auto ready_to_draw = [&](const formats::Model* m) {
            if (!world_async_loads() || !storage || f == 0 || sr.has_mesh(m)) return true;
            auto it = model_ready.find(m);
            if (it != model_ready.end()) return it->second;
            const bool r = SceneRenderer::request_model_textures(*m, storage, "", 0);
            model_ready.emplace(m, r);
            return r;
        };
        live.erase(std::remove_if(live.begin(), live.end(), [&](const SceneInstance* s) { return !ready_to_draw(s->model); }), live.end());
        std::vector<const SceneInstance*> light_only;
        for (const SceneInstance& s : scene)
            if (s.model && !(s.model->vertex_count && !s.model->indices.empty()) && !s.model->lights.empty()) light_only.push_back(&s);
        { OMC_PROF_SCOPE("prepare"); sr.prepare(live, &light_only); }
        std::vector<SceneRenderer::ParticleVertex> pverts;
        std::vector<SceneRenderer::ParticleGroup> pgroups;
        std::vector<const SceneInstance*> emitting = live;
        for (const SceneInstance& s : scene)
            if (s.model && !(s.model->vertex_count && !s.model->indices.empty()) && (!s.model->particles.empty() || !s.model->ribbons.empty()) &&
                ready_to_draw(s.model))
                emitting.push_back(&s);
        { OMC_PROF_SCOPE("particles"); build_particles(sr, emitting, cam, pverts, pgroups); }

        vkResetFences(dev, 1, &raw_fence);
        check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer(game_offscreen)");
        VkCommandBufferBeginInfo bbi{};
        bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        check(vkBeginCommandBuffer(cmd, &bbi), "vkBeginCommandBuffer(game_offscreen)");
        sr.record(cmd, live, cam, pverts, pgroups);

        VkImageMemoryBarrier lb{};
        lb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        lb.oldLayout = lb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        lb.srcQueueFamilyIndex = lb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        lb.image = sr.ldr_image();
        lb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        lb.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        lb.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &lb);
        VkImageMemoryBarrier tb{};
        tb.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        tb.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        tb.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        tb.srcQueueFamilyIndex = tb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        tb.image = target.img.get();
        tb.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        tb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &tb);
        VkImageBlit bl{};
        bl.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.srcOffsets[1] = {int32_t(sr.width()), int32_t(sr.height()), 1};
        bl.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        bl.dstOffsets[1] = {int32_t(width), int32_t(height), 1};
        vkCmdBlitImage(cmd, sr.ldr_image(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.img.get(), VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl,
                       VK_FILTER_LINEAR);
        sr.gpu_timer().mark(cmd, "gpu.blit");
        if (has_overlay &&
            record_overlay_upload(dev, cmd, ovl_stage.mem.get(), ovl_stage.buf.get(), ovl_tex.img.get(), ovl_shown, overlay, width, height,
                                  VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL))
            ovl_uploaded = true;
        sr.gpu_timer().mark(cmd, "gpu.hud");
        const bool dump_now = dump_last_frame && (dump_this_frame ? dump_this_frame() : (f + 1 == frames || !cont));
        if (dump_now) {
            VkImageMemoryBarrier db[2]{};
            for (VkImageMemoryBarrier& b : db) {
                b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
                b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            }
            db[0].image = target.img.get();
            db[1].image = ovl_tex.img.get();
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, ovl_uploaded ? 2u : 1u, db);
            VkBufferImageCopy rc{};
            rc.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rc.imageExtent = {width, height, 1};
            vkCmdCopyImageToBuffer(cmd, target.img.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump_buf.buf.get(), 1, &rc);
            if (ovl_uploaded) {
                rc.bufferOffset = VkDeviceSize(width) * height * 4;
                vkCmdCopyImageToBuffer(cmd, ovl_tex.img.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, dump_buf.buf.get(), 1, &rc);
            }
        }
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer(game_offscreen)");
        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        check(vkQueueSubmit(queue, 1, &si, raw_fence), "vkQueueSubmit(game_offscreen)");
        ++result.frames_presented;
        frame_prof_tick();
        if (dump_now) {
            vkWaitForFences(dev, 1, &raw_fence, VK_TRUE, UINT64_MAX);
            const size_t n = size_t(width) * height * 4;
            std::vector<uint8_t> scene_rgba(n), ovl_rgba(ovl_uploaded ? n : 0);
            void* p = nullptr;
            check(vkMapMemory(dev, dump_buf.mem.get(), 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory(game_offscreen dump)");
            const uint8_t* src = static_cast<const uint8_t*>(p);
            for (size_t i = 0; i < n; i += 4) {
                scene_rgba[i] = src[i + 2];
                scene_rgba[i + 1] = src[i + 1];
                scene_rgba[i + 2] = src[i];
                scene_rgba[i + 3] = src[i + 3];
            }
            if (ovl_uploaded) std::memcpy(ovl_rgba.data(), src + n, n);
            vkUnmapMemory(dev, dump_buf.mem.get());
            dump_last_frame(width, height, scene_rgba, ovl_rgba, overlay);
        }
        if (!cont) break;
    }
    vkWaitForFences(dev, 1, &raw_fence, VK_TRUE, UINT64_MAX);
    sr.gpu_timer().collect();
    vkDeviceWaitIdle(dev);
    return result;
}

}
