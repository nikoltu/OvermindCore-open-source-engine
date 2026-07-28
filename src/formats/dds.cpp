#include "sc2/formats/dds.hpp"

#include <stdexcept>

#include "sc2/core/byte_reader.hpp"

namespace sc2::formats {

using core::ByteReader;

DdsInfo dds_parse(const std::vector<uint8_t>& data) {
    if (data.size() < 128 || data[0] != 'D' || data[1] != 'D' || data[2] != 'S' ||
        data[3] != ' ')
        throw std::runtime_error("dds: bad magic");

    ByteReader r(data);
    r.seek(4);
    if (r.u32le() != 124) throw std::runtime_error("dds: bad header size");

    DdsInfo info;
    r.seek(0x0C);
    info.height = r.u32le();
    info.width = r.u32le();
    r.u32le();
    r.u32le();
    info.mip_count = r.u32le();
    if (info.mip_count == 0) info.mip_count = 1;

    r.seek(0x4C);
    r.u32le();
    uint32_t pf_flags = r.u32le();
    uint32_t fourcc = r.u32le();
    constexpr uint32_t kFourCC = 0x4;
    if (pf_flags & kFourCC) {
        char c[4] = {char(fourcc & 0xFF), char((fourcc >> 8) & 0xFF),
                     char((fourcc >> 16) & 0xFF), char((fourcc >> 24) & 0xFF)};
        info.fourcc.assign(c, 4);
    }

    info.data_offset = 128;
    if (info.fourcc == "DX10") {
        if (data.size() < 148) throw std::runtime_error("dds: truncated DX10 header");
        r.seek(128);
        info.dxgi_format = r.u32le();
        info.data_offset = 148;
    }
    return info;
}

namespace {
struct Rgb {
    int r, g, b;
};
static int rdiv(int v, int n) { return (2 * v + n) / (2 * n); }

static Rgb color565(uint16_t c) {
    int r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return {(r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2)};
}

static uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
static uint32_t rd32(const uint8_t* p) {
    return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 |
           uint32_t(p[3]) << 24;
}
}

std::vector<uint8_t> decode_dxt(const std::vector<uint8_t>& data, size_t off,
                                uint32_t w, uint32_t h, const std::string& fourcc) {
    int dxt = fourcc == "DXT1" ? 1 : fourcc == "DXT3" ? 3 : 5;
    size_t blk = (dxt == 1) ? 8 : 16;
    std::vector<uint8_t> out(size_t(w) * h * 4, 0);
    uint32_t bxn = (w + 3) / 4, byn = (h + 3) / 4;
    size_t p = off;

    for (uint32_t by = 0; by < byn; ++by) {
        for (uint32_t bx = 0; bx < bxn; ++bx) {
            if (p + blk > data.size()) return out;
            int alpha[16];
            bool have_alpha = false;
            size_t coff = p;
            if (dxt == 5) {
                int a0 = data[p], a1 = data[p + 1];
                int at[8] = {a0, a1, 0, 0, 0, 0, 0, 0};
                if (a0 > a1)
                    for (int k = 1; k <= 6; ++k) at[k + 1] = rdiv((7 - k) * a0 + k * a1, 7);
                else {
                    for (int k = 1; k <= 4; ++k) at[k + 1] = rdiv((5 - k) * a0 + k * a1, 5);
                    at[6] = 0;
                    at[7] = 255;
                }
                uint64_t bits = 0;
                for (int i = 0; i < 6; ++i) bits |= uint64_t(data[p + 2 + i]) << (8 * i);
                for (int k = 0; k < 16; ++k) alpha[k] = at[(bits >> (3 * k)) & 7];
                have_alpha = true;
                coff = p + 8;
            } else if (dxt == 3) {
                for (int k = 0; k < 16; ++k)
                    alpha[k] = ((data[p + (k >> 1)] >> ((k & 1) * 4)) & 0xF) * 17;
                have_alpha = true;
                coff = p + 8;
            }

            uint16_t c0 = rd16(&data[coff]), c1 = rd16(&data[coff + 2]);
            Rgb r0 = color565(c0), r1 = color565(c1);
            Rgb pal[4] = {r0, r1, {}, {}};
            bool onebit = (dxt == 1 && c0 <= c1);
            if (onebit) {
                pal[2] = {(r0.r + r1.r) >> 1, (r0.g + r1.g) >> 1, (r0.b + r1.b) >> 1};
                pal[3] = {0, 0, 0};
            } else {
                pal[2] = {rdiv(2 * r0.r + r1.r, 3), rdiv(2 * r0.g + r1.g, 3),
                          rdiv(2 * r0.b + r1.b, 3)};
                pal[3] = {rdiv(r0.r + 2 * r1.r, 3), rdiv(r0.g + 2 * r1.g, 3),
                          rdiv(r0.b + 2 * r1.b, 3)};
            }
            uint32_t idx = rd32(&data[coff + 4]);
            for (int k = 0; k < 16; ++k) {
                int ci = (idx >> (2 * k)) & 3;
                const Rgb& col = pal[ci];
                int a = (onebit && ci == 3) ? 0 : have_alpha ? alpha[k] : 255;
                uint32_t lx = bx * 4 + (k & 3), ly = by * 4 + (k >> 2);
                if (lx >= w || ly >= h) continue;
                size_t di = (size_t(ly) * w + lx) * 4;
                out[di] = uint8_t(col.r);
                out[di + 1] = uint8_t(col.g);
                out[di + 2] = uint8_t(col.b);
                out[di + 3] = uint8_t(a);
            }
            p += blk;
        }
    }
    return out;
}

std::vector<uint8_t> dds_decode_rgba(const std::vector<uint8_t>& data, DdsInfo& out) {
    out = dds_parse(data);
    return decode_dxt(data, out.data_offset, out.width, out.height, out.fourcc);
}

}
