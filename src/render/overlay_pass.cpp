#include "omc/render/overlay_pass.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "omc/render/device.hpp"
#include "omc/render/vk_helpers.hpp"
#include "omc/render/vk_raii.hpp"

static const uint32_t kOverlayVertSpv[] =
#include "overlay.vert.spv.h"
    ;
static const uint32_t kOverlayFragSpv[] =
#include "overlay.frag.spv.h"
    ;

namespace omc::render {

namespace {

struct Layer {
    Image image;
    Buffer staging;
    uint32_t w = 0, h = 0;
};

Layer make_layer(VkPhysicalDevice pd, VkDevice dev, uint32_t w, uint32_t h,
                 const std::vector<uint8_t>& rgba) {
    Layer t;
    t.w = w;
    t.h = h;
    t.image = make_image(pd, dev, VK_FORMAT_R8G8B8A8_UNORM,
                         VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                         VK_IMAGE_ASPECT_COLOR_BIT, w, h);
    t.staging = make_buffer(pd, dev, rgba.size(),
                            VK_BUFFER_USAGE_TRANSFER_SRC_BIT, rgba.data());
    return t;
}

void record_upload(VkCommandBuffer cmd, const Layer& t) {
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.image = t.image.img.get();
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = 0;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy rgn{};
    rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    rgn.imageExtent = {t.w, t.h, 1};
    vkCmdCopyBufferToImage(cmd, t.staging.buf.get(), t.image.img.get(),
                           VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
}

Unique<VkPipeline> make_overlay_pipeline(VkDevice dev, VkRenderPass pass,
                                         VkPipelineLayout layout, VkShaderModule vs,
                                         VkShaderModule fs, uint32_t w, uint32_t h,
                                         bool blend) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vin{};
    vin.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    VkPipelineInputAssemblyStateCreateInfo ia{};
    ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(w), float(h), 0, 1};
    VkRect2D scis{{0, 0}, {w, h}};
    VkPipelineViewportStateCreateInfo vps{};
    vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scis;
    VkPipelineRasterizationStateCreateInfo rs{};
    rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{};
    ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = blend ? VK_TRUE : VK_FALSE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{};
    cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gpi{};

    gpi.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    gpi.stageCount = 2; gpi.pStages = stages;
    gpi.pVertexInputState = &vin; gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps; gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms; gpi.pColorBlendState = &cb;
    gpi.layout = layout; gpi.renderPass = pass;
    VkPipeline raw = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &raw),
          "vkCreateGraphicsPipelines(overlay)");
    return Unique<VkPipeline>(raw, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });
}

}

std::vector<OverlayRowSpan> overlay_changed_rows(std::vector<uint8_t>& shown, const std::vector<uint8_t>& next,
                                                 uint32_t width, uint32_t height) {
    std::vector<OverlayRowSpan> spans;
    const size_t row = size_t(width) * 4;
    if (!width || !height || next.size() != row * height) return spans;
    if (shown.size() != next.size()) {
        shown = next;
        spans.push_back({0, height});
        return spans;
    }
    constexpr uint32_t kGap = 8;
    constexpr size_t kMaxSpans = 64;
    for (uint32_t y = 0; y < height; ++y) {
        const size_t off = size_t(y) * row;
        if (std::memcmp(shown.data() + off, next.data() + off, row) == 0) continue;
        std::memcpy(shown.data() + off, next.data() + off, row);
        if (!spans.empty() && y <= spans.back().y1 + kGap)
            spans.back().y1 = y + 1;
        else
            spans.push_back({y, y + 1});
    }
    if (spans.size() > kMaxSpans) {
        const OverlayRowSpan all{spans.front().y0, spans.back().y1};
        spans.assign(1, all);
    }
    return spans;
}

bool record_overlay_upload(VkDevice dev, VkCommandBuffer cmd, VkDeviceMemory stage_mem, VkBuffer stage, VkImage image,
                           std::vector<uint8_t>& shown, const std::vector<uint8_t>& next, uint32_t width, uint32_t height,
                           VkImageLayout settled_layout) {
    const bool first = shown.size() != next.size();
    const std::vector<OverlayRowSpan> spans = overlay_changed_rows(shown, next, width, height);
    if (spans.empty()) return false;
    const VkDeviceSize row = VkDeviceSize(width) * 4;
    const VkDeviceSize lo = spans.front().y0 * row, hi = spans.back().y1 * row;
    void* p = nullptr;
    check(vkMapMemory(dev, stage_mem, lo, hi - lo, 0, &p), "vkMapMemory(overlay upload)");
    std::vector<VkBufferImageCopy> rgns;
    rgns.reserve(spans.size());
    for (const OverlayRowSpan& sp : spans) {
        const VkDeviceSize off = sp.y0 * row, len = VkDeviceSize(sp.y1 - sp.y0) * row;
        std::memcpy(static_cast<uint8_t*>(p) + (off - lo), next.data() + off, size_t(len));
        VkBufferImageCopy rgn{};
        rgn.bufferOffset = off;
        rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        rgn.imageOffset = {0, int32_t(sp.y0), 0};
        rgn.imageExtent = {width, sp.y1 - sp.y0, 1};
        rgns.push_back(rgn);
    }
    vkUnmapMemory(dev, stage_mem);
    const bool sampled = settled_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkImageMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    b.oldLayout = first ? VK_IMAGE_LAYOUT_UNDEFINED : settled_layout;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.srcAccessMask = first || sampled ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    const VkPipelineStageFlags src_stage =
        first ? VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT : sampled ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT : VK_PIPELINE_STAGE_TRANSFER_BIT;
    vkCmdPipelineBarrier(cmd, src_stage, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkCmdCopyBufferToImage(cmd, stage, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, uint32_t(rgns.size()), rgns.data());
    if (sampled) {
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    }
    return true;
}

std::vector<uint8_t> composite_overlay(const std::vector<uint8_t>& base_rgba,
                                       const std::vector<uint8_t>& overlay_rgba,
                                       uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        throw std::runtime_error("composite_overlay: zero-sized frame");
    const size_t need = size_t(width) * height * 4;
    if (base_rgba.size() != need || overlay_rgba.size() != need)
        throw std::runtime_error("composite_overlay: layer size != width*height*4");

    Device& gpu = Device::shared_headless("composite_overlay");
    const VkPhysicalDevice pd = gpu.physical();
    const VkDevice dev = gpu.device();
    const VkQueue queue = gpu.graphics_queue();
    const uint32_t gfx = gpu.graphics_family();

    const VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM;

    Image color = make_image(pd, dev, kColor,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, width, height);

    Layer base = make_layer(pd, dev, width, height, base_rgba);
    Layer overlay = make_layer(pd, dev, width, height, overlay_rgba);

    VkSamplerCreateInfo smi{};
    smi.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    smi.magFilter = smi.minFilter = VK_FILTER_NEAREST;
    smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    smi.maxLod = 0.0f;
    VkSampler rawSampler = VK_NULL_HANDLE;
    check(vkCreateSampler(dev, &smi, nullptr, &rawSampler), "vkCreateSampler");
    Unique<VkSampler> sampler(rawSampler, [dev](VkSampler s) { vkDestroySampler(dev, s, nullptr); });

    VkDescriptorSetLayoutBinding texb{};
    texb.binding = 0;
    texb.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    texb.descriptorCount = 1;
    texb.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo texli{};
    texli.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    texli.bindingCount = 1;
    texli.pBindings = &texb;
    VkDescriptorSetLayout rawTexLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev, &texli, nullptr, &rawTexLayout), "vkCreateDescriptorSetLayout");
    Unique<VkDescriptorSetLayout> texLayout(rawTexLayout, [dev](VkDescriptorSetLayout l) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpi{};
    dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &psize;
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev, &dpi, nullptr, &rawPool), "vkCreateDescriptorPool");
    Unique<VkDescriptorPool> dpool(rawPool, [dev](VkDescriptorPool p) {
        vkDestroyDescriptorPool(dev, p, nullptr); });

    auto bind_layer = [&](const Layer& t) -> VkDescriptorSet {
        VkDescriptorSetAllocateInfo dsa{};
        dsa.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsa.descriptorPool = rawPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &rawTexLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        check(vkAllocateDescriptorSets(dev, &dsa, &set), "vkAllocateDescriptorSets");
        VkDescriptorImageInfo dii{rawSampler, t.image.view.get(),
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{};
        w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w.dstSet = set;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
        return set;
    };
    VkDescriptorSet base_set = bind_layer(base);
    VkDescriptorSet overlay_set = bind_layer(overlay);

    VkAttachmentDescription att{};
    att.format = kColor;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subp{};
    subp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subp.colorAttachmentCount = 1;
    subp.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo rpi{};
    rpi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &subp;
    VkRenderPass rawPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev, &rpi, nullptr, &rawPass), "vkCreateRenderPass");
    Unique<VkRenderPass> renderPass(rawPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });

    VkImageView fbView = color.view.get();
    VkFramebufferCreateInfo fbi{};
    fbi.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fbi.renderPass = rawPass; fbi.attachmentCount = 1; fbi.pAttachments = &fbView;
    fbi.width = width; fbi.height = height; fbi.layers = 1;
    VkFramebuffer rawFb = VK_NULL_HANDLE;
    check(vkCreateFramebuffer(dev, &fbi, nullptr, &rawFb), "vkCreateFramebuffer");
    Unique<VkFramebuffer> framebuffer(rawFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });

    VkPipelineLayoutCreateInfo pli{};
    pli.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &rawTexLayout;
    VkPipelineLayout rawLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(dev, &pli, nullptr, &rawLayout), "vkCreatePipelineLayout");
    Unique<VkPipelineLayout> layout(rawLayout, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });

    Unique<VkShaderModule> vs = make_shader(dev, kOverlayVertSpv, sizeof(kOverlayVertSpv));
    Unique<VkShaderModule> fs = make_shader(dev, kOverlayFragSpv, sizeof(kOverlayFragSpv));
    Unique<VkPipeline> basePipe = make_overlay_pipeline(dev, rawPass, rawLayout, vs.get(),
                                                        fs.get(), width, height, false);
    Unique<VkPipeline> overlayPipe = make_overlay_pipeline(dev, rawPass, rawLayout, vs.get(),
                                                           fs.get(), width, height, true);

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
    cbi.commandPool = rawCmdPool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bbi{};
    bbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bbi), "vkBeginCommandBuffer");

    record_upload(cmd, base);
    record_upload(cmd, overlay);

    VkClearValue clear;
    clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rawPass; rbi.framebuffer = rawFb;
    rbi.renderArea = {{0, 0}, {width, height}};
    rbi.clearValueCount = 1; rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, basePipe.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawLayout, 0, 1, &base_set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, overlayPipe.get());
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawLayout, 0, 1, &overlay_set, 0, nullptr);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    vkCmdEndRenderPass(cmd);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {width, height, 1};
    vkCmdCopyImageToBuffer(cmd, color.img.get(), VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                           readback.buf.get(), 1, &region);
    check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

    VkFenceCreateInfo fi{};

    fi.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence rawFence = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &fi, nullptr, &rawFence), "vkCreateFence");
    Unique<VkFence> fence(rawFence, [dev](VkFence f) { vkDestroyFence(dev, f, nullptr); });
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1; si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(queue, 1, &si, rawFence), "vkQueueSubmit");
    check(vkWaitForFences(dev, 1, &rawFence, VK_TRUE, UINT64_MAX), "vkWaitForFences");

    std::vector<uint8_t> out;
    out.resize(size_t(rbytes));
    void* mapped = nullptr;
    check(vkMapMemory(dev, readback.mem.get(), 0, rbytes, 0, &mapped), "vkMapMemory(readback)");
    std::memcpy(out.data(), mapped, size_t(rbytes));
    vkUnmapMemory(dev, readback.mem.get());
    return out;
}

}
