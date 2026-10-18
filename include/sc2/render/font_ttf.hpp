#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sc2/render/font_cff.hpp"

namespace sc2::render {

struct GlyphBitmap {
    int width = 0;
    int height = 0;
    int bearing_x = 0;
    int bearing_y = 0;
    int advance = 0;
    std::vector<std::uint8_t> coverage;
};

class OverlayFont {
public:
    bool load(const std::vector<std::uint8_t>& sfnt);
    bool valid() const { return valid_; }

    const GlyphBitmap& glyph(char ch, int pixel_height) const;

    static constexpr int kFractionalBase = 1 << 20;
    static int fractional(double px) { return px > 0.0 ? kFractionalBase + int(px * 64.0 + 0.5) : 0; }
    static double height_px(int pixel_height) {
        return pixel_height >= kFractionalBase ? double(pixel_height - kFractionalBase) / 64.0 : double(pixel_height);
    }

    int advance_px(char ch, int pixel_height) const { return glyph(ch, pixel_height).advance; }

    int ascent_px(int pixel_height) const;
    int line_height_px(int pixel_height) const;

    int units_per_em() const { return units_per_em_; }
    int raw_advance_units(char ch) const;

private:
    struct Point { float x, y; bool on; };
    struct Outline { std::vector<std::vector<Point>> contours; };

    bool valid_ = false;
    int units_per_em_ = 0;
    int num_glyphs_ = 0;
    int ascent_ = 0, descent_ = 0, line_gap_ = 0;
    int num_h_metrics_ = 0;
    int loc_long_ = 0;
    bool is_cff_ = false;
    std::shared_ptr<CffFont> cff_;
    std::vector<std::uint32_t> loca_;
    std::vector<std::uint8_t> glyf_;
    std::vector<std::uint16_t> advance_w_;
    std::uint16_t last_advance_ = 0;
    std::unordered_map<std::uint32_t, std::uint16_t> cmap_;

    mutable std::unordered_map<int, Outline> outline_cache_;
    mutable std::map<std::pair<int, int>, GlyphBitmap> raster_cache_;

    std::uint16_t glyph_id(std::uint32_t cp) const;
    std::uint16_t advance_units(int gid) const;
    const Outline& outline(int gid) const;
    void build_outline(int gid, Outline& out, int depth) const;
};

}
