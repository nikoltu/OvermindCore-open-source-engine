#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "omc/render/frame_prof.hpp"
#include "omc/render/scene.hpp"
#include "omc/render/vk_raii.hpp"

namespace omc::render {

inline const char* pass_group_label(std::uint8_t g) {
    switch (PassGroup(g)) {
        case PassGroup::Terrain: return "gpu.terrain.mesh";
        case PassGroup::Units: return "gpu.units";
        case PassGroup::Effects: return "gpu.effects";
        case PassGroup::Foliage: return "gpu.foliage";
        case PassGroup::Doodads: return "gpu.doodads";
        default: return "gpu.other";
    }
}

inline const char* shadow_group_label(std::uint8_t g) {
    switch (PassGroup(g)) {
        case PassGroup::Terrain: return "gpu.shadow.terrain";
        case PassGroup::Units: return "gpu.shadow.units";
        case PassGroup::Effects: return "gpu.shadow.effects";
        case PassGroup::Foliage: return "gpu.shadow.foliage";
        case PassGroup::Doodads: return "gpu.shadow.doodads";
        default: return "gpu.shadow.other";
    }
}

class GpuPassTimer {
public:
    static constexpr std::uint32_t kCap = 256;

    GpuPassTimer() = default;
    GpuPassTimer(const GpuPassTimer&) = delete;
    GpuPassTimer& operator=(const GpuPassTimer&) = delete;
    GpuPassTimer(GpuPassTimer&&) = default;
    GpuPassTimer& operator=(GpuPassTimer&&) = default;

    void init(VkPhysicalDevice pd, VkDevice dev, std::uint32_t family, const char* total_label, bool detail) {
        if (!gpu_prof_switch() || pool_) return;
        std::uint32_t n = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, nullptr);
        std::vector<VkQueueFamilyProperties> q(n);
        vkGetPhysicalDeviceQueueFamilyProperties(pd, &n, q.data());
        if (family >= n || q[family].timestampValidBits == 0) return;
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(pd, &props);
        if (!(props.limits.timestampPeriod > 0.0f)) return;
        period_ms_ = double(props.limits.timestampPeriod) * 1e-6;
        const std::uint32_t bits = q[family].timestampValidBits;
        mask_ = bits >= 64 ? ~std::uint64_t(0) : ((std::uint64_t(1) << bits) - 1);
        VkQueryPoolCreateInfo ci{};
        ci.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
        ci.queryType = VK_QUERY_TYPE_TIMESTAMP;
        ci.queryCount = kCap;
        VkQueryPool p = VK_NULL_HANDLE;
        if (vkCreateQueryPool(dev, &ci, nullptr, &p) != VK_SUCCESS) return;
        dev_ = dev;
        pool_ = Unique<VkQueryPool>(p, [dev](VkQueryPool x) { vkDestroyQueryPool(dev, x, nullptr); });
        total_ = total_label;
        detail_ = detail;
        if (detail)
            std::printf("gpu timing: %s, timestamp period %.3f ns, %u valid bits\n", props.deviceName, double(props.limits.timestampPeriod),
                        unsigned(bits));
        labels_.reserve(kCap);
    }

    bool on() const { return pool_.get() != VK_NULL_HANDLE; }

    void begin(VkCommandBuffer cmd) {
        if (!on()) return;
        collect();
        vkCmdResetQueryPool(cmd, pool_.get(), 0, kCap);
        labels_.clear();
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, pool_.get(), 0);
        labels_.push_back(nullptr);
        pending_ = true;
    }

    void mark(VkCommandBuffer cmd, const char* label) {
        if (!on() || labels_.empty() || labels_.size() >= kCap) return;
        vkCmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, pool_.get(), std::uint32_t(labels_.size()));
        labels_.push_back(label);
    }

    void collect() {
        if (!on() || !pending_) return;
        pending_ = false;
        const std::uint32_t n = std::uint32_t(labels_.size());
        if (n < 2) return;
        ts_.assign(n, 0);
        if (vkGetQueryPoolResults(dev_, pool_.get(), 0, n, std::size_t(n) * sizeof(std::uint64_t), ts_.data(), sizeof(std::uint64_t),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS)
            return;
        if (detail_) {
            names_.clear();
            sums_.clear();
            for (std::uint32_t i = 1; i < n; ++i) {
                const double ms = double((ts_[i] - ts_[i - 1]) & mask_) * period_ms_;
                std::size_t k = 0;
                while (k < names_.size() && std::strcmp(names_[k], labels_[i]) != 0) ++k;
                if (k == names_.size()) {
                    names_.push_back(labels_[i]);
                    sums_.push_back(0.0);
                }
                sums_[k] += ms;
            }
            for (std::size_t k = 0; k < names_.size(); ++k) frame_prof_add(names_[k], sums_[k]);
            parents_.clear();
            parent_sums_.clear();
            for (std::size_t k = 0; k < names_.size(); ++k) {
                const char* dot = std::strchr(names_[k] + 4, '.');
                if (std::strncmp(names_[k], "gpu.", 4) != 0 || !dot) continue;
                const std::string parent(names_[k], std::size_t(dot - names_[k]));
                std::size_t j = 0;
                while (j < parents_.size() && parents_[j] != parent) ++j;
                if (j == parents_.size()) {
                    parents_.push_back(parent);
                    parent_sums_.push_back(0.0);
                }
                parent_sums_[j] += sums_[k];
            }
            for (std::size_t j = 0; j < parents_.size(); ++j) frame_prof_add(parents_[j].c_str(), parent_sums_[j]);
        }
        frame_prof_add(total_, double((ts_[n - 1] - ts_[0]) & mask_) * period_ms_);
    }

private:
    VkDevice dev_ = VK_NULL_HANDLE;
    Unique<VkQueryPool> pool_;
    double period_ms_ = 0.0;
    std::uint64_t mask_ = 0;
    const char* total_ = "gpu.frame";
    bool detail_ = true;
    bool pending_ = false;
    std::vector<const char*> labels_;
    std::vector<std::uint64_t> ts_;
    std::vector<const char*> names_;
    std::vector<double> sums_;
    std::vector<std::string> parents_;
    std::vector<double> parent_sums_;
};

}
