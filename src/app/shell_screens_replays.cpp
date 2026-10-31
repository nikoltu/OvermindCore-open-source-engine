#include "omc/app/shell_screens.hpp"

#include <algorithm>
#include <cstdio>
#include <filesystem>

namespace omc::app::shell {

namespace v2 = theme::v2;
namespace L = layout::s80_replays;

namespace {

constexpr layout::R kList{64, 208, 1349, 904};
constexpr float kColDate = 88.0f, kColMap = 400.0f, kColMatchup = 900.0f, kColLength = 1200.0f, kListRight = 1413.0f;
constexpr layout::R kBanner{576, 112, 837, 64};

std::string clock_text(long long ticks, int tps) {
    const long long s = tps > 0 ? ticks / tps : 0;
    char buf[32];
    std::snprintf(buf, sizeof buf, "%lld:%02lld", s / 60, s % 60);
    return buf;
}

}

bool screen_replays(Ui& ui, ShellCtx& ctx) {
    chrome(ui, ctx, "REPLAYS AND SAVES", kChromeBack | kChromePlate, "ENTER Watch   ESC Back");
    UiState& st = ui.state();
    int& tab = st.num["replays/tab"];
    {
        int t = tab;
        Opts o;
        o.hint = "Recorded matches, or games saved from the game menu.";
        if (ui.segmented("replays/tab", ui.at(L::kReplays.x, L::kReplays.y, L::kReplays.w + L::kSavedGames.w + 8.0f, L::kReplays.h), {"Replays", "Saved games"}, t, o) &&
            t != tab) {
            tab = t;
            ctx.select_recording(-1);
            st.num["replays/save_sel"] = -1;
            st.scroll_px["replays/list"] = 0;
        }
    }
    const bool saves = tab == 1;
    const std::string& dir = saves ? ctx.saves_dir() : ctx.replays_dir();
    const std::vector<ShellCtx::RecordingRow> rows = ctx.recordings(saves);
    if (dir.empty()) ui.banner(ui.at(kBanner), State::Warn, "No folder", saves ? "No saves folder is configured." : "No replays folder is configured.");

    int sel = ctx.selected_recording();
    if (saves) sel = st.num.count("replays/save_sel") ? st.num["replays/save_sel"] : -1;
    if (sel >= int(rows.size())) sel = -1;
    const Rect vp = ui.at(kList);
    if (rows.empty()) {
        ui.fill(vp, v2::kSurface1);
        ui.text(Rect{vp.x0, vp.y0 + ui.px(240.0f), vp.x1, vp.y0 + ui.px(284.0f)}, saves ? "No saved games" : "No replays yet", v2::kH3, v2::kTextHi, Align::Centre);
        ui.text(Rect{vp.x0, vp.y0 + ui.px(296.0f), vp.x1, vp.y0 + ui.px(328.0f)},
                saves ? "Save from the game menu (Esc) during a match." : "Every finished match is recorded here.", v2::kBody, v2::kTextBody, Align::Centre);
        if (!dir.empty()) ui.text(Rect{vp.x0, vp.y0 + ui.px(336.0f), vp.x1, vp.y0 + ui.px(364.0f)}, dir, v2::kSmall, v2::kTextMuted, Align::Centre);
    } else {
        const int pitch = ui.px(76.0f), row_h = ui.px(72.0f);
        int moved = sel;
        const Ui::Range rg = ui.list("replays/list", vp, int(rows.size()), pitch, row_h, 1, moved);
        if (moved != sel && moved >= 0 && moved < int(rows.size())) sel = moved;
        bool open_now = false;
        for (int i = rg.first; i < rg.first + rg.count; ++i) {
            const ShellCtx::RecordingRow& r = rows[std::size_t(i)];
            const Rect rr = ui.list_item_rect(i);
            Opts o;
            o.hint = r.path;
            if (!r.readable) o.tag = "UNREADABLE";
            else if (r.map_missing) o.tag = "MAP MISSING";
            if (ui.item(r.name, rr, i == sel, "", o)) {
                sel = i;
                open_now = ui.double_clicked();
            }
            const theme::Rgb c = i == sel ? v2::kTextHi : v2::kTextBody;
            const auto col = [&](float x0, float x1, const std::string& s) {
                ui.text(Rect{ui.at(x0, 0, 0, 0).x0, rr.y0, ui.at(x1, 0, 0, 0).x0 - ui.px(16.0f), rr.y1}, s, v2::kBody, c);
            };
            col(kColDate, kColMap, r.date);
            col(kColMap, kColMatchup, r.readable ? r.map : std::string("-"));
            col(kColMatchup, kColLength, r.matchup);
            col(kColLength, kListRight - 180.0f, r.readable ? clock_text(r.ticks, ctx.ticks_per_second()) : std::string());
        }
        ui.list_end();
        ui.scrollbar("replays/list", ui.at(L::kScrollbar));
        if (saves) st.num["replays/save_sel"] = sel;
        else ctx.select_recording(sel);
        if (open_now && sel >= 0 && rows[std::size_t(sel)].readable && !rows[std::size_t(sel)].map_missing) {
            const std::string path = rows[std::size_t(sel)].path;
            if (saves ? !ctx.resume_save(path, "ResumeButton") : !ctx.watch_replay(path, "Watch replay")) ctx.fx.toast("That recording could not be read.", true);
            return true;
        }
    }

    const bool have = sel >= 0 && sel < int(rows.size());
    const ShellCtx::RecordingRow* cur = have ? &rows[std::size_t(sel)] : nullptr;
    const int map_index = cur ? ctx.map_by_title(cur->map) : -1;
    ui.image(ui.at(L::kMapPreview), map_index >= 0 ? ctx.map_preview(map_index) : nullptr, draw::Fit::Contain,
             !cur ? (saves ? "Select a saved game" : "Select a replay") : (cur->map_missing ? "Map not in the library" : "No preview in this map"), 2.0f);
    {
        const Rect m = ui.at(L::kMeta);
        const int lh = ui.px(36.0f);
        if (cur) {
            ui.text(Rect{m.x0, m.y0, m.x1, m.y0 + lh + ui.px(4.0f)}, cur->readable ? cur->map : cur->name, v2::kH3, v2::kTextHi);
            ui.text(Rect{m.x0, m.y0 + lh + ui.px(8.0f), m.x1, m.y0 + lh * 2 + ui.px(8.0f)}, cur->readable ? cur->matchup : std::string("This file does not read as a recording."),
                    v2::kBody, cur->readable ? v2::kTextBody : v2::kErr);
            ui.text(Rect{m.x0, m.y0 + lh * 2 + ui.px(12.0f), m.x1, m.y0 + lh * 3 + ui.px(12.0f)},
                    cur->date + (cur->readable ? "  -  " + clock_text(cur->ticks, ctx.ticks_per_second()) : std::string()), v2::kSmall, v2::kTextMuted);
            if (cur->map_missing && cur->readable)
                ui.text(Rect{m.x0, m.y0 + lh * 3 + ui.px(16.0f), m.x1, m.y0 + lh * 4 + ui.px(16.0f)}, "Its map is not in the map library.", v2::kSmall, v2::kWarn);
        }
    }
    {
        Opts d;
        d.enabled = have;
        d.danger = true;
        d.hint = "Delete this file (asks first).";
        d.why_disabled = "Select a row first.";
        if (ui.button("replays/delete", ui.at(L::kDelete), "Delete", ButtonKind::Ghost, d)) ui.open_modal("replays/delete");
        Opts f;
        std::error_code ec;
        f.enabled = !dir.empty() && ctx.has_folder_opener() && std::filesystem::is_directory(dir, ec);
        f.hint = "Show the folder in the file browser.";
        f.why_disabled = dir.empty() ? "No folder is configured." : "The folder does not exist yet.";
        if (ui.button("replays/open_folder", ui.at(L::kOpenFolder), "Open folder", ButtonKind::Ghost, f) && !ctx.open_folder(dir))
            ctx.fx.toast("The folder could not be opened.", true);
    }
    {
        const bool can = cur && cur->readable && !cur->map_missing;
        Opts o;
        o.trigger = saves ? "ResumeButton" : "Watch replay";
        o.enabled = can;
        o.hint = saves ? "Load the saved game and play on." : "Watch the recorded match.";
        o.why_disabled = !cur ? "Select a row first." : (!cur->readable ? "This file does not read as a recording." : "Its map is not in the map library.");
        if (ui.button("replays/primary", ui.at(L::kWatchResume), saves ? "Resume" : "Watch", ButtonKind::Primary, o) && can &&
            (saves ? !ctx.resume_save(cur->path, "ResumeButton") : !ctx.watch_replay(cur->path, "Watch replay")))
            ctx.fx.toast("That recording could not be read.", true);
    }
    if (cur && confirm(ui, "replays/delete", saves ? "Delete this saved game?" : "Delete this replay?", cur->name + "\nThe file is removed from the disk.", "Keep", "Delete",
                       true) == Confirm::Yes) {
        if (!ctx.delete_recording(cur->path)) ctx.fx.toast("The file could not be deleted.", true);
        if (saves) st.num["replays/save_sel"] = -1;
        else ctx.select_recording(-1);
    }
    toasts(ui, ctx);
    return true;
}

}
