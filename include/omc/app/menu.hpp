#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "omc/ui/layout.hpp"

namespace omc::casc {
class Storage;
}

namespace omc::app {

enum class MenuAction { None, StartSkirmish, Quit };

class MainMenu {
public:
    struct Button {
        std::string name;
        ui::Rect rect;
    };

    MainMenu() = default;

    MainMenu& add(const std::string& name, const ui::Rect& rect);

    MainMenu& add(const std::string& name, const ui::Rect& body, const ui::Rect& hit);

    static MainMenu from_buttons(const std::vector<Button>& buttons);

    static std::optional<MainMenu> from_install(const std::string& data_dir,
                                                const std::string& layout_path,
                                                const ui::Rect& canvas);

    static std::optional<MainMenu> from_store(casc::Storage& store, const std::string& layout_path,
                                              const ui::Rect& canvas);

    MenuAction click(float x, float y) const;

    static MenuAction classify(const std::string& name);

    std::size_t button_count() const { return buttons_.size(); }

    const std::vector<ui::SolvedFrame>& buttons() const { return buttons_; }

    const ui::Rect& hit_rect(std::size_t i) const { return hits_[i]; }

    void scale(float s) {
        for (ui::SolvedFrame& b : buttons_) b.rect = {b.rect.x * s, b.rect.y * s, b.rect.w * s, b.rect.h * s};
        for (ui::Rect& h : hits_) h = {h.x * s, h.y * s, h.w * s, h.h * s};
    }

private:
    std::vector<ui::SolvedFrame> buttons_;
    std::vector<ui::Rect> hits_;
};

}
