#include "sc2/formats/png.hpp"

#include <stdexcept>

namespace sc2::formats {

namespace {

uint32_t crc32(const uint8_t* p, size_t n, uint32_t crc = 0xFFFFFFFFu) {
    for (size_t i = 0; i < n; ++i) {
        crc ^= p[i];
        for (int k = 0; k < 8; ++k)
            crc = (crc >> 1) ^ (0xEDB88320u & (~(crc & 1) + 1));
    }
    return crc;
}

uint32_t adler32(const uint8_t* p, size_t n) {
    uint32_t a = 1, b = 0;
    for (size_t i = 0; i < n; ++i) {
        a = (a + p[i]) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void put_be32(std::vector<uint8_t>& out, uint32_t v) {
    out.push_back(uint8_t(v >> 24));
    out.push_back(uint8_t(v >> 16));
    out.push_back(uint8_t(v >> 8));
    out.push_back(uint8_t(v));
}

void chunk(std::vector<uint8_t>& out, const char type[4], const uint8_t* data,
           size_t len) {
    put_be32(out, uint32_t(len));
    size_t crc_start = out.size();
    out.insert(out.end(), type, type + 4);
    out.insert(out.end(), data, data + len);
    uint32_t c = crc32(out.data() + crc_start, 4 + len);
    put_be32(out, c ^ 0xFFFFFFFFu);
}

}

std::vector<uint8_t> write_png(const uint8_t* rgba, uint32_t width,
                               uint32_t height) {
    const size_t stride = size_t(width) * 4;
    std::vector<uint8_t> raw;
    raw.reserve((stride + 1) * height);
    for (uint32_t y = 0; y < height; ++y) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba + y * stride, rgba + (y + 1) * stride);
    }

    std::vector<uint8_t> zlib;
    zlib.push_back(0x78);
    zlib.push_back(0x01);
    size_t pos = 0;
    while (pos < raw.size() || raw.empty()) {
        size_t block = raw.size() - pos;
        if (block > 65535) block = 65535;
        bool last = (pos + block >= raw.size());
        zlib.push_back(last ? 1 : 0);
        zlib.push_back(uint8_t(block));
        zlib.push_back(uint8_t(block >> 8));
        zlib.push_back(uint8_t(~block));
        zlib.push_back(uint8_t(~block >> 8));
        zlib.insert(zlib.end(), raw.begin() + pos, raw.begin() + pos + block);
        pos += block;
        if (last) break;
    }
    uint32_t ad = adler32(raw.data(), raw.size());
    put_be32(zlib, ad);

    std::vector<uint8_t> out;
    const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
    out.insert(out.end(), sig, sig + 8);

    uint8_t ihdr[13];
    ihdr[0] = uint8_t(width >> 24); ihdr[1] = uint8_t(width >> 16);
    ihdr[2] = uint8_t(width >> 8);  ihdr[3] = uint8_t(width);
    ihdr[4] = uint8_t(height >> 24); ihdr[5] = uint8_t(height >> 16);
    ihdr[6] = uint8_t(height >> 8);  ihdr[7] = uint8_t(height);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    chunk(out, "IHDR", ihdr, 13);
    chunk(out, "IDAT", zlib.data(), zlib.size());
    chunk(out, "IEND", nullptr, 0);
    return out;
}

std::vector<uint8_t> write_png(const std::vector<uint8_t>& rgba, uint32_t width,
                               uint32_t height) {
    if (rgba.size() != size_t(width) * height * 4)
        throw std::runtime_error("png: pixel buffer size != width*height*4");
    return write_png(rgba.data(), width, height);
}

}
