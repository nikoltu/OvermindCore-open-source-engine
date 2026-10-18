#pragma once

#include <vulkan/vulkan.h>

#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

#include "sc2/render/vk_raii.hpp"

namespace sc2::render {

inline void check(VkResult r, const char* what) {
    if (r != VK_SUCCESS)
        throw std::runtime_error(std::string("vulkan: ") + what + " failed (" +
                                 std::to_string(int(r)) + ")");
}

inline uint32_t find_memory(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props) return i;
    throw std::runtime_error("vulkan: no suitable memory type");
}

struct Buffer {
    Unique<VkBuffer> buf;
    Unique<VkDeviceMemory> mem;
};

template <class T>
inline T vk_init(VkStructureType type) {
    T s{};
    s.sType = type;
    return s;
}

inline Buffer make_buffer(VkPhysicalDevice pd, VkDevice dev, VkDeviceSize size, VkBufferUsageFlags usage,
                          const void* src) {
    VkBufferCreateInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = size ? size : 4;
    bi.usage = usage;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer raw = VK_NULL_HANDLE;
    check(vkCreateBuffer(dev, &bi, nullptr, &raw), "vkCreateBuffer");
    Buffer out;
    out.buf = Unique<VkBuffer>(raw, [dev](VkBuffer b) { vkDestroyBuffer(dev, b, nullptr); });
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(dev, raw, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(
        pd, req.memoryTypeBits,
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
    Image() = default;
    Image(Image&&) noexcept = default;
    Image& operator=(Image&& o) noexcept {
        if (this != &o) {
            view.reset();
            img.reset();
            mem.reset();
            img = std::move(o.img);
            mem = std::move(o.mem);
            view = std::move(o.view);
        }
        return *this;
    }
};

inline Image make_image(VkPhysicalDevice pd, VkDevice dev, VkFormat fmt, VkImageUsageFlags usage,
                        VkImageAspectFlags aspect, uint32_t w, uint32_t h, uint32_t mips = 1) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = mips;
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
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory rawmem = VK_NULL_HANDLE;
    check(vkAllocateMemory(dev, &ai, nullptr, &rawmem), "vkAllocateMemory(image)");
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev](VkDeviceMemory m) { vkFreeMemory(dev, m, nullptr); });
    check(vkBindImageMemory(dev, raw, rawmem, 0), "vkBindImageMemory");
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = raw;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = fmt;
    vi.subresourceRange = {aspect, 0, mips, 0, 1};
    VkImageView rawview = VK_NULL_HANDLE;
    check(vkCreateImageView(dev, &vi, nullptr, &rawview), "vkCreateImageView");
    out.view = Unique<VkImageView>(rawview, [dev](VkImageView v) { vkDestroyImageView(dev, v, nullptr); });
    return out;
}

inline Image make_image_array(VkPhysicalDevice pd, VkDevice dev, VkFormat fmt,
                              VkImageUsageFlags usage, uint32_t w, uint32_t h, uint32_t layers,
                              uint32_t mips = 1) {
    VkImageCreateInfo ii{};
    ii.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ii.imageType = VK_IMAGE_TYPE_2D;
    ii.format = fmt;
    ii.extent = {w, h, 1};
    ii.mipLevels = mips;
    ii.arrayLayers = layers;
    ii.samples = VK_SAMPLE_COUNT_1_BIT;
    ii.tiling = VK_IMAGE_TILING_OPTIMAL;
    ii.usage = usage;
    ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImage raw = VK_NULL_HANDLE;
    check(vkCreateImage(dev, &ii, nullptr, &raw), "vkCreateImage(array)");
    Image out;
    out.img = Unique<VkImage>(raw, [dev](VkImage i) { vkDestroyImage(dev, i, nullptr); });
    VkMemoryRequirements req;
    vkGetImageMemoryRequirements(dev, raw, &req);
    VkMemoryAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = find_memory(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    VkDeviceMemory rawmem = VK_NULL_HANDLE;
    check(vkAllocateMemory(dev, &ai, nullptr, &rawmem), "vkAllocateMemory(image array)");
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev](VkDeviceMemory m) { vkFreeMemory(dev, m, nullptr); });
    check(vkBindImageMemory(dev, raw, rawmem, 0), "vkBindImageMemory(array)");
    VkImageViewCreateInfo vi{};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = raw;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = fmt;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, layers};
    VkImageView rawview = VK_NULL_HANDLE;
    check(vkCreateImageView(dev, &vi, nullptr, &rawview), "vkCreateImageView(array)");
    out.view = Unique<VkImageView>(rawview, [dev](VkImageView v) { vkDestroyImageView(dev, v, nullptr); });
    return out;
}

inline bool pick_present_device(VkInstance inst, VkSurfaceKHR surface, VkPhysicalDevice& out_pd,
                                uint32_t& out_gfx) {
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, nullptr);
    if (ndev == 0) return false;
    std::vector<VkPhysicalDevice> pds(ndev);
    vkEnumeratePhysicalDevices(inst, &ndev, pds.data());
    for (VkPhysicalDevice cand : pds) {
        uint32_t nde = 0;
        vkEnumerateDeviceExtensionProperties(cand, nullptr, &nde, nullptr);
        std::vector<VkExtensionProperties> de(nde);
        if (nde) vkEnumerateDeviceExtensionProperties(cand, nullptr, &nde, de.data());
        bool has_swap = false;
        for (const auto& e : de)
            if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
                has_swap = true;
                break;
            }
        if (!has_swap) continue;
        uint32_t nqf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &nqf, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(nqf);
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &nqf, qfs.data());
        for (uint32_t i = 0; i < nqf; ++i) {
            if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(cand, i, surface, &present);
            if (present) {
                out_pd = cand;
                out_gfx = i;
                return true;
            }
        }
    }
    return false;
}

inline Unique<VkShaderModule> make_shader(VkDevice dev, const uint32_t* code, size_t bytes) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = bytes;
    ci.pCode = code;
    VkShaderModule sm = VK_NULL_HANDLE;
    check(vkCreateShaderModule(dev, &ci, nullptr, &sm), "vkCreateShaderModule");
    return Unique<VkShaderModule>(sm, [dev](VkShaderModule s) { vkDestroyShaderModule(dev, s, nullptr); });
}

}
