#pragma once
#include <string>
#include <vector>

#include "omc/app/platform.hpp"
#include "omc/app/screen_router.hpp"
#include "omc/app/shell_ui.hpp"
#include "omc/app/shell_widgets.hpp"
#include "omc/app/ui_icons.hpp"

namespace omc::app {
class FrontEnd;
class PortraitCatalog;
struct PortraitRef;
struct ScoreRow;
struct MatchPlayerCard {
    Race race = Race::Terran;
    int team = -1;
    MeleeSlot::Controller controller = MeleeSlot::Controller::AI;
    int difficulty = 0;
    bool won = false;
};
}

namespace omc::app::shell {

struct ShellCtx {
    FrontEnd& fe;
    ShellSettings& settings;
    ShellFx& fx;
    ScreenRouter& router;

    const std::vector<platform::MapEntry>& maps() const;
    int selected_map() const;
    void select_map(int index);
    const IconAtlas::Icon* map_preview(int index);
    const IconAtlas::Icon* map_thumb(int index);
    void warm_map_thumbs(int budget);
    void pick_maps_folder();
    bool team_color(int slot, theme::Rgb& out);
    int first_real_map() const;
    int setup_map() const;
    std::string map_title(int index) const;
    std::string stored_map_label() const;
    int map_by_title(const std::string& title) const;
    bool start_match(const char* via);
    bool start_selected_match(const char* via);
    const std::string& match_error() const;
    bool watch_replay(const std::string& path, const char* via);
    int ticks_per_second() const;
    struct LoadView {
        std::string map, stage;
        int map_index = 0;
        Race race[2] = {Race::Terran, Race::Terran};
        std::vector<MeleeSlot> players;
        bool replay = false;
        std::size_t done = 0, total = 0;
        float elapsed_ms = 0.0f;
    };
    LoadView loading() const;
    struct ResultView {
        bool decided = false, victory = false;
        std::string map, replay;
        long long ticks = 0;
        Race race[2] = {Race::Terran, Race::Terran};
        int tier = 0;
        const std::vector<ScoreRow>* score = nullptr;
        const std::vector<MatchPlayerCard>* players = nullptr;
    };
    ResultView result() const;
    int lobby_format() const;
    bool set_lobby_format(int format);
    int lobby_rows() const;
    void lobby_row(int i, int& controller, int& race, int& difficulty, int& team) const;
    void set_lobby_row(int i, int controller, int race, int difficulty, int team);
    int map_start_count(int index);
    std::string player_name() const;
    Race player_default_race() const;
    const IconAtlas::Icon* player_portrait();
    const IconAtlas::Icon* portrait(const PortraitRef& ref);
    const IconAtlas::Icon* portrait_texture(const std::string& name);
    const IconAtlas::Icon* ai_avatar();
    const IconAtlas::Icon* race_mark(LobbyRace race);
    PortraitCatalog& portraits();
    bool scan_portraits(int budget);
    void quit();
    void apply_settings();
    bool in_match() const;
    const char* version() const;
    bool has_maps_folder_picker() const;
    bool has_arena() const;
    struct GameData {
        std::string version;
        bool valid = false, verified = false;
    };
    GameData game_data();
};

inline std::vector<std::vector<int>> team_columns(const std::vector<int>& teams) {
    std::vector<int> keys;
    std::vector<std::vector<int>> cols;
    for (std::size_t p = 0; p < teams.size(); ++p) {
        const int key = teams[p] >= 0 ? teams[p] : -(int(p) + 2);
        std::size_t c = 0;
        while (c < keys.size() && keys[c] != key) ++c;
        if (c == keys.size()) {
            keys.push_back(key);
            cols.emplace_back();
        }
        cols[c].push_back(int(p));
    }
    return cols;
}
inline std::string format_label(const std::vector<std::vector<int>>& cols) {
    bool singles = true;
    for (const auto& c : cols) singles = singles && c.size() == 1;
    if (singles && cols.size() > 2) return "FFA";
    std::string s;
    for (std::size_t i = 0; i < cols.size(); ++i) s += (i ? "V" : "") + std::to_string(cols[i].size());
    return s;
}

using ScreenFn = bool (*)(Ui&, ShellCtx&);
ScreenFn screen_fn(ScreenId id);

enum ChromeFlag : unsigned {
    kChromeBack = 1u,
    kChromePlate = 2u,
    kChromeHome = 4u,
    kChromeBare = 8u,
};
bool chrome(Ui& ui, ShellCtx& ctx, const std::string& title, unsigned flags, const std::string& legend = {});
void toasts(Ui& ui, ShellCtx& ctx);

enum class Confirm { None, Yes, No };
Confirm confirm(Ui& ui, const std::string& id, const std::string& title, const std::string& body, const std::string& no_label,
                const std::string& yes_label, bool danger = false);

bool screen_home(Ui&, ShellCtx&);
bool screen_setup(Ui&, ShellCtx&);
bool screen_loading(Ui&, ShellCtx&);
bool screen_result(Ui&, ShellCtx&);
bool screen_score(Ui&, ShellCtx&);
bool screen_settings(Ui&, ShellCtx&);
bool screen_profile(Ui&, ShellCtx&);
bool screen_portraits(Ui&, ShellCtx&);
bool screen_replays(Ui&, ShellCtx&);
bool screen_about(Ui&, ShellCtx&);
bool screen_widgets(Ui&, ShellCtx&);

}
