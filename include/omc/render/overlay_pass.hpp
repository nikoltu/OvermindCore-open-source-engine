#pragma once

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace omc::render {

std::vector<uint8_t> composite_overlay(const std::vector<uint8_t>& base_rgba,
                                       const std::vector<uint8_t>& overlay_rgba,
                                       uint32_t width, uint32_t height);

struct OverlayRowSpan {
    uint32_t y0 = 0, y1 = 0;
};

std::vector<OverlayRowSpan> overlay_changed_rows(std::vector<uint8_t>& shown, const std::vector<uint8_t>& next,
                                                 uint32_t width, uint32_t height);

bool record_overlay_upload(VkDevice dev, VkCommandBuffer cmd, VkDeviceMemory stage_mem, VkBuffer stage, VkImage image,
                           std::vector<uint8_t>& shown, const std::vector<uint8_t>& next, uint32_t width, uint32_t height,
                           VkImageLayout settled_layout);

}
