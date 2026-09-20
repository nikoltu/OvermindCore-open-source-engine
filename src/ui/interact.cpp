#include "sc2/ui/interact.hpp"

#include <string>

namespace sc2::ui {

namespace {
bool ends_with(const std::string& s, const char* suffix) {
    const std::size_t n = std::char_traits<char>::length(suffix);
    return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}
bool xml_true(const std::string& v) {
    return v == "True" || v == "true" || v == "1" || v == "TRUE";
}
}

bool interactive_type(const std::string& type) {
    static const char* const kFamilies[] = {"Button",  "CheckBox", "Pulldown", "EditBox",  "ScrollBar", "Slider", "ListBox", "ListItem",
                                            "TabControl", "ComboBox", "Toggle", "Link", "DropDown", "Spinner", "Radio"};
    for (const char* fam : kFamilies)
        if (ends_with(type, fam)) return true;
    return false;
}

bool is_interactive(const SolvedFrame& f) {
    if (!f.accepts_mouse.empty()) return xml_true(f.accepts_mouse);
    return interactive_type(f.type);
}

std::string press_action(const SolvedFrame& f) {
    return is_interactive(f) ? f.name : std::string();
}

bool rect_contains(const Rect& r, float x, float y) {
    return x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
}

std::size_t hit_test(const std::vector<SolvedFrame>& frames, float x, float y) {
    for (std::size_t n = frames.size(); n-- > 0;)
        if (rect_contains(frames[n].rect, x, y)) return n;
    return kNoFrame;
}

const SolvedFrame* hit_test_frame(const std::vector<SolvedFrame>& frames, float x, float y) {
    std::size_t i = hit_test(frames, x, y);
    return i == kNoFrame ? nullptr : &frames[i];
}

Interactor::Interactor(std::vector<SolvedFrame> frames) {
    set_frames(std::move(frames));
}

void Interactor::set_frames(std::vector<SolvedFrame> frames) {
    frames_ = std::move(frames);
    states_.assign(frames_.size(), WidgetState::Normal);
    hover_ = kNoFrame;
    capture_ = kNoFrame;
    down_ = false;
}

std::size_t Interactor::hit_test(float x, float y) const {
    return ui::hit_test(frames_, x, y);
}

static std::size_t hovered_widget(const std::vector<SolvedFrame>& frames, std::size_t hit) {
    if (hit == kNoFrame) return kNoFrame;
    return is_interactive(frames[hit]) ? hit : kNoFrame;
}

void Interactor::recompute() {
    for (std::size_t i = 0; i < states_.size(); ++i) {
        if (down_ && i == capture_ && hover_ == capture_)
            states_[i] = WidgetState::Pressed;
        else if (!down_ && i == hover_)
            states_[i] = WidgetState::Hover;
        else
            states_[i] = WidgetState::Normal;
    }
}

std::size_t Interactor::move(float x, float y) {
    std::size_t hit = hit_test(x, y);
    hover_ = hovered_widget(frames_, hit);
    recompute();
    return hit;
}

std::string Interactor::press(float x, float y) {
    std::size_t hit = hit_test(x, y);
    hover_ = hovered_widget(frames_, hit);
    capture_ = hover_;
    down_ = true;
    recompute();
    return capture_ == kNoFrame ? std::string() : frames_[capture_].name;
}

bool Interactor::release(float x, float y) {
    std::size_t hit = hit_test(x, y);
    bool click = capture_ != kNoFrame && hit == capture_;
    down_ = false;
    capture_ = kNoFrame;
    hover_ = hovered_widget(frames_, hit);
    recompute();
    return click;
}

WidgetState Interactor::state(std::size_t i) const {
    return i < states_.size() ? states_[i] : WidgetState::Normal;
}

}
