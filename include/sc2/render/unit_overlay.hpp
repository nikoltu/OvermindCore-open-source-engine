#pragma once

#include <cstdint>
#include <vector>

#include "sc2/render/mat4.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::render {

class UnitOverlay {
public:
    static constexpr int kRingBaseRadius = 12;
    static constexpr int kRingRadiusPerWorld = 6;
    static constexpr int kRingMaxRadius = 48;
    static constexpr float kRingThickness = 1.3f;

    static constexpr int kBarWidth = 24;
    static constexpr int kBarHeight = 4;
    static constexpr int kBarOffsetY = 16;

    UnitOverlay() = default;

    std::vector<uint8_t> render(const sim::World& w, const Mat4& mvp, uint32_t view_w,
                                uint32_t view_h, const std::vector<uint32_t>& selection,
                                int viewer_player) const;

    struct StatusBarOptions {
        enum class Mode : uint8_t { Normal, Targeting, Selected, Damaged, Always };
        Mode mode = Mode::Normal;
        bool reveal_all = false;
        bool team_colored = false;
        int viewer_player = -1;
        const std::vector<uint32_t>* selection = nullptr;
        const std::vector<float>* anchor_z = nullptr;
        const std::vector<float>* life_trail = nullptr;
        const std::vector<float>* shield_trail = nullptr;
        bool own_rows = false;
        const std::vector<float>* bar_width = nullptr;
        const std::vector<float>* bar_offset = nullptr;
        const std::vector<std::uint8_t>* cargo_ok = nullptr;
        float px_scale = 1.0f;
    };
    static constexpr int kStatusBarWidth = 60;
    static constexpr int kStatusBarHeight = 7;
    static constexpr int kStatusBarDistance = 34;
    static constexpr int kStatusSegmentValue = 25;
    std::vector<uint8_t> render_status(const sim::World& w, const Mat4& mvp, uint32_t view_w, uint32_t view_h,
                                       const StatusBarOptions& o) const;
};

}
