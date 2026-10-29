#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "omc/render/vk_raii.hpp"

namespace omc::render {

inline std::atomic<long long>* gpu_alloc_bytes() {
    static std::atomic<long long> b[2] = {{0}, {0}};
    return b;
}

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
    const VkMemoryPropertyFlags host = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    ai.memoryTypeIndex = find_memory(pd, req.memoryTypeBits, host);
    VkDeviceMemory rawmem = VK_NULL_HANDLE;
    const VkBufferUsageFlags gpu_read = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                                        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    if ((usage & gpu_read) && !(usage & VK_BUFFER_USAGE_TRANSFER_DST_BIT)) {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        VkPhysicalDeviceMemoryProperties mp{};
        vkGetPhysicalDeviceMemoryProperties(pd, &mp);
        const VkMemoryPropertyFlags bar = host | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
        for (uint32_t i = 0; props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU && i < mp.memoryTypeCount; ++i)
            if ((req.memoryTypeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & bar) == bar) {
                VkMemoryAllocateInfo bi_alloc = ai;
                bi_alloc.memoryTypeIndex = i;
                if (vkAllocateMemory(dev, &bi_alloc, nullptr, &rawmem) != VK_SUCCESS) rawmem = VK_NULL_HANDLE;
                break;
            }
    }
    if (rawmem == VK_NULL_HANDLE) check(vkAllocateMemory(dev, &ai, nullptr, &rawmem), "vkAllocateMemory(buffer)");
    gpu_alloc_bytes()[1] += (long long)req.size;
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev, sz = (long long)req.size](VkDeviceMemory m) {
        vkFreeMemory(dev, m, nullptr);
        gpu_alloc_bytes()[1] -= sz;
    });
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
    gpu_alloc_bytes()[0] += (long long)req.size;
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev, sz = (long long)req.size](VkDeviceMemory m) {
        vkFreeMemory(dev, m, nullptr);
        gpu_alloc_bytes()[0] -= sz;
    });
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
    gpu_alloc_bytes()[0] += (long long)req.size;
    out.mem = Unique<VkDeviceMemory>(rawmem, [dev, sz = (long long)req.size](VkDeviceMemory m) {
        vkFreeMemory(dev, m, nullptr);
        gpu_alloc_bytes()[0] -= sz;
    });
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

inline const char* device_type_name(VkPhysicalDeviceType t) {
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return "discrete";
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return "integrated";
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return "virtual";
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return "cpu";
        default: return "other";
    }
}

inline int device_type_rank(VkPhysicalDeviceType t, const char* prefer) {
    if (prefer && *prefer && std::strcmp(prefer, device_type_name(t)) == 0) return -1;
    switch (t) {
        case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU: return 0;
        case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU: return 1;
        case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU: return 2;
        case VK_PHYSICAL_DEVICE_TYPE_CPU: return 4;
        default: return 3;
    }
}

struct RankedDevice {
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t index = 0;
};

inline std::vector<RankedDevice> ranked_physical_devices(VkInstance inst) {
    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(inst, &ndev, nullptr);
    std::vector<VkPhysicalDevice> pds(ndev);
    if (ndev) vkEnumeratePhysicalDevices(inst, &ndev, pds.data());
    pds.resize(ndev);
    const char* prefer = std::getenv("OMC_GPU_PREFER");
    long forced = -1;
    if (const char* s = std::getenv("OMC_GPU_INDEX"); s && *s) {
        char* end = nullptr;
        const long v = std::strtol(s, &end, 10);
        if (end != s && v >= 0 && v < long(ndev)) forced = v;
    }
    std::vector<std::pair<int, RankedDevice>> keyed;
    for (uint32_t i = 0; i < ndev; ++i) {
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(pds[i], &p);
        const int r = long(i) == forced ? -2 : device_type_rank(p.deviceType, prefer);
        keyed.push_back({r, RankedDevice{pds[i], i}});
    }
    std::stable_sort(keyed.begin(), keyed.end(),
                     [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<RankedDevice> out;
    for (const auto& k : keyed) out.push_back(k.second);
    return out;
}

inline void log_chosen_device(VkPhysicalDevice pd, uint32_t index, size_t count) {
    static std::atomic<bool> said{false};
    if (said.exchange(true)) return;
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(pd, &p);
    std::fprintf(stderr, "[render] gpu: %s (%s, index %u of %zu)\n", p.deviceName,
                 device_type_name(p.deviceType), index, count);
    std::fflush(stderr);
}

inline bool pick_present_device(VkInstance inst, VkSurfaceKHR surface, VkPhysicalDevice& out_pd,
                                uint32_t& out_gfx) {
    const std::vector<RankedDevice> ranked = ranked_physical_devices(inst);
    for (const RankedDevice& rd : ranked) {
        VkPhysicalDevice cand = rd.pd;
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
                log_chosen_device(cand, rd.index, ranked.size());
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
