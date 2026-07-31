#pragma once

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

#include "sc2/ui/layout.hpp"

namespace sc2::app {

enum class MenuAction { None, StartSkirmish, Quit };

class MainMenu {
public:
    struct Button {
        std::string name;
        ui::Rect rect;
    };

    MainMenu() = default;

    MainMenu& add(const std::string& name, const ui::Rect& rect);

    static MainMenu from_buttons(const std::vector<Button>& buttons);

    static std::optional<MainMenu> from_install(const std::string& data_dir,
                                                const std::string& layout_path,
                                                const ui::Rect& canvas);

    MenuAction click(float x, float y) const;

    static MenuAction classify(const std::string& name);

    std::size_t button_count() const { return buttons_.size(); }

    const std::vector<ui::SolvedFrame>& buttons() const { return buttons_; }

private:
    std::vector<ui::SolvedFrame> buttons_;
};

}
