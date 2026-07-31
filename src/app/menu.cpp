#include "sc2/app/menu.hpp"

#include <cctype>
#include <exception>
#include <string>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/ui/interact.hpp"

namespace sc2::app {

namespace {

std::string to_lower(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
    return out;
}

bool contains(const std::string& hay, const char* needle) {
    return hay.find(needle) != std::string::npos;
}

}

MainMenu& MainMenu::add(const std::string& name, const ui::Rect& rect) {
    ui::SolvedFrame f;
    f.rect = rect;
    f.type = "Button";
    f.name = name;
    buttons_.push_back(std::move(f));
    return *this;
}

MainMenu MainMenu::from_buttons(const std::vector<Button>& buttons) {
    MainMenu m;
    m.buttons_.reserve(buttons.size());
    for (const Button& b : buttons) m.add(b.name, b.rect);
    return m;
}

std::optional<MainMenu> MainMenu::from_install(const std::string& data_dir,
                                               const std::string& layout_path,
                                               const ui::Rect& canvas) {
    try {
        casc::Storage store;
        store.open(data_dir);

        std::vector<uint8_t> bytes = store.read_by_name(layout_path);
        std::string xml(bytes.begin(), bytes.end());

        ui::LayoutSet set;
        set.add_xml(layout_path, xml);
        if (set.docs.empty()) return std::nullopt;

        MainMenu m;
        for (const ui::Frame& top : set.docs.front().layout.frames) {
            ui::Frame inst = set.instantiate(top);
            for (const ui::SolvedFrame& sf : ui::solve_layout(inst, canvas)) {
                if (ui::is_interactive(sf) && !sf.name.empty()) m.add(sf.name, sf.rect);
            }
        }
        return m;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

MenuAction MainMenu::classify(const std::string& name) {
    const std::string n = to_lower(name);
    if (contains(n, "play") || contains(n, "singleplayer") || contains(n, "campaign") ||
        contains(n, "skirmish"))
        return MenuAction::StartSkirmish;
    if (contains(n, "exit") || contains(n, "quit")) return MenuAction::Quit;
    return MenuAction::None;
}

MenuAction MainMenu::click(float x, float y) const {
    std::size_t hit = ui::hit_test(buttons_, x, y);
    if (hit == ui::kNoFrame) return MenuAction::None;
    return classify(buttons_[hit].name);
}

}
