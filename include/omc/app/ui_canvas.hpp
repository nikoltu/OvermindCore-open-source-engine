#pragma once

#include <cstdint>

namespace omc::app {

inline constexpr float kRefHeight = 1200.0f;

inline constexpr float kAspectBands[] = {1.33f, 1.4f, 1.44f, 1.5f, 1.51f, 1.6f, 1.7f, 1.75f, 1.77f};
inline constexpr int kAspectBandCount = int(sizeof(kAspectBands) / sizeof(kAspectBands[0]));

struct Canvas {
    std::uint32_t phys_w = 0, phys_h = 0;
    float ref_height = kRefHeight;
    float scale = 1.0f;
    float virt_w = 0.0f, virt_h = 0.0f;
    float aspect = 1.0f;
};

inline Canvas make_canvas(std::uint32_t phys_w, std::uint32_t phys_h,
                          float ref_height = kRefHeight) {
    Canvas c;
    c.phys_w = phys_w;
    c.phys_h = phys_h;
    c.ref_height = ref_height;
    c.scale = (phys_h > 0 && ref_height > 0.0f) ? float(phys_h) / ref_height : 1.0f;
    c.virt_h = (ref_height > 0.0f) ? ref_height : float(phys_h);
    c.virt_w = (c.scale > 0.0f) ? float(phys_w) / c.scale : float(phys_w);
    c.aspect = (phys_h > 0) ? float(phys_w) / float(phys_h) : 1.0f;
    return c;
}

inline constexpr float kUiScaleDefault = 1.0f;

inline float scaled_ref_height(float base_ref_height, float ui_scale) {
    return (ui_scale > 0.0f) ? base_ref_height / ui_scale : base_ref_height;
}

inline Canvas make_canvas_scaled(std::uint32_t phys_w, std::uint32_t phys_h, float base_ref_height,
                                 float ui_scale) {
    return make_canvas(phys_w, phys_h, scaled_ref_height(base_ref_height, ui_scale));
}

inline float virt_to_phys(const Canvas& c, float v) { return v * c.scale; }
inline float phys_to_virt(const Canvas& c, float p) { return (c.scale > 0.0f) ? p / c.scale : p; }

inline int aspect_band(float aspect) {
    int band = -1;
    for (int i = 0; i < kAspectBandCount; ++i) {
        if (aspect >= kAspectBands[i])
            band = i;
        else
            break;
    }
    return band;
}

}
