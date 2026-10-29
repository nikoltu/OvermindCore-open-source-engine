#pragma once

#include <algorithm>
#include <cstdint>

namespace omc::render {

enum class ShadowQuality : std::uint8_t { Full = 0, Units = 1, Off = 2 };
enum class TerrainQuality : std::uint8_t { High = 0, Medium = 1, Low = 2 };
enum class DoodadDensity : std::uint8_t { All = 0, Medium = 1, Low = 2 };

struct RenderQuality {
    ShadowQuality shadows = ShadowQuality::Full;
    int scale_pct = 100;
    std::uint32_t shadow_dim = 2048;
    TerrainQuality terrain = TerrainQuality::High;
    DoodadDensity doodads = DoodadDensity::All;
};

inline RenderQuality& render_quality() {
    static RenderQuality q;
    return q;
}

inline std::uint32_t scaled_extent(std::uint32_t full, int pct) {
    const int p = std::clamp(pct, 25, 100);
    return std::max<std::uint32_t>(1, std::uint32_t((std::uint64_t(full) * std::uint64_t(p) + 50) / 100));
}

}
