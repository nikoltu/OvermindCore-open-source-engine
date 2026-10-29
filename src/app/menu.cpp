#include "omc/app/menu.hpp"

#include <cctype>
#include <exception>
#include <string>
#include <vector>

#include "omc/app/glue_screen.hpp"
#include "omc/casc/storage.hpp"
#include "omc/ui/interact.hpp"

namespace omc::app {

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
    return add(name, rect, rect);
}

MainMenu& MainMenu::add(const std::string& name, const ui::Rect& body, const ui::Rect& hit) {
    ui::SolvedFrame f;
    f.rect = body;
    f.type = "Button";
    f.name = name;
    buttons_.push_back(std::move(f));
    hits_.push_back(hit);
    return *this;
}

MainMenu MainMenu::from_buttons(const std::vector<Button>& buttons) {
    MainMenu m;
    m.buttons_.reserve(buttons.size());
    for (const Button& b : buttons) m.add(b.name, b.rect);
    return m;
}

std::optional<MainMenu> MainMenu::from_store(casc::Storage& store, const std::string& layout_path,
                                             const ui::Rect& canvas) {
    std::string rel = layout_path;
    const std::string root = kGlueLayoutRoot;
    if (rel.size() > root.size() && rel.compare(0, root.size(), root) == 0)
        rel = rel.substr(root.size());
    std::optional<GlueScreen> screen = GlueScreen::harvest(store, {rel}, canvas);
    if (!screen) return std::nullopt;
    MainMenu m;
    for (std::size_t i = 0; i < screen->controls().size(); ++i)
        m.add(screen->controls()[i].name, screen->controls()[i].rect, screen->hit_rect(i));
    return m;
}

std::optional<MainMenu> MainMenu::from_install(const std::string& data_dir,
                                               const std::string& layout_path,
                                               const ui::Rect& canvas) {
    try {
        casc::Storage store;
        store.open(data_dir);
        return from_store(store, layout_path, canvas);
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
    for (std::size_t n = hits_.size(); n-- > 0;) {
        const ui::Rect& r = hits_[n];
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return classify(buttons_[n].name);
    }
    return MenuAction::None;
}

}
