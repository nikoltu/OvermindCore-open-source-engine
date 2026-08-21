#include "sc2/app/menu.hpp"

#include <cctype>
#include <exception>
#include <string>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/data/strings.hpp"
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
        std::string prefix;
        if (std::size_t p = layout_path.find("base.sc2data/"); p != std::string::npos)
            prefix = layout_path.substr(0, p + 13);
        try {
            auto di = store.read_by_name(prefix + "UI/Layout/DescIndex.SC2Layout");
            for (const std::string& inc : ui::LayoutSet::parse_includes(std::string(di.begin(), di.end()))) {
                try {
                    auto b = store.read_by_name(prefix + inc);
                    set.add_xml(inc, std::string(b.begin(), b.end()));
                } catch (const std::exception&) {   }
            }
        } catch (const std::exception&) {   }

        ui::Layout screen = ui::parse_layout(xml);
        set.add(layout_path, screen);

        static const char* kMods[] = {"core",  "liberty",    "libertymulti", "swarm",
                                      "swarmmulti", "void",   "voidmulti"};
        data::StringTable strings;
        for (const char* mod : kMods) {
            try {
                strings.load(store.read_by_name(std::string("mods/") + mod +
                             ".sc2mod/enus.sc2data/LocalizedData/GameStrings.txt"));
            } catch (const std::exception&) {   }
        }
        ui::TextResolver resolve_text = [&strings](const std::string& ref) { return strings.resolve(ref); };

        const float cx0 = canvas.x, cy0 = canvas.y, cx1 = canvas.x + canvas.w, cy1 = canvas.y + canvas.h;
        MainMenu m;
        for (const ui::Frame& top : screen.frames) {
            ui::Frame inst = set.instantiate(top);
            for (const ui::SolvedFrame& sf : ui::solve_layout(inst, canvas, resolve_text)) {
                if (!ui::is_interactive(sf) || sf.name.empty()) continue;
                const float mx = sf.rect.x + sf.rect.w * 0.5f, my = sf.rect.y + sf.rect.h * 0.5f;
                if (mx < cx0 || mx > cx1 || my < cy0 || my > cy1) continue;
                if (sf.rect.w >= canvas.w || sf.rect.h >= canvas.h) continue;
                m.add(sf.name, sf.rect);
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
