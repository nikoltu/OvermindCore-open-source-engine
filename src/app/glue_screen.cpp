#include "omc/app/glue_screen.hpp"

#include <exception>

#include "omc/app/ui_layout_source.hpp"
#include "omc/casc/storage.hpp"
#include "omc/data/melee_mods.hpp"
#include "omc/data/strings.hpp"
#include "omc/ui/interact.hpp"

namespace omc::app {

std::optional<GlueScreen> GlueScreen::harvest(casc::Storage& store,
                                              const std::vector<std::string>& layout_paths,
                                              const ui::Rect& canvas) {
    return harvest(store, layout_paths, canvas, HarvestOptions{});
}

std::optional<GlueScreen> GlueScreen::harvest(casc::Storage& store,
                                              const std::vector<std::string>& layout_paths,
                                              const ui::Rect& canvas, const HarvestOptions& opts) {
    if (layout_paths.empty() || canvas.w <= 0.0f || canvas.h <= 0.0f) return std::nullopt;
    try {
        const ui::LayoutSet* shared = layout_namespace(store, kCoreLayoutRoot);
        ui::LayoutSet set = shared ? *shared : ui::LayoutSet{};

        std::vector<ui::Layout> screens;
        std::vector<std::string> names;
        for (std::size_t di = 0; di < layout_paths.size(); ++di) {
            const std::string& rel = layout_paths[di];
            const std::string path = std::string(kGlueLayoutRoot) + rel;
            try {
                std::vector<std::uint8_t> bytes = store.read_by_name(path);
                std::string xml(bytes.begin(), bytes.end());
                if (di >= opts.screen_docs) {
                    set.add_xml(path, xml);
                    continue;
                }
                ui::Layout doc = ui::parse_layout(xml);
                bool all_templates = di > 0 && !doc.frames.empty();
                for (const ui::Frame& top : doc.frames)
                    if (top.name.find("Template") == std::string::npos) { all_templates = false; break; }
                if (all_templates) {
                    set.add(path, std::move(doc));
                    continue;
                }
                screens.push_back(std::move(doc));
                names.push_back(path);
            } catch (const std::exception&) {
            }
        }
        if (screens.empty()) return std::nullopt;
        for (std::size_t i = 0; i < screens.size(); ++i) set.add(names[i], screens[i]);

        data::StringTable strings;
        for (const char* mod : data::kMeleeMods) {
            try {
                strings.load(store.read_by_name(std::string("mods/") + mod +
                                                ".sc2mod/enus.sc2data/LocalizedData/GameStrings.txt"));
            } catch (const std::exception&) {
            }
        }
        ui::TextResolver resolve_text = [&strings](const std::string& ref) {
            return strings.resolve(ref);
        };

        const float cx0 = canvas.x, cy0 = canvas.y;
        const float cx1 = canvas.x + canvas.w, cy1 = canvas.y + canvas.h;
        GlueScreen out;
        out.documents_ = int(screens.size());
        for (const ui::Layout& screen : screens)
            for (const ui::Frame& top : screen.frames) {
                ui::Frame inst = set.instantiate(top);
                const std::vector<ui::SolvedFrame> solved =
                    ui::solve_layout(inst, canvas, resolve_text, ui::FrameCollapsed{}, opts.data);
                auto hidden_offline = [](const std::string& v) {
                    return v == "False" || v == "false" || v == "{$BattleUI/@IsOnline}";
                };
                int hidden_below = -1;
                int closed_below = -1;
                for (std::size_t i = 0; i < solved.size(); ++i) {
                    const ui::SolvedFrame& sf = solved[i];
                    if (hidden_below >= 0 && sf.depth <= hidden_below) hidden_below = -1;
                    if (closed_below >= 0 && sf.depth <= closed_below) closed_below = -1;
                    if (hidden_below < 0 && hidden_offline(sf.visible)) {
                        bool shown = false;
                        for (const std::string& n : opts.shown)
                            if (n == sf.name) { shown = true; break; }
                        if (!shown) hidden_below = sf.depth;
                    }
                    if (hidden_below >= 0 || closed_below >= 0) continue;
                    const bool pulldown = sf.type.size() >= 8 && sf.type.compare(sf.type.size() - 8, 8, "Pulldown") == 0;
                    if (pulldown) closed_below = sf.depth;
                    if (!ui::is_interactive(sf) || sf.name.empty()) continue;
                    ui::Rect hit = sf.rect;
                    for (std::size_t j = i + 1; j < solved.size() && solved[j].depth > sf.depth; ++j)
                        if (solved[j].name == "HitTestFrame") {
                            hit = solved[j].rect;
                            break;
                        }
                    const float mx = hit.x + hit.w * 0.5f, my = hit.y + hit.h * 0.5f;
                    if (mx < cx0 || mx > cx1 || my < cy0 || my > cy1) continue;
                    if (hit.w >= canvas.w || hit.h >= canvas.h) continue;
                    out.controls_.push_back(sf);
                    out.hits_.push_back(hit);
                }
            }
        return out;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::size_t GlueScreen::hit_index(float x, float y) const {
    for (std::size_t n = hits_.size(); n-- > 0;) {
        const ui::Rect& r = hits_[n];
        if (x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h) return n;
    }
    return controls_.size();
}

const ui::SolvedFrame* GlueScreen::hit(float x, float y) const {
    const std::size_t i = hit_index(x, y);
    return i < controls_.size() ? &controls_[i] : nullptr;
}

}
