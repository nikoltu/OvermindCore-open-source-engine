#pragma once

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

#define VK_USE_PLATFORM_WIN32_KHR
#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sc2/render/vk_helpers.hpp"
#include "sc2/render/vk_raii.hpp"
#include "sc2/render/window.hpp"

namespace sc2::render {

namespace wsi {

inline bool has_layer(const std::vector<VkLayerProperties>& layers, const char* name) {
    for (const auto& l : layers)
        if (std::strcmp(l.layerName, name) == 0) return true;
    return false;
}

inline bool has_ext(const std::vector<VkExtensionProperties>& exts, const char* name) {
    for (const auto& e : exts)
        if (std::strcmp(e.extensionName, name) == 0) return true;
    return false;
}

inline VKAPI_ATTR VkBool32 VKAPI_CALL debug_cb(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
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

}

struct WsiDevice {
    Unique<VkInstance> instance;
    Unique<VkDebugUtilsMessengerEXT> messenger;
    Unique<VkSurfaceKHR> surface;
    Unique<VkDevice> device;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    uint32_t gfx_family = UINT32_MAX;
    VkQueue queue = VK_NULL_HANDLE;
};

inline WsiDevice create_wsi_device(HINSTANCE hinst, HWND hwnd, const char* ctx,
                                   uint32_t* val_errors) {
    auto fail = [ctx](const char* what) {
        throw WindowUnavailable(std::string(ctx) + what);
    };

    WsiDevice w;

    uint32_t nlayers = 0;
    vkEnumerateInstanceLayerProperties(&nlayers, nullptr);
    std::vector<VkLayerProperties> layers(nlayers);
    if (nlayers) vkEnumerateInstanceLayerProperties(&nlayers, layers.data());
    uint32_t niexts = 0;
    vkEnumerateInstanceExtensionProperties(nullptr, &niexts, nullptr);
    std::vector<VkExtensionProperties> iexts(niexts);
    if (niexts) vkEnumerateInstanceExtensionProperties(nullptr, &niexts, iexts.data());

    if (!wsi::has_ext(iexts, VK_KHR_SURFACE_EXTENSION_NAME) ||
        !wsi::has_ext(iexts, VK_KHR_WIN32_SURFACE_EXTENSION_NAME))
        fail("win32 surface instance extensions unavailable");

    std::vector<const char*> want_ext = {VK_KHR_SURFACE_EXTENSION_NAME,
                                         VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    std::vector<const char*> want_layer;
    const char* want_validation = std::getenv("SC2_VK_VALIDATION");
    const bool validation = want_validation && *want_validation && *want_validation != '0' &&
                            wsi::has_layer(layers, "VK_LAYER_KHRONOS_validation") &&
                            wsi::has_ext(iexts, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (validation) {
        want_layer.push_back("VK_LAYER_KHRONOS_validation");
        want_ext.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    VkApplicationInfo app{};

    app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = uint32_t(want_ext.size());
    ici.ppEnabledExtensionNames = want_ext.data();
    ici.enabledLayerCount = uint32_t(want_layer.size());
    ici.ppEnabledLayerNames = want_layer.empty() ? nullptr : want_layer.data();
    VkInstance rawInst = VK_NULL_HANDLE;
    if (vkCreateInstance(&ici, nullptr, &rawInst) != VK_SUCCESS) fail("vkCreateInstance (WSI) failed");
    w.instance = Unique<VkInstance>(rawInst, [](VkInstance i) { vkDestroyInstance(i, nullptr); });

    if (validation && val_errors) {
        auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(rawInst, "vkCreateDebugUtilsMessengerEXT"));
        auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
            vkGetInstanceProcAddr(rawInst, "vkDestroyDebugUtilsMessengerEXT"));
        if (create && destroy) {
            VkDebugUtilsMessengerCreateInfoEXT mi =
                vk_init<VkDebugUtilsMessengerCreateInfoEXT>(
                    VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT);
            mi.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                                 VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
            mi.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                             VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
            mi.pfnUserCallback = wsi::debug_cb;
            mi.pUserData = val_errors;
            VkDebugUtilsMessengerEXT raw = VK_NULL_HANDLE;
            if (create(rawInst, &mi, nullptr, &raw) == VK_SUCCESS)
                w.messenger = Unique<VkDebugUtilsMessengerEXT>(
                    raw, [rawInst, destroy](VkDebugUtilsMessengerEXT m) {
                        destroy(rawInst, m, nullptr);
                    });
        }
    }

    VkWin32SurfaceCreateInfoKHR wsci{};
    wsci.sType = VK_STRUCTURE_TYPE_WIN32_SURFACE_CREATE_INFO_KHR;
    wsci.hinstance = hinst;
    wsci.hwnd = hwnd;
    VkSurfaceKHR rawSurf = VK_NULL_HANDLE;
    if (vkCreateWin32SurfaceKHR(rawInst, &wsci, nullptr, &rawSurf) != VK_SUCCESS)
        fail("vkCreateWin32SurfaceKHR failed");
    w.surface = Unique<VkSurfaceKHR>(
        rawSurf, [rawInst](VkSurfaceKHR s) { vkDestroySurfaceKHR(rawInst, s, nullptr); });

    if (!pick_present_device(rawInst, rawSurf, w.pd, w.gfx_family))
        fail("no graphics+present queue family");

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{};
    qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    qci.queueFamilyIndex = w.gfx_family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devext[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures have{};
    vkGetPhysicalDeviceFeatures(w.pd, &have);
    VkPhysicalDeviceFeatures want{};
    want.samplerAnisotropy = have.samplerAnisotropy;
    VkDeviceCreateInfo dci{};
    dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devext;
    dci.pEnabledFeatures = &want;
    VkDevice dev = VK_NULL_HANDLE;
    if (vkCreateDevice(w.pd, &dci, nullptr, &dev) != VK_SUCCESS) fail("vkCreateDevice (swapchain) failed");
    w.device = Unique<VkDevice>(dev, [](VkDevice d) { vkDestroyDevice(d, nullptr); });
    vkGetDeviceQueue(dev, w.gfx_family, 0, &w.queue);

    return w;
}

}
