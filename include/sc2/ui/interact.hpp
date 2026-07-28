#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "sc2/ui/layout.hpp"

namespace sc2::ui {

inline constexpr std::size_t kNoFrame = static_cast<std::size_t>(-1);

enum class WidgetState : std::uint8_t { Normal, Hover, Pressed };

bool is_interactive(const SolvedFrame& f);

std::string press_action(const SolvedFrame& f);

bool rect_contains(const Rect& r, float x, float y);

std::size_t hit_test(const std::vector<SolvedFrame>& frames, float x, float y);

const SolvedFrame* hit_test_frame(const std::vector<SolvedFrame>& frames, float x, float y);

class Interactor {
public:
    Interactor() = default;
    explicit Interactor(std::vector<SolvedFrame> frames);

    void set_frames(std::vector<SolvedFrame> frames);
    const std::vector<SolvedFrame>& frames() const { return frames_; }

    std::size_t hit_test(float x, float y) const;

    std::size_t move(float x, float y);

    std::string press(float x, float y);

    bool release(float x, float y);

    WidgetState state(std::size_t i) const;
    std::size_t hovered() const { return hover_; }
    std::size_t captured() const { return capture_; }
    bool button_down() const { return down_; }

private:
    void recompute();

    std::vector<SolvedFrame> frames_;
    std::vector<WidgetState> states_;
    std::size_t hover_ = kNoFrame;
    std::size_t capture_ = kNoFrame;
    bool down_ = false;
};

}
