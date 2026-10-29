#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <stdexcept>
#include <vector>

#include "omc/render/offscreen.hpp"
#include "omc/render/vk_helpers.hpp"
#include "omc/render/vk_raii.hpp"

namespace omc::render {

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

        const std::vector<RankedDevice> ranked = ranked_physical_devices(rawInst);
        if (ranked.empty()) throw std::runtime_error("render device: no Vulkan physical device");
        d.gfx_family_ = UINT32_MAX;
        for (const RankedDevice& rd : ranked) {
            uint32_t nqf = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(rd.pd, &nqf, nullptr);
            std::vector<VkQueueFamilyProperties> qfs(nqf);
            vkGetPhysicalDeviceQueueFamilyProperties(rd.pd, &nqf, qfs.data());
            for (uint32_t i = 0; i < nqf; ++i)
                if (qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
                    d.gfx_family_ = i;
                    break;
                }
            if (d.gfx_family_ != UINT32_MAX) {
                d.pd_ = rd.pd;
                log_chosen_device(rd.pd, rd.index, ranked.size());
                break;
            }
        }
        if (d.gfx_family_ == UINT32_MAX) throw std::runtime_error("render device: no graphics queue");

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{};
        qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        qci.queueFamilyIndex = d.gfx_family_;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkPhysicalDeviceFeatures have{};
        vkGetPhysicalDeviceFeatures(d.pd_, &have);
        VkPhysicalDeviceFeatures want{};
        want.samplerAnisotropy = have.samplerAnisotropy;
        VkDeviceCreateInfo dci{};
        dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        dci.pEnabledFeatures = &want;
        VkDevice dev = VK_NULL_HANDLE;
        check(vkCreateDevice(d.pd_, &dci, nullptr, &dev), "vkCreateDevice");
        d.device_ = Unique<VkDevice>(dev, [](VkDevice x) { vkDestroyDevice(x, nullptr); });
        vkGetDeviceQueue(dev, d.gfx_family_, 0, &d.gfx_queue_);
        return d;
    }

    static Device& shared_headless(const char* app_name) {
        Device*& g = shared_slot();
        if (!g) {
            const Offer& o = offer_slot();
            if (o.dev != VK_NULL_HANDLE && offscreen_on_window_device()) {
                Device* d = new Device();
                d->instance_ = Unique<VkInstance>(o.inst, [](VkInstance) {});
                d->device_ = Unique<VkDevice>(o.dev, [](VkDevice) {});
                d->pd_ = o.pd;
                d->gfx_queue_ = o.queue;
                d->gfx_family_ = o.family;
                d->adopted_ = true;
                g = d;
            } else {
                g = new Device(create_headless(app_name));
            }
        }
        return *g;
    }
    static void offer_window_device(VkInstance inst, VkPhysicalDevice pd, VkDevice dev, VkQueue q, uint32_t family) {
        offer_slot() = Offer{inst, pd, dev, q, family};
    }
    static void withdraw_window_device() {
        offer_slot() = Offer{};
        Device*& g = shared_slot();
        if (g && g->adopted_) {
            delete g;
            g = nullptr;
        }
    }
    static bool shared_is_adopted() { return shared_slot() && shared_slot()->adopted_; }

    VkInstance instance() const { return instance_.get(); }
    VkPhysicalDevice physical() const { return pd_; }
    VkDevice device() const { return device_.get(); }
    VkQueue graphics_queue() const { return gfx_queue_; }
    uint32_t graphics_family() const { return gfx_family_; }

private:
    static Device*& shared_slot() {
        static Device* g = nullptr;
        return g;
    }
    struct Offer {
        VkInstance inst = VK_NULL_HANDLE;
        VkPhysicalDevice pd = VK_NULL_HANDLE;
        VkDevice dev = VK_NULL_HANDLE;
        VkQueue queue = VK_NULL_HANDLE;
        uint32_t family = 0;
    };
    static Offer& offer_slot() {
        static Offer o;
        return o;
    }
    bool adopted_ = false;
    Unique<VkInstance> instance_;
    VkPhysicalDevice pd_ = VK_NULL_HANDLE;
    Unique<VkDevice> device_;
    VkQueue gfx_queue_ = VK_NULL_HANDLE;
    uint32_t gfx_family_ = 0;
};

}
