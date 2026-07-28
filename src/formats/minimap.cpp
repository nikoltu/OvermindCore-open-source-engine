#include "sc2/formats/minimap.hpp"

#include <cmath>
#include <stdexcept>

namespace sc2::formats {

namespace {

struct RampStop {
    float t;
    float r, g, b;
};

const RampStop kRamp[] = {
    {0.00f, 20.0f, 45.0f, 80.0f},
    {0.22f, 28.0f, 92.0f, 96.0f},
    {0.45f, 52.0f, 118.0f, 70.0f},
    {0.65f, 132.0f, 142.0f, 76.0f},
    {0.82f, 168.0f, 132.0f, 100.0f},
    {1.00f, 240.0f, 238.0f, 230.0f},
};

void ramp_color(float t, float& r, float& g, float& b) {
    const int n = int(sizeof(kRamp) / sizeof(kRamp[0]));
    if (t <= kRamp[0].t) {
        r = kRamp[0].r;
        g = kRamp[0].g;
        b = kRamp[0].b;
        return;
    }
    for (int i = 1; i < n; ++i) {
        if (t <= kRamp[i].t) {
            float f = (t - kRamp[i - 1].t) / (kRamp[i].t - kRamp[i - 1].t);
            r = kRamp[i - 1].r + (kRamp[i].r - kRamp[i - 1].r) * f;
            g = kRamp[i - 1].g + (kRamp[i].g - kRamp[i - 1].g) * f;
            b = kRamp[i - 1].b + (kRamp[i].b - kRamp[i - 1].b) * f;
            return;
        }
    }
    r = kRamp[n - 1].r;
    g = kRamp[n - 1].g;
    b = kRamp[n - 1].b;
}

uint8_t clamp8(float v) {
    int iv = int(v + 0.5f);
    return uint8_t(iv < 0 ? 0 : (iv > 255 ? 255 : iv));
}

}

std::vector<uint8_t> render_minimap(const HeightMap& hm, const std::vector<uint8_t>& data,
                                    const TerrainQuantize& q, uint32_t out_w,
                                    uint32_t out_h) {
    if (out_w == 0 || out_h == 0)
        throw std::runtime_error("minimap: zero output dimensions");
    if (hm.width < 1 || hm.height < 1 || hm.stride < 2)
        throw std::runtime_error("minimap: degenerate heightmap");

    const uint32_t W = hm.width, H = hm.height;
    const float scale = q.quantize_scale;
    const float bias = q.quantize_bias;

    auto height_at = [&](int col, int row) -> float {
        col = col < 0 ? 0 : (col >= int(W) ? int(W) - 1 : col);
        row = row < 0 ? 0 : (row >= int(H) ? int(H) - 1 : row);
        size_t rec = hm.data_offset + (size_t(row) * W + size_t(col)) * hm.stride;
        if (rec + 1 >= data.size()) return 0.0f;
        uint16_t u = uint16_t(uint16_t(data[rec]) | uint16_t(uint16_t(data[rec + 1]) << 8));
        return float(u) * scale - bias;
    };

    auto sample = [&](float fx, float fy) -> float {
        float ffx = std::floor(fx), ffy = std::floor(fy);
        int x0 = int(ffx), y0 = int(ffy);
        float tx = fx - ffx, ty = fy - ffy;
        float h00 = height_at(x0, y0), h10 = height_at(x0 + 1, y0);
        float h01 = height_at(x0, y0 + 1), h11 = height_at(x0 + 1, y0 + 1);
        float a = h00 + (h10 - h00) * tx;
        float b = h01 + (h11 - h01) * tx;
        return a + (b - a) * ty;
    };

    float zmin = 1e30f, zmax = -1e30f;
    for (uint32_t row = 0; row < H; ++row)
        for (uint32_t col = 0; col < W; ++col) {
            float z = height_at(int(col), int(row));
            zmin = z < zmin ? z : zmin;
            zmax = z > zmax ? z : zmax;
        }
    float span = zmax - zmin;
    float inv_span = span > 1e-6f ? 1.0f / span : 0.0f;

    float lx = -0.60f, ly = -0.60f, lz = 0.53f;
    float ll = std::sqrt(lx * lx + ly * ly + lz * lz);
    lx /= ll;
    ly /= ll;
    lz /= ll;
    const float kExagg = 6.0f;

    std::vector<uint8_t> out(size_t(out_w) * size_t(out_h) * 4);
    for (uint32_t py = 0; py < out_h; ++py) {
        float fy = (out_h > 1 && H > 1) ? float(py) * float(H - 1) / float(out_h - 1) : 0.0f;
        for (uint32_t px = 0; px < out_w; ++px) {
            float fx =
                (out_w > 1 && W > 1) ? float(px) * float(W - 1) / float(out_w - 1) : 0.0f;

            float z = sample(fx, fy);
            float t = (z - zmin) * inv_span;
            t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
            float r, g, b;
            ramp_color(t, r, g, b);

            float hl = sample(fx - 1.0f, fy), hr = sample(fx + 1.0f, fy);
            float hu = sample(fx, fy - 1.0f), hd = sample(fx, fy + 1.0f);
            float gx = (hr - hl) * inv_span * kExagg;
            float gy = (hd - hu) * inv_span * kExagg;
            float nx = -gx, ny = -gy, nz = 1.0f;
            float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
            float ndl = (nx * lx + ny * ly + nz * lz) / nl;
            if (ndl < 0.0f) ndl = 0.0f;
            float shade = 0.55f + 0.9f * ndl;

            r *= shade;
            g *= shade;
            b *= shade;

            size_t o = (size_t(py) * out_w + px) * 4;
            out[o + 0] = clamp8(r);
            out[o + 1] = clamp8(g);
            out[o + 2] = clamp8(b);
            out[o + 3] = 255;
        }
    }
    return out;
}

}
