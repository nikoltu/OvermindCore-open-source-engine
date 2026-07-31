#include "sc2/render/game_window.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#include <windows.h>
#include <windowsx.h>

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/dds.hpp"
#include "sc2/render/anim.hpp"
#include "sc2/render/vk_raii.hpp"
#include "mesh_build.hpp"

static const uint32_t kMeshVertSpv[] =
#include "mesh.vert.spv.h"
    ;
static const uint32_t kMeshFragSpv[] =
#include "mesh.frag.spv.h"
    ;
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

[[noreturn]] void unavailable(const char* what) {
    throw WindowUnavailable(std::string("run_game_window: ") + what);
}

uint32_t find_memory(VkPhysicalDevice pd, uint32_t bits, VkMemoryPropertyFlags props) {
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(pd, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & props) == props)
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

Image make_image(VkPhysicalDevice pd, VkDevice dev, VkFormat fmt, VkImageUsageFlags usage,
                 VkImageAspectFlags aspect, uint32_t w, uint32_t h) {
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
    ai.memoryTypeIndex = find_memory(pd, req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
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

struct TexData {
    uint32_t w, h;
    std::vector<uint8_t> rgba;
};

enum TexSlot { kDiffuse = 0, kNormal = 1, kEmissive = 2, kNumSlots = 3 };

struct Mesh {
    Buffer vbuf;
    Buffer ibuf;
    std::vector<Draw> draws;
    std::vector<std::array<int, kNumSlots>> draw_tex;
    std::vector<VkDescriptorSet> draw_set;
};

bool has_layer(const std::vector<VkLayerProperties>& layers, const char* name) {
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}
bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
                                        VkDebugUtilsMessageTypeFlagsEXT,
                                        const VkDebugUtilsMessengerCallbackDataEXT* data,
                                        void* user) {
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        if (user) ++*static_cast<uint32_t*>(user);
        std::fprintf(stderr, "[vk-validation] %s\n",
                     data && data->pMessage ? data->pMessage : "(no message)");
    }
    return VK_FALSE;
}

struct InputAccum {
    float mouse_x = 0, mouse_y = 0;
    bool left = false, right = false, middle = false, shift = false;
    bool pan_left = false, pan_right = false, pan_up = false, pan_down = false;
    bool orbit_left = false, orbit_right = false;
    bool train = false;
    bool build = false;
    bool debug = false;
    bool hold = false;
    bool stop = false;
    bool patrol = false;
    bool ctrl = false;
    bool num[10] = {};
    float wheel = 0;
    bool quit = false;
};

LRESULT CALLBACK game_wndproc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    auto* in = reinterpret_cast<InputAccum*>(GetWindowLongPtrW(h, GWLP_USERDATA));
    switch (msg) {
        case WM_CLOSE:
            if (in) in->quit = true;
            PostQuitMessage(0);
            return 0;
        case WM_MOUSEMOVE:
            if (in) { in->mouse_x = float(GET_X_LPARAM(lp)); in->mouse_y = float(GET_Y_LPARAM(lp)); }
            return 0;
        case WM_LBUTTONDOWN: if (in) in->left = true; return 0;
        case WM_LBUTTONUP:   if (in) in->left = false; return 0;
        case WM_RBUTTONDOWN: if (in) in->right = true; return 0;
        case WM_RBUTTONUP:   if (in) in->right = false; return 0;
        case WM_MBUTTONDOWN: if (in) in->middle = true; return 0;
        case WM_MBUTTONUP:   if (in) in->middle = false; return 0;
        case WM_MOUSEWHEEL:
            if (in) in->wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / 120.0f;
            return 0;
        case WM_KEYDOWN:
        case WM_KEYUP: {
            const bool down = (msg == WM_KEYDOWN);
            if (in) switch (wp) {
                case 'W': case VK_UP: in->pan_up = down; break;
                case 'S': case VK_DOWN: in->pan_down = down; break;
                case 'A': case VK_LEFT: in->pan_left = down; break;
                case 'D': case VK_RIGHT: in->pan_right = down; break;
                case 'Q': in->orbit_left = down; break;
                case 'E': in->orbit_right = down; break;
                case 'T': in->train = down; break;
                case 'B': in->build = down; break;
                case 'H': in->hold = down; break;
                case 'X': in->stop = down; break;
                case 'P': in->patrol = down; break;
                case VK_F4: in->debug = down; break;
                case VK_CONTROL: in->ctrl = down; break;
                case VK_SHIFT: in->shift = down; break;
                case VK_ESCAPE: if (down) in->quit = true; break;
                default:
                    if (wp >= '0' && wp <= '9') in->num[wp - '0'] = down;
                    break;
            }
            return 0;
        }
    }
    return DefWindowProcW(h, msg, wp, lp);
}

const wchar_t* game_window_class(HINSTANCE hinst) {
    static const wchar_t* kName = L"sc2_game_window";
    static bool registered = false;
    if (!registered) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.style = CS_HREDRAW | CS_VREDRAW;
        wc.lpfnWndProc = game_wndproc;
        wc.hInstance = hinst;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.lpszClassName = kName;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
            unavailable("RegisterClassEx failed");
        registered = true;
    }
    return kName;
}

}

WindowResult run_game_window(const GameFrameFn& frame, const casc::Storage* storage,
                             uint32_t width, uint32_t height,
                             const std::string& texture_root, uint32_t max_frames) {
    HINSTANCE hinst = GetModuleHandleW(nullptr);
    const wchar_t* cls = game_window_class(hinst);
    RECT rc{0, 0, LONG(width), LONG(height)};
    AdjustWindowRect(&rc, WS_OVERLAPPEDWINDOW, FALSE);
    HWND rawWin = CreateWindowExW(0, cls, L"sc2 game", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT,
                                  CW_USEDEFAULT, rc.right - rc.left, rc.bottom - rc.top,
                                  nullptr, nullptr, hinst, nullptr);
    if (!rawWin) unavailable("CreateWindowEx failed");
    Unique<HWND> window(rawWin, [](HWND h) { DestroyWindow(h); });
    InputAccum accum;
    accum.mouse_x = float(width) / 2.0f;
    accum.mouse_y = float(height) / 2.0f;
    SetWindowLongPtrW(rawWin, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(&accum));
    ShowWindow(rawWin, SW_SHOWNA);

    uint32_t nlayers = 0;
    vkEnumerateInstanceLayerProperties(&nlayers, nullptr);
    std::vector<VkLayerProperties> layers(nlayers);
    if (nlayers) vkEnumerateInstanceLayerProperties(&nlayers, layers.data());
    uint32_t niexts = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &niexts, nullptr);
    std::vector<VkExtensionProperties> iexts(niexts);
    if (niexts) vkEnumerateInstanceExtensionProperties(nullptr, &niexts, iexts.data());
    if (!has_ext(iexts, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !has_ext(iexts, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
        unavailable("win32 surface instance extensions unavailable");

    std::vector<const char*> want_ext = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> want_layer;
    const bool validation = has_layer(layers, "VK_LAYER_KHRONOS_validation") &&
                            has_ext(iexts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation) {
        want_layer.push_back("VK_LAYER_KHRONOS_validation");
        want_ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }
    VkApplicationInfo appi{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    appi.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &appi;
    ici.enabledExtensionCount = uint32_t(want_ext.size());
    ici.ppEnabledExtensionNames = want_ext.data();
    ici.enabledLayerCount = uint32_t(want_layer.size());
    ici.ppEnabledLayerNames = want_layer.empty() ? nullptr : want_layer.data();
    VkInstance rawInst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &rawInst) != VK_SUCCESS)
        unavailable("vkCreateInstance (WSI) failed");
    Unique<VkInstance> instance(rawInst, [](VkInstance i) { vkDestroyInstance(i, nullptr); });

    uint32_t val_errors = 0;
    Unique<VkDebugUtilsMessengerEXT> messenger;
    if (validation) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(rawInst, "vkCreateDebugUtilsMessengerEXT"));
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(rawInst, "vkDestroyDebugUtilsMessengerEXT"));
        if (create && destroy) {
            VkDebugUtilsMessengerCreateInfoEXT mi{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = debug_cb;
            mi.pUserData = &val_errors;
            VkDebugUtilsMessengerEXT raw = VK_NULL_HANDLE;
            if (create(rawInst, &mi, nullptr, &raw) == VK_SUCCESS)
                messenger = Unique<VkDebugUtilsMessengerEXT>(raw, [rawInst, destroy](VkDebugUtilsMessengerEXT m) {
                    destroy(rawInst, m, nullptr);
                });
        }
    }

    VkWin32SurfaceCreateInfoKHR wsci{VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR};
    wsci.hinstance = hinst;
    wsci.hwnd = rawWin;
    VkSurfaceKHR rawSurf = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(rawInst, &wsci, nullptr, &rawSurf) != VK_SUCCESS)
        unavailable("vkCreateWin32SurfaceKHR failed");
    Unique<VkSurfaceKHR> surface(rawSurf, [rawInst](VkSurfaceKHR s) { vkDestroySurfaceKHR(rawInst, s, nullptr); });

    uint32_t ndev = 0;
    vkEnumeratePhysicalDevices(rawInst, &ndev, nullptr);
    if (ndev == 0) unavailable("no Vulkan physical device");
    std::vector<VkPhysicalDevice> pds(ndev);
    vkEnumeratePhysicalDevices(rawInst, &ndev, pds.data());
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t gfx = UINT32_MAX;
    for (VkPhysicalDevice cand : pds) {
        uint32_t nde = 0;
        vkEnumerateDeviceExtensionProperties(cand, nullptr, &nde, nullptr);
        std::vector<VkExtensionProperties> de(nde);
        if (nde) vkEnumerateDeviceExtensionProperties(cand, nullptr, &nde, de.data());
        if (!has_ext(de, VK_KHR_SWAPCHAIN_EXTENSION_NAME)) continue;
        uint32_t nqf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &nqf, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(nqf);
        vkGetPhysicalDeviceQueueFamilyProperties(cand, &nqf, qfs.data());
        for (uint32_t i = 0; i < nqf; ++i) {
            if (!(qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) continue;
            VkBool32 present = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(cand, i, rawSurf, &present);
            if (present) { pd = cand; gfx = i; break; }
        }
        if (pd != VK_NULL_HANDLE) break;
    }
    if (pd == VK_NULL_HANDLE) unavailable("no graphics+present queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = gfx;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devext;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(pd, &dci, nullptr, &dev) != VK_SUCCESS)
        unavailable("vkCreateDevice (swapchain) failed");
    Unique<VkDevice> device(dev, [](VkDevice d) { vkDestroyDevice(d, nullptr); });
    VkQueue queue;
    vkGetDeviceQueue(dev, gfx, 0, &queue);

    VkSurfaceCapabilitiesKHR caps{};
    check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(pd, rawSurf, &caps),
          "vkGetPhysicalDeviceSurfaceCapabilitiesKHR");
    uint32_t nfmt = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, rawSurf, &nfmt, nullptr);
    if (nfmt == 0) unavailable("surface reports no formats");
    std::vector<VkSurfaceFormatKHR> fmts(nfmt);
    vkGetPhysicalDeviceSurfaceFormatsKHR(pd, rawSurf, &nfmt, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (const auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM && f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR) {
            fmt = f;
            break;
        }
    VkExtent2D extent = caps.currentExtent;
    if (extent.width == 0xFFFFFFFFu) {
        extent.width = std::clamp(width, caps.minImageExtent.width, caps.maxImageExtent.width);
        extent.height = std::clamp(height, caps.minImageExtent.height, caps.maxImageExtent.height);
    }
    if (extent.width == 0 || extent.height == 0) unavailable("surface extent is zero (minimized)");
    width = extent.width;
    height = extent.height;

    uint32_t want_images = caps.minImageCount + 1;
    if (caps.maxImageCount > 0 && want_images > caps.maxImageCount) want_images = caps.maxImageCount;
    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = rawSurf;
    sci.minImageCount = want_images;
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VkSwapchainKHR rawSwap = VK_NULL_HANDLE;
    if (vkCreateSwapchainKHR(dev, &sci, nullptr, &rawSwap) != VK_SUCCESS)
        unavailable("vkCreateSwapchainKHR failed");
    Unique<VkSwapchainKHR> swapchain(rawSwap, [dev](VkSwapchainKHR s) { vkDestroySwapchainKHR(dev, s, nullptr); });

    uint32_t nimg = 0;
    vkGetSwapchainImagesKHR(dev, rawSwap, &nimg, nullptr);
    std::vector<VkImage> swap_images(nimg);
    vkGetSwapchainImagesKHR(dev, rawSwap, &nimg, swap_images.data());

    const VkFormat kDepth = VK_FORMAT_D32_SFLOAT;
    std::vector<Unique<VkImageView>> swap_views(nimg);
    std::vector<Image> depths(nimg);
    for (uint32_t i = 0; i < nimg; ++i) {
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = swap_images[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = fmt.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView rawv = VK_NULL_HANDLE;
        check(vkCreateImageView(dev, &vi, nullptr, &rawv), "vkCreateImageView(swapchain)");
        swap_views[i] = Unique<VkImageView>(rawv, [dev](VkImageView v) { vkDestroyImageView(dev, v, nullptr); });
        depths[i] = make_image(pd, dev, kDepth, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT,
                               VK_IMAGE_ASPECT_DEPTH_BIT, width, height);
    }

    VkCommandPoolCreateInfo cpi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    cpi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    cpi.queueFamilyIndex = gfx;
    VkCommandPool rawCmdPool = VK_NULL_HANDLE;
    check(vkCreateCommandPool(dev, &cpi, nullptr, &rawCmdPool), "vkCreateCommandPool");
    Unique<VkCommandPool> cmdPool(rawCmdPool, [dev](VkCommandPool p) { vkDestroyCommandPool(dev, p, nullptr); });

    auto one_time = [&](const std::function<void(VkCommandBuffer)>& rec) {
        VkCommandBufferAllocateInfo ab{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
        ab.commandPool = rawCmdPool;
        ab.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        ab.commandBufferCount = 1;
        VkCommandBuffer cb;
        check(vkAllocateCommandBuffers(dev, &ab, &cb), "vkAllocateCommandBuffers(one_time)");
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        check(vkBeginCommandBuffer(cb, &bi), "vkBeginCommandBuffer(one_time)");
        rec(cb);
        check(vkEndCommandBuffer(cb), "vkEndCommandBuffer(one_time)");
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cb;
        check(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit(one_time)");
        check(vkQueueWaitIdle(queue), "vkQueueWaitIdle(one_time)");
        vkFreeCommandBuffers(dev, rawCmdPool, 1, &cb);
    };

    VkSamplerCreateInfo smi{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    smi.magFilter = smi.minFilter = VK_FILTER_LINEAR;
    smi.addressModeU = smi.addressModeV = smi.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    smi.maxLod = 1.0f;
    VkSampler rawSampler = VK_NULL_HANDLE;
    check(vkCreateSampler(dev, &smi, nullptr, &rawSampler), "vkCreateSampler");
    Unique<VkSampler> sampler(rawSampler, [dev](VkSampler s) { vkDestroySampler(dev, s, nullptr); });

    VkDescriptorSetLayoutBinding texb[kNumSlots]{};
    for (int s = 0; s < kNumSlots; ++s) {
        texb[s].binding = uint32_t(s);
        texb[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        texb[s].descriptorCount = 1;
        texb[s].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo texli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    texli.bindingCount = kNumSlots;
    texli.pBindings = texb;
    VkDescriptorSetLayout rawTexLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev, &texli, nullptr, &rawTexLayout), "vkCreateDescriptorSetLayout");
    Unique<VkDescriptorSetLayout> texLayout(rawTexLayout, [dev](VkDescriptorSetLayout l) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr);
    });

    VkDescriptorSetLayoutBinding boneb{};
    boneb.binding = 0;
    boneb.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    boneb.descriptorCount = 1;
    boneb.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    VkDescriptorSetLayoutCreateInfo boneli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    boneli.bindingCount = 1;
    boneli.pBindings = &boneb;
    VkDescriptorSetLayout rawBoneLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev, &boneli, nullptr, &rawBoneLayout), "vkCreateDescriptorSetLayout(bone)");
    Unique<VkDescriptorSetLayout> boneLayout(rawBoneLayout, [dev](VkDescriptorSetLayout l) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr);
    });

    const uint32_t kMaxMeshSets = 256, kMaxBoneSets = 4096;
    VkDescriptorPoolSize matSize{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kNumSlots * kMaxMeshSets};
    VkDescriptorPoolCreateInfo matdpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    matdpi.maxSets = kMaxMeshSets;
    matdpi.poolSizeCount = 1;
    matdpi.pPoolSizes = &matSize;
    VkDescriptorPool rawMatPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev, &matdpi, nullptr, &rawMatPool), "vkCreateDescriptorPool(mat)");
    Unique<VkDescriptorPool> matPool(rawMatPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });

    VkDescriptorPoolSize boneSize{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, kMaxBoneSets};
    VkDescriptorPoolCreateInfo bonedpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    bonedpi.maxSets = kMaxBoneSets;
    bonedpi.poolSizeCount = 1;
    bonedpi.pPoolSizes = &boneSize;
    VkDescriptorPool rawBonePool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev, &bonedpi, nullptr, &rawBonePool), "vkCreateDescriptorPool(bone)");
    Unique<VkDescriptorPool> bonePool(rawBonePool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });

    const VkFormat kTexColor = VK_FORMAT_R8G8B8A8_UNORM;
    std::vector<Image> textures;
    std::unordered_map<std::string, int> tex_index;
    auto upload_texture = [&](const TexData& td) -> int {
        int idx = int(textures.size());
        Image img = make_image(pd, dev, kTexColor,
                               VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                               VK_IMAGE_ASPECT_COLOR_BIT, td.w, td.h);
        Buffer stg = make_buffer(pd, dev, td.rgba.size(), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, td.rgba.data());
        one_time([&](VkCommandBuffer cb) {
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.image = img.img.get();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rgn.imageExtent = {td.w, td.h, 1};
            vkCmdCopyBufferToImage(cb, stg.buf.get(), img.img.get(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
        });
        textures.push_back(std::move(img));
        return idx;
    };
    const int kDefaultWhite = upload_texture({1, 1, {255, 255, 255, 255}});
    const int kDefaultNormal = upload_texture({1, 1, {128, 128, 255, 255}});
    const int kDefaultEmissive = upload_texture({1, 1, {0, 0, 0, 255}});

    auto resolve_texture = [&](const formats::Model& m, int material, const char* usage, int def) -> int {
        std::string path = layer_of(m, material, usage);
        if (path.empty() || !storage) return def;
        auto it = tex_index.find(path);
        if (it != tex_index.end()) return it->second;
        int idx = def;
        std::vector<std::string> roots = {texture_root};
        for (const char* r : {"mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
                              "mods/void.sc2mod/base.sc2assets/", "mods/swarm.sc2mod/base.sc2assets/", ""})
            roots.push_back(r);
        for (const std::string& root : roots) {
            try {
                auto bytes = storage->read_by_name(root + path);
                formats::DdsInfo info;
                auto rgba = formats::dds_decode_rgba(bytes, info);
                if (!rgba.empty() && info.width && info.height) {
                    idx = upload_texture({info.width, info.height, std::move(rgba)});
                    break;
                }
            } catch (const std::exception&) {   }
        }
        tex_index[path] = idx;
        return idx;
    };

    std::unordered_map<const formats::Model*, Mesh> mesh_cache;
    auto ensure_mesh = [&](const formats::Model& m) -> Mesh& {
        auto it = mesh_cache.find(&m);
        if (it != mesh_cache.end()) return it->second;
        Mesh mesh;
        std::vector<uint32_t> tri = resolve_indices(m, mesh.draws);
        std::vector<Vertex> verts = build_vertices(m, tri);
        mesh.draw_tex.resize(mesh.draws.size());
        for (size_t i = 0; i < mesh.draws.size(); ++i) {
            int mat = mesh.draws[i].material;
            mesh.draw_tex[i] = {resolve_texture(m, mat, "Diffuse", kDefaultWhite),
                                resolve_texture(m, mat, "Normal", kDefaultNormal),
                                resolve_texture(m, mat, "Emissive", kDefaultEmissive)};
        }
        mesh.vbuf = make_buffer(pd, dev, sizeof(Vertex) * verts.size(),
                                VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, verts.data());
        mesh.ibuf = make_buffer(pd, dev, sizeof(uint32_t) * tri.size(),
                                VK_BUFFER_USAGE_INDEX_BUFFER_BIT, tri.data());
        mesh.draw_set.assign(mesh.draws.size(), VK_NULL_HANDLE);
        for (size_t d = 0; d < mesh.draws.size(); ++d) {
            VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsa.descriptorPool = rawMatPool;
            dsa.descriptorSetCount = 1;
            dsa.pSetLayouts = &rawTexLayout;
            check(vkAllocateDescriptorSets(dev, &dsa, &mesh.draw_set[d]), "vkAllocateDescriptorSets(material)");
            VkDescriptorImageInfo dii[kNumSlots];
            VkWriteDescriptorSet w[kNumSlots];
            for (int s = 0; s < kNumSlots; ++s) {
                dii[s] = {rawSampler, textures[mesh.draw_tex[d][s]].view.get(),
                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
                w[s] = VkWriteDescriptorSet{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                w[s].dstSet = mesh.draw_set[d];
                w[s].dstBinding = uint32_t(s);
                w[s].descriptorCount = 1;
                w[s].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[s].pImageInfo = &dii[s];
            }
            vkUpdateDescriptorSets(dev, kNumSlots, w, 0, nullptr);
        }
        return mesh_cache.emplace(&m, std::move(mesh)).first->second;
    };

    VkAttachmentDescription att[2]{};
    att[0].format = fmt.format;
    att[0].samples = VK_SAMPLE_COUNT_1_BIT;
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att[1].format = kDepth;
    att[1].samples = VK_SAMPLE_COUNT_1_BIT;
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference depthRef{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription subp{};
    subp.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    subp.colorAttachmentCount = 1;
    subp.pColorAttachments = &colorRef;
    subp.pDepthStencilAttachment = &depthRef;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    deps[0].srcAccessMask = 0;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstAccessMask = 0;
    VkRenderPassCreateInfo rpi{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpi.attachmentCount = 2;
    rpi.pAttachments = att;
    rpi.subpassCount = 1;
    rpi.pSubpasses = &subp;
    rpi.dependencyCount = 2;
    rpi.pDependencies = deps;
    VkRenderPass rawPass = VK_NULL_HANDLE;
    check(vkCreateRenderPass(dev, &rpi, nullptr, &rawPass), "vkCreateRenderPass");
    Unique<VkRenderPass> renderPass(rawPass, [dev](VkRenderPass p) { vkDestroyRenderPass(dev, p, nullptr); });

    std::vector<Unique<VkFramebuffer>> framebuffers(nimg);
    for (uint32_t i = 0; i < nimg; ++i) {
        VkImageView fbViews[2] = {swap_views[i].get(), depths[i].view.get()};
        VkFramebufferCreateInfo fbi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fbi.renderPass = rawPass;
        fbi.attachmentCount = 2;
        fbi.pAttachments = fbViews;
        fbi.width = width;
        fbi.height = height;
        fbi.layers = 1;
        VkFramebuffer rawFb = VK_NULL_HANDLE;
        check(vkCreateFramebuffer(dev, &fbi, nullptr, &rawFb), "vkCreateFramebuffer");
        framebuffers[i] = Unique<VkFramebuffer>(rawFb, [dev](VkFramebuffer f) { vkDestroyFramebuffer(dev, f, nullptr); });
    }

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80};
    VkDescriptorSetLayout set_layouts[2] = {rawTexLayout, rawBoneLayout};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 2;
    pli.pSetLayouts = set_layouts;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pcr;
    VkPipelineLayout rawLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(dev, &pli, nullptr, &rawLayout), "vkCreatePipelineLayout");
    Unique<VkPipelineLayout> layout(rawLayout, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });

    Unique<VkShaderModule> vs = make_shader(dev, kMeshVertSpv, sizeof(kMeshVertSpv));
    Unique<VkShaderModule> fs = make_shader(dev, kMeshFragSpv, sizeof(kMeshFragSpv));
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs.get();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fs.get();
    stages[1].pName = "main";

    VkVertexInputBindingDescription vib{0, sizeof(Vertex), VK_VERTEX_INPUT_RATE_VERTEX};
    VkVertexInputAttributeDescription via[5] = {{0, 0, VK_FORMAT_R32G32B32_SFLOAT, 0},
                                                {1, 0, VK_FORMAT_R32G32B32_SFLOAT, 12},
                                                {2, 0, VK_FORMAT_R32G32_SFLOAT, 24},
                                                {3, 0, VK_FORMAT_R32G32B32A32_SFLOAT, 32},
                                                {4, 0, VK_FORMAT_R32G32B32A32_UINT, 48}};
    VkPipelineVertexInputStateCreateInfo vin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vin.vertexBindingDescriptionCount = 1;
    vin.pVertexBindingDescriptions = &vib;
    vin.vertexAttributeDescriptionCount = 5;
    vin.pVertexAttributeDescriptions = via;
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkViewport vp{0, 0, float(width), float(height), 0, 1};
    VkRect2D scis{{0, 0}, {width, height}};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &scis;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    dss.depthTestEnable = VK_TRUE;
    dss.depthWriteEnable = VK_TRUE;
    dss.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo gpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gpi.stageCount = 2;
    gpi.pStages = stages;
    gpi.pVertexInputState = &vin;
    gpi.pInputAssemblyState = &ia;
    gpi.pViewportState = &vps;
    gpi.pRasterizationState = &rs;
    gpi.pMultisampleState = &ms;
    gpi.pDepthStencilState = &dss;
    gpi.pColorBlendState = &cb;
    gpi.layout = rawLayout;
    gpi.renderPass = rawPass;
    VkPipeline rawPipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &gpi, nullptr, &rawPipeline), "vkCreateGraphicsPipelines");
    Unique<VkPipeline> pipeline(rawPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });

    VkDescriptorSetLayoutBinding ovlB{};
    ovlB.binding = 0;
    ovlB.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    ovlB.descriptorCount = 1;
    ovlB.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo ovlLi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ovlLi.bindingCount = 1;
    ovlLi.pBindings = &ovlB;
    VkDescriptorSetLayout rawOvlSetLayout = VK_NULL_HANDLE;
    check(vkCreateDescriptorSetLayout(dev, &ovlLi, nullptr, &rawOvlSetLayout), "vkCreateDescriptorSetLayout(overlay)");
    Unique<VkDescriptorSetLayout> ovlSetLayout(rawOvlSetLayout, [dev](VkDescriptorSetLayout l) {
        vkDestroyDescriptorSetLayout(dev, l, nullptr); });
    VkPipelineLayoutCreateInfo ovlPli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    ovlPli.setLayoutCount = 1;
    ovlPli.pSetLayouts = &rawOvlSetLayout;
    VkPipelineLayout rawOvlLayout = VK_NULL_HANDLE;
    check(vkCreatePipelineLayout(dev, &ovlPli, nullptr, &rawOvlLayout), "vkCreatePipelineLayout(overlay)");
    Unique<VkPipelineLayout> ovlLayout(rawOvlLayout, [dev](VkPipelineLayout l) { vkDestroyPipelineLayout(dev, l, nullptr); });

    Unique<VkShaderModule> ovlVs = make_shader(dev, kOverlayVertSpv, sizeof(kOverlayVertSpv));
    Unique<VkShaderModule> ovlFs = make_shader(dev, kOverlayFragSpv, sizeof(kOverlayFragSpv));
    VkPipelineShaderStageCreateInfo ovlStages[2]{};
    ovlStages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ovlStages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    ovlStages[0].module = ovlVs.get();
    ovlStages[0].pName = "main";
    ovlStages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    ovlStages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    ovlStages[1].module = ovlFs.get();
    ovlStages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo ovlVin{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineDepthStencilStateCreateInfo ovlDss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ovlDss.depthTestEnable = VK_FALSE;
    ovlDss.depthWriteEnable = VK_FALSE;
    VkPipelineColorBlendAttachmentState ovlCba{};
    ovlCba.blendEnable = VK_TRUE;
    ovlCba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
    ovlCba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ovlCba.colorBlendOp = VK_BLEND_OP_ADD;
    ovlCba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    ovlCba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    ovlCba.alphaBlendOp = VK_BLEND_OP_ADD;
    ovlCba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo ovlCb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    ovlCb.attachmentCount = 1;
    ovlCb.pAttachments = &ovlCba;
    VkGraphicsPipelineCreateInfo ovlGpi{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ovlGpi.stageCount = 2;
    ovlGpi.pStages = ovlStages;
    ovlGpi.pVertexInputState = &ovlVin;
    ovlGpi.pInputAssemblyState = &ia;
    ovlGpi.pViewportState = &vps;
    ovlGpi.pRasterizationState = &rs;
    ovlGpi.pMultisampleState = &ms;
    ovlGpi.pDepthStencilState = &ovlDss;
    ovlGpi.pColorBlendState = &ovlCb;
    ovlGpi.layout = rawOvlLayout;
    ovlGpi.renderPass = rawPass;
    VkPipeline rawOvlPipeline = VK_NULL_HANDLE;
    check(vkCreateGraphicsPipelines(dev, VK_NULL_HANDLE, 1, &ovlGpi, nullptr, &rawOvlPipeline),
          "vkCreateGraphicsPipelines(overlay)");
    Unique<VkPipeline> ovlPipeline(rawOvlPipeline, [dev](VkPipeline p) { vkDestroyPipeline(dev, p, nullptr); });

    Image ovlTex = make_image(pd, dev, kTexColor,
                              VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                              VK_IMAGE_ASPECT_COLOR_BIT, width, height);
    Buffer ovlStaging = make_buffer(pd, dev, VkDeviceSize(width) * height * 4,
                                    VK_BUFFER_USAGE_TRANSFER_SRC_BIT, nullptr);
    VkDescriptorPoolSize ovlPsz{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1};
    VkDescriptorPoolCreateInfo ovlDpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    ovlDpi.maxSets = 1;
    ovlDpi.poolSizeCount = 1;
    ovlDpi.pPoolSizes = &ovlPsz;
    VkDescriptorPool rawOvlPool = VK_NULL_HANDLE;
    check(vkCreateDescriptorPool(dev, &ovlDpi, nullptr, &rawOvlPool), "vkCreateDescriptorPool(overlay)");
    Unique<VkDescriptorPool> ovlPool(rawOvlPool, [dev](VkDescriptorPool p) { vkDestroyDescriptorPool(dev, p, nullptr); });
    VkDescriptorSet ovlSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsa.descriptorPool = rawOvlPool;
        dsa.descriptorSetCount = 1;
        dsa.pSetLayouts = &rawOvlSetLayout;
        check(vkAllocateDescriptorSets(dev, &dsa, &ovlSet), "vkAllocateDescriptorSets(overlay)");
        VkDescriptorImageInfo dii{rawSampler, ovlTex.view.get(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = ovlSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &dii;
        vkUpdateDescriptorSets(dev, 1, &w, 0, nullptr);
    }

    VkCommandBufferAllocateInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbi.commandPool = rawCmdPool;
    cbi.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbi.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    check(vkAllocateCommandBuffers(dev, &cbi, &cmd), "vkAllocateCommandBuffers");

    VkSemaphoreCreateInfo semi{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkSemaphore aRaw = VK_NULL_HANDLE;
    check(vkCreateSemaphore(dev, &semi, nullptr, &aRaw), "vkCreateSemaphore(acquire)");
    Unique<VkSemaphore> acquireSem(aRaw, [dev](VkSemaphore s) { vkDestroySemaphore(dev, s, nullptr); });
    std::vector<Unique<VkSemaphore>> submitSem(nimg);
    for (uint32_t i = 0; i < nimg; ++i) {
        VkSemaphore sRaw = VK_NULL_HANDLE;
        check(vkCreateSemaphore(dev, &semi, nullptr, &sRaw), "vkCreateSemaphore(submit)");
        submitSem[i] = Unique<VkSemaphore>(sRaw, [dev](VkSemaphore s) { vkDestroySemaphore(dev, s, nullptr); });
    }
    VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fi.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    VkFence fRaw = VK_NULL_HANDLE;
    check(vkCreateFence(dev, &fi, nullptr, &fRaw), "vkCreateFence");
    Unique<VkFence> fence(fRaw, [dev](VkFence x) { vkDestroyFence(dev, x, nullptr); });

    LARGE_INTEGER freq, prev_t;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&prev_t);
    InputAccum prev = accum;

    WindowResult result;
    result.created = true;
    result.width = width;
    result.height = height;
    result.validation_active = bool(messenger);

    std::vector<Buffer> bonebufs;
    std::vector<SceneInstance> scene;
    std::vector<uint8_t> overlay;
    Camera cam;
    bool running = true;

    while (running) {
        MSG m;
        while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&m);
            DispatchMessageW(&m);
            if (m.message == WM_QUIT) accum.quit = true;
        }

        LARGE_INTEGER now;
        QueryPerformanceCounter(&now);
        float dt_ms = float(double(now.QuadPart - prev_t.QuadPart) * 1000.0 / double(freq.QuadPart));
        prev_t = now;
        if (dt_ms > 250.0f) dt_ms = 250.0f;

        float msx = 1.0f, msy = 1.0f;
        if (RECT crc; GetClientRect(window.get(), &crc)) {
            const LONG cw = crc.right - crc.left, ch = crc.bottom - crc.top;
            if (cw > 0) msx = float(width) / float(cw);
            if (ch > 0) msy = float(height) / float(ch);
        }

        InputState in;
        in.view_w = width;
        in.view_h = height;
        in.mouse_x = accum.mouse_x * msx;
        in.mouse_y = accum.mouse_y * msy;
        in.mouse_dx = (accum.mouse_x - prev.mouse_x) * msx;
        in.mouse_dy = (accum.mouse_y - prev.mouse_y) * msy;
        in.wheel = accum.wheel;
        in.left_down = accum.left;
        in.left_pressed = accum.left && !prev.left;
        in.left_released = !accum.left && prev.left;
        in.right_down = accum.right;
        in.right_pressed = accum.right && !prev.right;
        in.right_released = !accum.right && prev.right;
        in.middle_down = accum.middle;
        in.shift = accum.shift;
        in.key_pan_left = accum.pan_left;
        in.key_pan_right = accum.pan_right;
        in.key_pan_up = accum.pan_up;
        in.key_pan_down = accum.pan_down;
        in.key_orbit_left = accum.orbit_left;
        in.key_orbit_right = accum.orbit_right;
        in.key_train = accum.train;
        in.key_build = accum.build;
        in.key_debug = accum.debug;
        in.key_hold = accum.hold;
        in.key_stop = accum.stop;
        in.key_patrol = accum.patrol;
        in.key_ctrl = accum.ctrl;
        for (int d = 0; d < 10; ++d) in.key_num[d] = accum.num[d];
        in.quit = accum.quit;
        prev = accum;
        accum.wheel = 0;

        scene.clear();
        overlay.clear();
        bool cont = frame(in, dt_ms, scene, cam, overlay);
        if (!cont || in.quit) running = false;
        const bool has_overlay = overlay.size() == size_t(width) * size_t(height) * 4;

        VkFence raw_fence = fence.get();
        vkWaitForFences(dev, 1, &raw_fence, VK_TRUE, UINT64_MAX);

        if (!running && result.frames_presented == 0 && max_frames == 0) break;

        std::vector<const SceneInstance*> live;
        for (const SceneInstance& s : scene)
            if (s.model && s.model->vertex_count && !s.model->indices.empty()) live.push_back(&s);

        for (const SceneInstance* inst : live) ensure_mesh(*inst->model);

        uint32_t idx = 0;
        VkResult ar = vkAcquireNextImageKHR(dev, rawSwap, UINT64_MAX, acquireSem.get(), VK_NULL_HANDLE, &idx);
        if (ar == VK_ERROR_OUT_OF_DATE_KHR) break;
        if (ar != VK_SUCCESS && ar != VK_SUBOPTIMAL_KHR) break;
        vkResetFences(dev, 1, &raw_fence);

        bonebufs.clear();
        bonebufs.reserve(live.size());
        vkResetDescriptorPool(dev, rawBonePool, 0);
        std::vector<VkDescriptorSet> bone_set(live.size(), VK_NULL_HANDLE);
        Mat4 view_proj = cam.view_proj(float(width) / float(height));
        for (size_t i = 0; i < live.size(); ++i) {
            const SceneInstance& inst = *live[i];
            std::vector<Mat4> bone_mats;
            if (inst.seq >= 0 && !inst.model->bones.empty())
                bone_mats = pose(*inst.model, uint32_t(inst.seq), inst.time_ms);
            else
                for (size_t b = 0; b < std::max<size_t>(1, inst.model->bones.size()); ++b)
                    bone_mats.push_back(Mat4::identity());
            if (bone_mats.empty()) bone_mats.push_back(Mat4::identity());
            std::vector<float> flat;
            flat.reserve(bone_mats.size() * 16);
            for (const Mat4& mm : bone_mats)
                for (int k = 0; k < 16; ++k) flat.push_back(mm.m[k]);
            bonebufs.push_back(make_buffer(pd, dev, flat.size() * 4, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, flat.data()));

            VkDescriptorSetAllocateInfo dsa{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            dsa.descriptorPool = rawBonePool;
            dsa.descriptorSetCount = 1;
            dsa.pSetLayouts = &rawBoneLayout;
            check(vkAllocateDescriptorSets(dev, &dsa, &bone_set[i]), "vkAllocateDescriptorSets(bone)");
            VkDescriptorBufferInfo bdi{bonebufs[i].buf.get(), 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet bw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            bw.dstSet = bone_set[i];
            bw.dstBinding = 0;
            bw.descriptorCount = 1;
            bw.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            bw.pBufferInfo = &bdi;
            vkUpdateDescriptorSets(dev, 1, &bw, 0, nullptr);
        }

        check(vkResetCommandBuffer(cmd, 0), "vkResetCommandBuffer");
        VkCommandBufferBeginInfo bbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        check(vkBeginCommandBuffer(cmd, &bbi), "vkBeginCommandBuffer");
        if (has_overlay) {
            void* p = nullptr;
            check(vkMapMemory(dev, ovlStaging.mem.get(), 0, overlay.size(), 0, &p), "vkMapMemory(overlay)");
            std::memcpy(p, overlay.data(), overlay.size());
            vkUnmapMemory(dev, ovlStaging.mem.get());
            VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.image = ovlTex.img.get();
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            b.srcAccessMask = 0;
            b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
            VkBufferImageCopy rgn{};
            rgn.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            rgn.imageExtent = {width, height, 1};
            vkCmdCopyBufferToImage(cmd, ovlStaging.buf.get(), ovlTex.img.get(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &rgn);
            b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                                 0, 0, nullptr, 0, nullptr, 1, &b);
        }
        VkClearValue clears[2];
        clears[0].color = {{0.06f, 0.07f, 0.09f, 1.0f}};
        clears[1].depthStencil = {1.0f, 0};
        VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        rbi.renderPass = rawPass;
        rbi.framebuffer = framebuffers[idx].get();
        rbi.renderArea = {{0, 0}, {width, height}};
        rbi.clearValueCount = 2;
        rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawPipeline);
        for (size_t i = 0; i < live.size(); ++i) {
            const SceneInstance& inst = *live[i];
            Mesh& mesh = ensure_mesh(*inst.model);
            Mat4 mvp = view_proj * inst.world;
            float pc[20];
            std::memcpy(pc, mvp.m.data(), 64);
            pc[16] = inst.team[0];
            pc[17] = inst.team[1];
            pc[18] = inst.team[2];
            pc[19] = inst.team[3];
            vkCmdPushConstants(cmd, rawLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 80, pc);
            VkDeviceSize off = 0;
            VkBuffer vb = mesh.vbuf.buf.get();
            vkCmdBindVertexBuffers(cmd, 0, 1, &vb, &off);
            vkCmdBindIndexBuffer(cmd, mesh.ibuf.buf.get(), 0, VK_INDEX_TYPE_UINT32);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawLayout, 1, 1, &bone_set[i], 0, nullptr);
            for (size_t d = 0; d < mesh.draws.size(); ++d) {
                vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawLayout, 0, 1, &mesh.draw_set[d], 0, nullptr);
                vkCmdDrawIndexed(cmd, mesh.draws[d].index_count, 1, mesh.draws[d].first_index, 0, 0);
            }
        }
        if (has_overlay) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawOvlPipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, rawOvlLayout, 0, 1, &ovlSet, 0, nullptr);
            vkCmdDraw(cmd, 3, 1, 0, 0);
        }
        vkCmdEndRenderPass(cmd);
        check(vkEndCommandBuffer(cmd), "vkEndCommandBuffer");

        VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        VkSemaphore waitSem = acquireSem.get(), sigSem = submitSem[idx].get();
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.waitSemaphoreCount = 1;
        si.pWaitSemaphores = &waitSem;
        si.pWaitDstStageMask = &wait_stage;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &cmd;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &sigSem;
        check(vkQueueSubmit(queue, 1, &si, raw_fence), "vkQueueSubmit");

        VkPresentInfoKHR ppi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        ppi.waitSemaphoreCount = 1;
        ppi.pWaitSemaphores = &sigSem;
        ppi.swapchainCount = 1;
        ppi.pSwapchains = &rawSwap;
        ppi.pImageIndices = &idx;
        VkResult pr = vkQueuePresentKHR(queue, &ppi);
        if (pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR) ++result.frames_presented;
        else break;

        if (max_frames && result.frames_presented >= max_frames) running = false;
    }

    vkDeviceWaitIdle(dev);
    result.validation_errors = val_errors;
    return result;
}

}
