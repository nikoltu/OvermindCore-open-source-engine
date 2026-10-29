#include "omc/app/shell_screens.hpp"

#include <cstdio>

#include "omc/app/shell_emblem.hpp"

namespace omc::app::shell {

namespace v2 = theme::v2;

namespace {
struct Live {
    int seg = 1, step = 4, drop = 0, tab = 0, sel = 1;
    float slide = 0.6f;
    bool tog = true;
    std::string field = "Player";
};
Live& live() {
    static Live l;
    return l;
}
}

bool screen_widgets(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "WIDGETS", kChromeBack | kChromePlate, "ESC Back");
    Live& l = live();
    static const char* const kStates[] = {"NORMAL", "HOVER", "PRESSED", "FOCUSED", "DISABLED"};
    const float x0 = 304.0f, col = 272.0f, w = 256.0f;
    for (int s = 0; s < 5; ++s) ui.text(ui.at(x0 + col * float(s), 112, w, 24), kStates[s], v2::kLabel, v2::kTextMuted);
    auto state = [](int s) {
        Opts o;
        o.force = s == 1 ? 1 : (s == 2 ? 3 : (s == 3 ? 4 : 0));
        o.enabled = s != 4;
        return o;
    };
    struct RowDef {
        const char* name;
        ButtonKind kind;
        float h;
    };
    const RowDef rows[] = {{"PRIMARY", ButtonKind::Primary, 80.0f}, {"BUTTON", ButtonKind::Normal, 72.0f}, {"GHOST", ButtonKind::Ghost, 48.0f}, {"MENU ROW", ButtonKind::Menu, 72.0f}};
    float y = 144.0f;
    for (const RowDef& r : rows) {
        ui.text(ui.at(64, y, 224, r.h), r.name, v2::kLabel, v2::kTextMuted);
        for (int s = 0; s < 5; ++s) {
            Opts o = state(s);
            if (r.kind == ButtonKind::Menu) o.hotkey = 'K';
            ui.button(std::string("g/") + r.name + "/" + kStates[s], ui.at(x0 + col * float(s), y, w, r.h), r.kind == ButtonKind::Menu ? "Row" : "Start", r.kind, o);
        }
        y += r.h + 16.0f;
    }
    ui.text(ui.at(64, y, 224, 64), "SEGMENTED", v2::kLabel, v2::kTextMuted);
    {
        int v = 1;
        ui.segmented("g/seg/rest", ui.at(x0, y, 528, 64), {"Slow", "Normal", "Fast"}, v);
        Opts off;
        off.enabled = false;
        ui.segmented("g/seg/off", ui.at(x0 + 544, y, 528, 64), {"Slow", "Normal", "Fast"}, v, off);
    }
    y += 80.0f;
    ui.text(ui.at(64, y, 224, 64), "STEPPER / TOGGLE", v2::kLabel, v2::kTextMuted);
    {
        int v = 4;
        ui.stepper("g/step/rest", ui.at(x0, y, 528, 64), v, 1, 7, ShellSettings::difficulty_name(v));
        bool on = true, off_v = false;
        ui.toggle("g/tog/on", ui.at(x0 + 544, y + 8, 96, 48), on);
        ui.toggle("g/tog/off", ui.at(x0 + 656, y + 8, 96, 48), off_v);
        Opts off;
        off.enabled = false;
        ui.toggle("g/tog/dis", ui.at(x0 + 768, y + 8, 96, 48), on, off);
    }
    y += 80.0f;
    ui.text(ui.at(64, y, 224, 64), "SLIDER / DROPDOWN", v2::kLabel, v2::kTextMuted);
    {
        float v = 0.4f;
        ui.slider("g/slide/rest", ui.at(x0, y, 528, 64), v, "40 %");
        int d = 0;
        Opts off;
        off.enabled = false;
        ui.dropdown("g/drop/off", ui.at(x0 + 544, y, 528, 64), {"Normal", "Always"}, d, off);
    }
    y += 80.0f;
    ui.text(ui.at(64, y, 224, 72), "FIELD / BANNER", v2::kLabel, v2::kTextMuted);
    {
        std::string empty;
        ui.text_field("g/field/empty", ui.at(x0, y, 528, 72), empty, 24, "Placeholder");
        ui.banner(ui.at(x0 + 544, y, 800, 72), State::Err, "Refused", "The map could not be read.");
    }
    y += 88.0f;
    ui.text(ui.at(64, y, 224, 64), "TABS / PROGRESS", v2::kLabel, v2::kTextMuted);
    {
        int t = 1;
        ui.tabs("g/tabs/rest", ui.at(x0, y, 760, 64), {"Gameplay", "Video", "Audio"}, t);
        ui.progress(ui.at(x0 + 800, y + 8, 400, 16), 0.37f);
        ui.progress(ui.at(x0 + 800, y + 40, 400, 16), -1.0f);
    }

    const float rx = 1700.0f;
    ui.panel(ui.at(rx - 24, 128, 393, 984), "Live");
    ui.portrait(ui.at(rx, 184, 104, 158), &shell_emblem(false));
    ui.image(ui.at(rx + 128, 184, 200, 158), nullptr, draw::Fit::Contain, "No preview");
    ui.segmented("live/seg", ui.at(rx, 372, 345, 64), {"A", "B", "C"}, l.seg);
    ui.stepper("live/step", ui.at(rx, 452, 345, 64), l.step, 1, 7, ShellSettings::difficulty_name(l.step));
    ui.toggle("live/tog", ui.at(rx, 532, 96, 48), l.tog);
    char pct[16];
    std::snprintf(pct, sizeof pct, "%d %%", int(l.slide * 100.0f + 0.5f));
    ui.slider("live/slide", ui.at(rx, 596, 345, 64), l.slide, pct);
    ui.text_field("live/field", ui.at(rx, 676, 345, 72), l.field, 16, "Name");
    const Rect vp = ui.at(rx, 844, 329, 248);
    const Ui::Range rg = ui.list("live/list", vp, 12, ui.px(68), ui.px(64), 1, l.sel);
    for (int i = rg.first; i < rg.first + rg.count; ++i) {
        Opts o;
        o.tag = i % 2 ? "2P" : "4P";
        if (ui.item("live/list/" + std::to_string(i), ui.list_item_rect(i), i == l.sel, "List row " + std::to_string(i + 1), o)) l.sel = i;
    }
    ui.list_end();
    ui.scrollbar("live/list", ui.at(rx + 337, 844, 8, 248));
    ui.dropdown("live/drop", ui.at(rx, 764, 345, 64), {"Normal", "Targeting", "Selected", "Damaged", "Always"}, l.drop);

    if (ui.button("g/confirm", ui.at(64, 1048, 320, 64), "Open the dialog", ButtonKind::Normal)) ui.open_modal("g/modal");
    confirm(ui, "g/modal", "Delete this replay?", "The file is removed from the replays folder.\nThis cannot be undone.", "Keep", "Delete", true);
    toasts(ui, ctx);
    return true;
}

}
