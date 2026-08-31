#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "sc2/render/vk_helpers.hpp"
#include "sc2/render/vk_raii.hpp"

namespace sc2::render {

class Device {
public:
    Device() = default;

    static Device create_headless(const char* app_name) {
        Device d;
        VkInstance rawInst = VK_NULL_HANDLE;
        VkApplicationInfo app{};
        app.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
        app.pApplicationName = app_name;
        app.apiVersion = VK_API_VERSION_1_1;
        VkInstanceCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
        ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &rawInst), "vkCreateInstance");
        d.instance_ = Unique<VkInstance>(rawInst, [](VkInstance i) { vkDestroyInstance(i, nullptr); });

        uint32_t ndev = 0;
        vkEnumeratePhysicalDevices(rawInst, &ndev, nullptr);
        if (ndev == 0) throw std::runtime_error("render device: no Vulkan physical device");
        std::vector<VkPhysicalDevice> pds(ndev);
        vkEnumeratePhysicalDevices(rawInst, &ndev, pds.data());
        d.pd_ = pds[0];

        uint32_t nqf = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(d.pd_, &nqf, nullptr);
        std::vector<VkQueueFamilyProperties> qfs(nqf);
        vkGetPhysicalDeviceQueueFamilyProperties(d.pd_, &nqf, qfs.data());
        d.gfx_family_ = UINT32_MAX;
        for (uint32_t i = 0; i < nqf; ++i)
            if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                d.gfx_family_ = i;
                break;
            }
        if (d.gfx_family_ == UINT32_MAX) throw std::runtime_error("render device: no graphics queue");

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = d.gfx_family_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        VkDevice dev = VK_NULL_HANDLE;
        check(vkCreateDevice(d.pd_, &dci, nullptr, &dev), "vkCreateDevice");
        d.device_ = Unique<VkDevice>(dev, [](VkDevice x) { vkDestroyDevice(x, nullptr); });
        vkGetDeviceQueue(dev, d.gfx_family_, 0, &d.gfx_queue_);
        return d;
    }

    VkInstance instance() const { return instance_.get(); }
    VkPhysicalDevice physical() const { return pd_; }
    VkDevice device() const { return device_.get(); }
    VkQueue graphics_queue() const { return gfx_queue_; }
    uint32_t graphics_family() const { return gfx_family_; }

private:
    Unique<VkInstance> instance_;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    Unique<VkDevice> device_;
    VkQueue gfx_queue_ = VK_NULL_HANDLE;
    uint32_t gfx_family_ = 0;
};

}
