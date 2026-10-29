#include "omc/render/fog.hpp"

namespace omc::render {

std::vector<uint8_t> FogOverlay::mask(const sim::VisionGrid& vis) const {
    const int w = vis.width();
    const int h = vis.height();
    std::vector<uint8_t> out;
    if (w <= 0 || h <= 0) return out;
    out.resize(size_t(w) * size_t(h));
    for (int cy = 0; cy < h; ++cy) {
        for (int cx = 0; cx < w; ++cx) {
            uint8_t a;
            switch (vis.state(cx, cy)) {
                case sim::Visibility::Visible:
                    a = kVisibleAlpha;
                    break;
                case sim::Visibility::Explored:
                    a = explored_alpha_;
                    break;
                case sim::Visibility::Hidden:
                default:
                    a = kUnexploredAlpha;
                    break;
            }
            out[size_t(cy) * size_t(w) + size_t(cx)] = a;
        }
    }
    return out;
}

std::vector<uint8_t> FogOverlay::render(const sim::VisionGrid& vis, uint32_t out_w,
                                        uint32_t out_h) const {
    std::vector<uint8_t> rgba(size_t(out_w) * size_t(out_h) * 4, 0);

    const int w = vis.width();
    const int h = vis.height();
    if (w <= 0 || h <= 0 || out_w == 0 || out_h == 0) {
        for (size_t p = 3; p < rgba.size(); p += 4) rgba[p] = kUnexploredAlpha;
        return rgba;
    }

    const std::vector<uint8_t> cell_alpha = mask(vis);
    for (uint32_t py = 0; py < out_h; ++py) {
        const int cy = int(uint64_t(py) * uint64_t(h) / uint64_t(out_h));
        const size_t cell_row = size_t(cy) * size_t(w);
        const size_t pix_row = size_t(py) * size_t(out_w);
        for (uint32_t px = 0; px < out_w; ++px) {
            const int cx = int(uint64_t(px) * uint64_t(w) / uint64_t(out_w));
            rgba[(pix_row + size_t(px)) * 4 + 3] = cell_alpha[cell_row + size_t(cx)];
        }
    }
    return rgba;
}

}
