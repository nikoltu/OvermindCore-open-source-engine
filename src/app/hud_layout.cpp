#include "sc2/app/hud_layout.hpp"

#include <cmath>
#include <exception>
#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "sc2/app/ui_layout_source.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/ui/layout.hpp"

namespace sc2::app {

namespace {

HudAuthoredRects::R find_rect(const std::vector<ui::SolvedFrame>& solved,
                              const std::string& type, float cw, float ch) {
    for (const ui::SolvedFrame& f : solved) {
        if (f.type != type) continue;
        if (f.rect.w <= 0 || f.rect.h <= 0) continue;
        if (f.rect.w > cw || f.rect.h > ch) continue;
        return {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
    }
    return {};
}

}

const HudAuthoredRects* hud_authored_rects(casc::Storage& store, float aspect) {
    if (!(aspect > 0.1f) || aspect > 10.0f) return nullptr;
    constexpr float kAspectSteps = 4096.0f;
    const int akey = int(std::lround(aspect * kAspectSteps));
    static std::map<std::pair<const casc::Storage*, int>, std::unique_ptr<HudAuthoredRects>> cache;
    auto key = std::make_pair(static_cast<const casc::Storage*>(&store), akey);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second && it->second->ok ? it->second.get() : nullptr;

    auto& slot = cache[key];
    const ui::LayoutSet* set = layout_namespace(store);
    if (!set) return nullptr;
    const ui::Frame* gui = set->find_template("GameUI/GameUI");
    if (!gui) return nullptr;
    try {
        const float ch = kRefHeight;
        const float cw = ch * (float(akey) / kAspectSteps);
        ui::Frame inst = set->instantiate(*gui);
        static const char* const kMeleeCollapsed[] = {"CashPanel", "TeamResourceButton"};
        auto melee_collapsed = [](const ui::Frame& f) {
            for (const char* n : kMeleeCollapsed)
                if (f.name == n) return true;
            return false;
        };
        std::vector<ui::SolvedFrame> solved =
            ui::solve_layout(inst, ui::Rect{0, 0, cw, ch}, ui::TextResolver{}, melee_collapsed);
        auto r = std::make_unique<HudAuthoredRects>();
        r->canvas_w = cw;
        r->minimap = find_rect(solved, "Minimap", cw, ch);
        r->minimap_panel = find_rect(solved, "MinimapPanel", cw, ch);
        if (!r->minimap.ok()) r->minimap = r->minimap_panel;
        r->control_group = find_rect(solved, "ControlGroupPanel", cw, ch);
        r->alert = find_rect(solved, "AlertPanel", cw, ch);
        r->command = find_rect(solved, "CommandPanel", cw, ch);
        r->info = find_rect(solved, "InfoPanel", cw, ch);
        r->portrait = find_rect(solved, "PortraitPanel", cw, ch);
        r->resource = find_rect(solved, "ResourcePanel", cw, ch);
        if (r->command.ok()) {
            int found = 0;
            for (const ui::SolvedFrame& f : solved) {
                if (f.name.size() != 15 || f.name.compare(0, 13, "CommandButton") != 0) continue;
                const char a = f.name[13], b = f.name[14];
                if (a < '0' || a > '9' || b < '0' || b > '9') continue;
                const int idx = (a - '0') * 10 + (b - '0');
                if (idx >= 15 || f.rect.w <= 0 || f.rect.h <= 0) continue;
                const float cx = f.rect.x + f.rect.w * 0.5f, cy = f.rect.y + f.rect.h * 0.5f;
                if (cx < r->command.x0 || cx > r->command.x1 || cy < r->command.y0 ||
                    cy > r->command.y1)
                    continue;
                if (!r->buttons[idx].ok()) ++found;
                r->buttons[idx] = {f.rect.x, f.rect.y, f.rect.x + f.rect.w,
                                   f.rect.y + f.rect.h};
            }
            r->buttons_ok = found == 15;
        }
        r->ok = r->minimap.ok() && r->command.ok() && r->info.ok();
        slot = std::move(r);
    } catch (const std::exception&) {
        return nullptr;
    }
    return slot && slot->ok ? slot.get() : nullptr;
}

}
