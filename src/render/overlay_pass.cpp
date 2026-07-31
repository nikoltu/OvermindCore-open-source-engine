#include "sc2/render/overlay_pass.hpp"

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstring>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

#include "sc2/render/vk_raii.hpp"

static const uint32_t kOverlayVertSpv[] =
#include "overlay.vert.spv.h"
    ;
static const uint32_t kOverlayFragSpv[] =
#include "overlay.frag.spv.h"
    ;

namespace sc2::render {

namespace {

void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("vulkan: ") + what + " failed (" +
                                 std::to_string(int(r)) + ")");
}

uint32_t find_memory(VkPhysicalDevice pd, uint32_t bits,
                     VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) &&
            (mp.memoryTypes[i].propertyFlags & props) == props)
            return i;
    throw std::runtime_error("vulkan: no suitable memory type");
}

struct Buffer {
    Unique<VkBuffer> buf;
    Unique<VkDeviceMemory> mem;
};

Buffer make_buffer(VkPhysicalDevice pd, VkDevice dev, VkDeviceSize size,
                   VkBufferUsageFlags usage, const void* src) {
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size ? size : 4;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer raw = VK_NULL_HANDLE;
    check(vkCreateBuffer(dev, &bi, nullptr, &raw), "vkCreateBuffer");
    Buffer out;
    out.buf = Unique<VkBuffer>(raw, [dev](VkBuffer b) { vkDestroyBuffer(dev, b, nullptr); });
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, raw, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(pd, req.memoryTypeBits,
                                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                         VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory rawmem = VK_NULL_HANDLE;
    check(vkAllocateMemory(dev, &ai, nullptr, &rawmem), "vkAllocateMemory(buffer)");
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev](VkDeviceMemory m) { vkFreeMemory(dev, m, nullptr); });
    check(vkBindBufferMemory(dev, raw, rawmem, 0), "vkBindBufferMemory");
    if (src && size) {
        void* p = nullptr;
        check(vkMapMemory(dev, rawmem, 0, size, 0, &p), "vkMapMemory");
        std::memcpy(p, src, size_t(size));
        vkUnmapMemory(dev, rawmem);
    }
    return out;
}

struct Image {
    Unique<VkImage> img;
    Unique<VkDeviceMemory> mem;
    Unique<VkImageView> view;
};

Image make_image(VkPhysicalDevice pd, VkDevice dev, VkFormat fmt,
                 VkImageUsageFlags usage, VkImageAspectFlags aspect, uint32_t w,
                 uint32_t h) {
    VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = 1;
    ii.arrayLayers = 1;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage raw = VK_NULL_HANDLE;
    check(vkCreateImage(dev, &ii, nullptr, &raw), "vkCreateImage");
    Image out;
    out.img = Unique<VkImage>(raw, [dev](VkImage i) { vkDestroyImage(dev, i, nullptr); });
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, raw, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex =
        find_memory(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory rawmem = VK_NULL_HANDLE;
    check(vkAllocateMemory(dev, &ai, nullptr, &rawmem), "vkAllocateMemory(image)");
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev](VkDeviceMemory m) { vkFreeMemory(dev, m, nullptr); });
    check(vkBindImageMemory(dev, raw, rawmem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = raw;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, 1, 0, 1};
    VkImageView rawview = VK_NULL_HANDLE;
    check(vkCreateImageView(dev, &vi, nullptr, &rawview), "vkCreateImageView");
    out.view = Unique<VkImageView>(rawview, [dev](VkImageView v) { vkDestroyImageView(dev, v, nullptr); });
    return out;
}

Unique<VkShaderModule> make_shader(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule sm = VK_NULL_HANDLE;
    check(vkCreateShaderModule(dev, &ci, nullptr, &sm), "vkCreateShaderModule");
    return Unique<VkShaderModule>(sm, [dev](VkShaderModule s) { vkDestroyShaderModule(dev, s, nullptr); });
}

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
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
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

    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(w), float(h), 0, 1};
    VkRect2D scis{{0, 0}, {w, h}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1; vps.pViewports = &vp; vps.scissorCount = 1; vps.pScissors = &scis;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL; rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE; rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
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
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1; cb.pAttachments = &cba;

    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
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

std::vector<uint8_t> composite_overlay(const std::vector<uint8_t>& base_rgba,
                                       const std::vector<uint8_t>& overlay_rgba,
                                       uint32_t width, uint32_t height) {
    if (width == 0 || height == 0)
        throw std::runtime_error("composite_overlay: zero-sized frame");
    const size_t need = size_t(width) * height * 4;
    if (base_rgba.size() != need || overlay_rgba.size() != need)
        throw std::runtime_error("composite_overlay: layer size != width*height*4");

    VkInstance rawInst = VK_NULL_HANDLE;
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    check(vkCreateInstance(&ici, nullptr, &rawInst), "vkCreateInstance");
    Unique<VkInstance> instance(rawInst, [](VkInstance i) { vkDestroyInstance(i, nullptr); });

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(rawInst, &ndev, nullptr);
    if (ndev == 0) throw std::runtime_error("composite_overlay: no Vulkan physical device");
    std::vector<VkPhysicalDevice> pds(ndev);
    vkEnumeratePhysicalDevices(rawInst, &ndev, pds.data());
    VkPhysicalDevice pd = pds[0];

    uint32_t nqf = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, nullptr);
    std::vector<VkQueueFamilyProperties> qfs(nqf);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &nqf, qfs.data());
    uint32_t gfx = UINT32_MAX;
    for (uint32_t i = 0; i < nqf; ++i)
        if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { gfx = i; break; }
    if (gfx == UINT32_MAX) throw std::runtime_error("composite_overlay: no graphics queue");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gfx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    VkDevice dev = VK_NULL_HANDLE;
    check(vkCreateDevice(pd, &dci, nullptr, &dev), "vkCreateDevice");
    Unique<VkDevice> device(dev, [](VkDevice d) { vkDestroyDevice(d, nullptr); });
    VkQueue queue;
    vkGetDeviceQueue(dev, gfx, 0, &queue);

    const VkFormat kColor = VK_FORMAT_R8G8B8A8_UNORM;

    Image color = make_image(pd, dev, kColor,
                             VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                             VK_IMAGE_ASPECT_COLOR_BIT, width, height);

    Layer base = make_layer(pd, dev, width, height, base_rgba);
    Layer overlay = make_layer(pd, dev, width, height, overlay_rgba);

    VkSamplerCreateInfo smi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
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
    VkDescriptorSetLayoutCreateInfo texli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    texli.bindingCount = 1;
    texli.pBindings = &texb;
    VkDescriptorSetLayout rawTexLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev, &texli, nullptr, &rawTexLayout), "vkCreateDescriptorSetLayout");
    Unique<VkDescriptorSetLayout> texLayout(rawTexLayout, [dev](VkDescriptorSetLayout l) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr); });

    VkDescriptorPoolSize psize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = 2;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &psize;
    VkDescriptorPool rawPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev, &dpi, nullptr, &rawPool), "vkCreateDescriptorPool");
    Unique<VkDescriptorPool> dpool(rawPool, [dev](VkDescriptorPool p) {
        vkDestroyDescriptorPool(dev, p, nullptr); });

    auto bind_layer = [&](const Layer& t) -> VkDescriptorSet {
        VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsa.descriptorPool = rawPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &rawTexLayout;
        VkDescriptorSet set = VK_NULL_HANDLE;
        check(vkAllocateDescriptorSets(dev, &dsa, &set), "vkAllocateDescriptorSets");
        VkDescriptorImageInfo dii{rawSampler, t.image.view.get(),
                                  VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
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
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 1; rpi.pAttachments = &att;
    rpi.subpassCount = 1; rpi.pSubpasses = &subp;
    VkRenderPass rawPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev, &rpi, nullptr, &rawPass), "vkCreateRenderPass");
    Unique<VkRenderPass> renderPass(rawPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });

    VkImageView fbView = color.view.get();
    VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fbi.renderPass = rawPass; fbi.attachmentCount = 1; fbi.pAttachments = &fbView;
    fbi.width = width; fbi.height = height; fbi.layers = 1;
    VkFramebuffer rawFb = VK_NULL_HANDLE;
    check(vkCreateFramebuffer(dev, &fbi, nullptr, &rawFb), "vkCreateFramebuffer");
    Unique<VkFramebuffer> framebuffer(rawFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });

    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
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

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.queueFamilyIndex = gfx;
    VkCommandPool rawCmdPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev, &cpi, nullptr, &rawCmdPool), "vkCreateCommandPool");
    Unique<VkCommandPool> cmdPool(rawCmdPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });
    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = rawCmdPool; cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY; cbi.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    check(vkBeginCommandBuffer(cmd, &bbi), "vkBeginCommandBuffer");

    record_upload(cmd, base);
    record_upload(cmd, overlay);

    VkClearValue clear;
    clear.color = {{0.0f, 0.0f, 0.0f, 0.0f}};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
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

    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    VkFence rawFence = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &fi, nullptr, &rawFence), "vkCreateFence");
    Unique<VkFence> fence(rawFence, [dev](VkFence f) { vkDestroyFence(dev, f, nullptr); });
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
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
