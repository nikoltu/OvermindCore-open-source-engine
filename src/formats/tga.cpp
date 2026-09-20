#include "sc2/formats/tga.hpp"

#include <cstddef>
#include <stdexcept>
#include <string>

namespace sc2::formats {

std::vector<uint8_t> decode_tga(const std::vector<uint8_t>& bytes, TgaInfo& out) {
    if (bytes.size() < 18) throw std::runtime_error("tga: file shorter than its 18-byte header");
    const uint8_t id_len = bytes[0];
    const uint8_t cmap_type = bytes[1];
    const uint8_t img_type = bytes[2];
    const uint32_t w = uint32_t(bytes[12]) | (uint32_t(bytes[13]) << 8);
    const uint32_t h = uint32_t(bytes[14]) | (uint32_t(bytes[15]) << 8);
    const uint8_t bpp = bytes[16];
    const uint8_t desc = bytes[17];

    if (cmap_type != 0)
        throw std::runtime_error("tga: colour-mapped images are not supported (cmap type " +
                                 std::to_string(int(cmap_type)) + ")");
    if (img_type != 2)
        throw std::runtime_error("tga: only uncompressed true-colour (type 2) is supported, got type " +
                                 std::to_string(int(img_type)));
    if (bpp != 24 && bpp != 32)
        throw std::runtime_error("tga: only 24- or 32-bit true-colour is supported, got " +
                                 std::to_string(int(bpp)) + " bpp");
    if (!w || !h) throw std::runtime_error("tga: zero-sized image");

    const std::size_t stride = std::size_t(bpp / 8);
    const std::size_t pixels = std::size_t(w) * h;
    const std::size_t data_off = std::size_t(18) + id_len;
    if (bytes.size() < data_off + pixels * stride)
        throw std::runtime_error("tga: pixel data truncated");

    const bool top_origin = (desc & 0x20) != 0;

    std::vector<uint8_t> rgba(pixels * 4);
    for (uint32_t y = 0; y < h; ++y) {
        const uint32_t src_row = top_origin ? y : (h - 1 - y);
        const std::size_t si = data_off + std::size_t(src_row) * w * stride;
        std::size_t di = std::size_t(y) * w * 4;
        for (uint32_t x = 0; x < w; ++x) {
            const std::size_t s = si + std::size_t(x) * stride;
            rgba[di + 0] = bytes[s + 2];
            rgba[di + 1] = bytes[s + 1];
            rgba[di + 2] = bytes[s + 0];
            rgba[di + 3] = stride == 4 ? bytes[s + 3] : uint8_t(255);
            di += 4;
        }
    }
    out.width = w;
    out.height = h;
    out.bpp = bpp;
    return rgba;
}

}
