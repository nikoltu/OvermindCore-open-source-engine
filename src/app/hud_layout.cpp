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

const HudPauseRects* hud_pause_rects(casc::Storage& store, float aspect) {
    if (!(aspect > 0.1f) || aspect > 10.0f) return nullptr;
    constexpr float kAspectSteps = 4096.0f;
    const int akey = int(std::lround(aspect * kAspectSteps));
    static std::map<std::pair<const casc::Storage*, int>, std::unique_ptr<HudPauseRects>> cache;
    auto key = std::make_pair(static_cast<const casc::Storage*>(&store), akey);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second && it->second->ok ? it->second.get() : nullptr;
    if (cache.size() >= 32) cache.clear();
    auto& slot = cache[key];
    const ui::LayoutSet* set = layout_namespace(store);
    if (!set) return nullptr;
    const ui::Frame* dlg = set->find_template("GameMenuDialog/GameMenuDialogTemplate");
    if (!dlg) return nullptr;
    try {
        const float ch = kRefHeight;
        const float cw = ch * (float(akey) / kAspectSteps);
        ui::Frame inst = set->instantiate(*dlg);
        const std::vector<ui::SolvedFrame> solved = ui::solve_layout(inst, ui::Rect{0, 0, cw, ch});
        auto r = std::make_unique<HudPauseRects>();
        r->canvas_w = cw;
        std::size_t i0 = solved.size();
        for (std::size_t i = 0; i < solved.size(); ++i)
            if (solved[i].name == "MainPanel") { i0 = i; break; }
        if (i0 < solved.size()) {
            const int d0 = solved[i0].depth;
            const ui::SolvedFrame& mp = solved[i0];
            r->panel = {mp.rect.x, mp.rect.y, mp.rect.x + mp.rect.w, mp.rect.y + mp.rect.h};
            for (std::size_t i = i0 + 1; i < solved.size() && solved[i].depth > d0; ++i) {
                const ui::SolvedFrame& f = solved[i];
                if (f.name != "Button" || f.rect.w <= 0 || f.rect.h <= 0) continue;
                if (f.parent == "ReturnButton" && !r->resume.ok()) r->resume = {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
                else if (f.parent == "QuitButton" && !r->quit.ok()) r->quit = {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
                else if (f.parent == "SaveButton" && !r->save.ok()) r->save = {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
                else if (f.parent == "LoadButton" && !r->load.ok()) r->load = {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
            }
        }
        if (r->panel.ok() && r->resume.ok() && r->quit.ok() && std::fabs(r->resume.y0 - r->quit.y0) < 1.0f) {
            const float row = 70.0f, top = r->panel.y0 + 86.0f;
            auto place = [&](HudPauseRects::R& b, int index) {
                const float h = b.y1 - b.y0;
                b.y0 = top + row * float(index) + 8.0f;
                b.y1 = b.y0 + h;
            };
            int index = 0;
            if (r->load.ok() || r->save.ok()) {
                if (r->load.ok()) place(r->load, index);
                if (r->save.ok()) place(r->save, index);
                ++index;
            }
            place(r->quit, index++);
            place(r->resume, index++);
        }
        r->ok = r->panel.ok() && r->resume.ok() && r->quit.ok();
        slot = std::move(r);
    } catch (const std::exception&) {
        slot.reset();
    }
    return slot && slot->ok ? slot.get() : nullptr;
}

const HudAuthoredRects* hud_authored_rects(casc::Storage& store, float aspect) {
    if (!(aspect > 0.1f) || aspect > 10.0f) return nullptr;
    constexpr float kAspectSteps = 4096.0f;
    const int akey = int(std::lround(aspect * kAspectSteps));
    static std::map<std::pair<const casc::Storage*, int>, std::unique_ptr<HudAuthoredRects>> cache;
    auto key = std::make_pair(static_cast<const casc::Storage*>(&store), akey);
    auto it = cache.find(key);
    if (it != cache.end()) return it->second && it->second->ok ? it->second.get() : nullptr;
    if (cache.size() >= 32) cache.clear();

    auto& slot = cache[key];
    const ui::LayoutSet* set = layout_namespace(store);
    if (!set) return nullptr;
    const ui::Frame* gui = set->find_template("GameUI/GameUI");
    if (!gui) return nullptr;
    try {
        const float ch = kRefHeight;
        const float cw = ch * (float(akey) / kAspectSteps);
        ui::Frame inst = set->instantiate(*gui);
        static const char* const kMeleeCollapsed[] = {
            "CashPanel",         "TeamResourceButton",     "ResourceLabel2",
            "ResourceLabel3",    "ResourceIconBackground2", "ResourceIconBackground3",
            "ResourceIcon2",     "ResourceIcon3",          "UpkeepLabel"};
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
        r->idle_button = find_rect(solved, "IdleButton", cw, ch);
        r->equipment = find_rect(solved, "EquipmentPanel", cw, ch);
        {
            int found = 0;
            for (const ui::SolvedFrame& f : solved) {
                if (f.name.compare(0, 11, "QueueButton") != 0) continue;
                const std::string digits = f.name.substr(11);
                if (digits.empty() || digits.size() > 2) continue;
                int idx = 0;
                bool numeric = true;
                for (char ch : digits) {
                    if (ch < '0' || ch > '9') { numeric = false; break; }
                    idx = idx * 10 + (ch - '0');
                }
                if (!numeric || idx >= 8 || f.rect.w <= 0 || f.rect.h <= 0) continue;
                if (!r->queue_slots[idx].ok()) ++found;
                r->queue_slots[idx] = {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
            }
            r->queue_slots_ok = found > 0;
        }
        {
            int found = 0;
            for (const ui::SolvedFrame& f : solved) {
                if (f.name.compare(0, 11, "CargoButton") != 0) continue;
                const std::string digits = f.name.substr(11);
                if (digits.empty() || digits.size() > 2) continue;
                int idx = 0;
                bool numeric = true;
                for (char ch : digits) {
                    if (ch < '0' || ch > '9') { numeric = false; break; }
                    idx = idx * 10 + (ch - '0');
                }
                if (!numeric || idx >= 8 || f.rect.w <= 0 || f.rect.h <= 0) continue;
                if (!r->cargo_slots[idx].ok()) ++found;
                r->cargo_slots[idx] = {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
            }
            r->cargo_slots_ok = found > 0;
        }
        r->command = find_rect(solved, "CommandPanel", cw, ch);
        r->info = find_rect(solved, "InfoPanel", cw, ch);
        r->wireframe = find_rect(solved, "UnitWireframe", cw, ch);
        for (const ui::SolvedFrame& f : solved) {
            if (f.parent != "ProgressContainer") continue;
            if (f.name == "ProgressBar") r->progress_bar = find_rect({f}, "ProgressBar", cw, ch);
            else if (f.name == "ProgressButton")
                r->progress_button = find_rect({f}, "ProgressButton", cw, ch);
        }
        r->progress_ok = r->progress_bar.ok() && r->progress_button.ok();
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
                if (f.parent != "CommandPanel") continue;
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
        {
            int found = 0;
            for (const ui::SolvedFrame& f : solved) {
                if (f.parent != "PageFrame0") continue;
                if (f.name.compare(0, 15, "GroupUnitButton") != 0) continue;
                const std::string digits = f.name.substr(15);
                if (digits.empty() || digits.size() > 2) continue;
                int idx = 0;
                bool numeric = true;
                for (char ch : digits) {
                    if (ch < '0' || ch > '9') { numeric = false; break; }
                    idx = idx * 10 + (ch - '0');
                }
                if (!numeric || idx >= 24 || f.rect.w <= 0 || f.rect.h <= 0) continue;
                if (!r->group_slots[idx].ok()) ++found;
                r->group_slots[idx] = {f.rect.x, f.rect.y, f.rect.x + f.rect.w,
                                       f.rect.y + f.rect.h};
            }
            r->group_slots_ok = found == 24;
        }
        {
            std::string pane_parent;
            for (const ui::SolvedFrame& f : solved)
                if (f.name == "PageFrame0") { pane_parent = f.parent; break; }
            int found = 0;
            for (const ui::SolvedFrame& f : solved) {
                if (f.parent != pane_parent) continue;
                if (f.name.size() != 11 || f.name.compare(0, 10, "PageButton") != 0) continue;
                const char d = f.name[10];
                if (d < '0' || d > '5' || f.rect.w <= 0 || f.rect.h <= 0) continue;
                const int idx = d - '0';
                if (!r->page_buttons[idx].ok()) ++found;
                r->page_buttons[idx] = {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
            }
            r->page_buttons_ok = found == 6;
        }
        {
            int found = 0;
            for (const ui::SolvedFrame& f : solved) {
                if (f.parent != "ResourceFrame") continue;
                if (f.rect.w <= 0 || f.rect.h <= 0) continue;
                HudAuthoredRects::R* dst = nullptr;
                if (f.name == "ResourceIcon0") dst = &r->resource_icon[0];
                else if (f.name == "ResourceIcon1") dst = &r->resource_icon[1];
                else if (f.name == "SupplyIcon") dst = &r->supply_icon;
                else if (f.name == "ResourceLabel0") dst = &r->resource_label[0];
                else if (f.name == "ResourceLabel1") dst = &r->resource_label[1];
                else if (f.name == "SupplyLabel") dst = &r->supply_label;
                if (!dst) continue;
                if (!dst->ok()) ++found;
                *dst = {f.rect.x, f.rect.y, f.rect.x + f.rect.w, f.rect.y + f.rect.h};
            }
            r->resource_icons_ok = found == 6;
        }
        r->ok = r->minimap.ok() && r->command.ok() && r->info.ok();
        slot = std::move(r);
    } catch (const std::exception&) {
        return nullptr;
    }
    return slot && slot->ok ? slot.get() : nullptr;
}

}
