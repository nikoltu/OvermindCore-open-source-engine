#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/ui/font.hpp"

namespace sc2::app::draw {

inline void px(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x, int y,
               std::uint8_t r, std::uint8_t g, std::uint8_t bl, std::uint8_t a) {
    if (x < 0 || y < 0 || x >= int(w) || y >= int(h)) return;
    std::size_t i = (std::size_t(y) * w + x) * 4;
    b[i] = r; b[i + 1] = g; b[i + 2] = bl; b[i + 3] = a;
}

inline void fill(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0,
                 int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t bl, std::uint8_t a) {
    for (int y = y0; y < y1; ++y)
        for (int x = x0; x < x1; ++x) px(b, w, h, x, y, r, g, bl, a);
}

inline void box(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x0, int y0,
                int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t bl) {
    for (int x = x0; x < x1; ++x) { px(b, w, h, x, y0, r, g, bl, 255); px(b, w, h, x, y1 - 1, r, g, bl, 255); }
    for (int y = y0; y < y1; ++y) { px(b, w, h, x0, y, r, g, bl, 255); px(b, w, h, x1 - 1, y, r, g, bl, 255); }
}

inline void text(std::vector<std::uint8_t>& b, std::uint32_t w, std::uint32_t h, int x, int y,
                 const std::string& s, int sc, std::uint8_t r, std::uint8_t g, std::uint8_t bl) {
    int pen = x;
    for (char ch : s) {
        for (int row = 0; row < ui::kFontH; ++row)
            for (int col = 0; col < ui::kFontW; ++col)
                if (ui::font_pixel(ch, col, row))
                    for (int dy = 0; dy < sc; ++dy)
                        for (int dx = 0; dx < sc; ++dx) {
                            px(b, w, h, pen + col * sc + dx + sc, y + row * sc + dy + sc, 0, 0, 0, 255);
                            px(b, w, h, pen + col * sc + dx, y + row * sc + dy, r, g, bl, 255);
                        }
        pen += (ui::kFontW + 1) * sc;
    }
}

inline int text_w(const std::string& s, int sc) {
    return int(s.size()) * (ui::kFontW + 1) * sc;
}

}
