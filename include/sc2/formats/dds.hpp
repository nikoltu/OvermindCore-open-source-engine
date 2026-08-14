#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace sc2::formats {

struct DdsInfo {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mip_count = 1;
    std::string fourcc;
    uint32_t dxgi_format = 0;
    size_t data_offset = 128;
};

DdsInfo dds_parse(const std::vector<uint8_t>& data);

std::vector<uint8_t> decode_dxt(const std::vector<uint8_t>& data, size_t off,
                                uint32_t w, uint32_t h, const std::string& fourcc);

std::vector<uint8_t> decode_bc45(const std::vector<uint8_t>& data, size_t off,
                                 uint32_t w, uint32_t h, bool two_channel);

std::vector<uint8_t> dds_decode_rgba(const std::vector<uint8_t>& data, DdsInfo& out);

}
