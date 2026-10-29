#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <vector>

#include "omc/app/game_clock.hpp"
#include "omc/app/melee_game.hpp"
#include "omc/app/race.hpp"
#include "omc/render/input.hpp"
#include "omc/render/scene.hpp"
#include "omc/sim/world.hpp"
#include "omc/render/quality.hpp"
#include "omc/render/unit_overlay.hpp"
#include "omc/ui/layout.hpp"

namespace omc::app {

class ShellSettings {
public:
    using Get = std::function<std::string(const std::string&)>;
    using Set = std::function<bool(const std::string&, const std::string&)>;
    void set_io(Get get, Set set) { get_ = std::move(get); set_ = std::move(set); }

    std::string get(const std::string& key) const;
    bool set(const std::string& key, const std::string& value);
    bool get_bool(const std::string& key, bool dflt) const;
    float get_float(const std::string& key, float dflt) const;
    int get_int(const std::string& key, int dflt) const;

    struct Bus {
        const char* key;
        const char* label;
        std::vector<const char*> categories;
    };
    static const std::vector<Bus>& buses();
    float bus_gain(const Bus& b) const { return get_float(b.key, 1.0f); }
    bool set_bus_gain(const Bus& b, float g);

    static constexpr char kUnitStatusKey[] = "gameplay.unit_status";
    static const std::vector<const char*>& unit_status_names();
    render::UnitOverlay::StatusBarOptions::Mode unit_status_mode() const;
    void cycle_unit_status(int dir);
    static constexpr char kGameSpeedKey[] = "gameplay.game_speed";
    GameClock::Speed game_speed() const;
    void cycle_game_speed(int dir);
    static const char* speed_name(GameClock::Speed s);
    static constexpr char kAiDifficultyKey[] = "gameplay.ai_difficulty";
    int ai_difficulty() const;
    void cycle_ai_difficulty(int dir);
    static const char* difficulty_name(int tier);

    static constexpr char kHotkeyProfileKey[] = "hotkey.profile";
    struct Profile { const char* id; const char* name; };
    static const std::vector<Profile>& hotkey_profiles();
    int hotkey_profile() const;
    void cycle_hotkey_profile(int dir);

    int display_mode() const;
    void cycle_display_mode(int dir);
    static const char* display_mode_name(int m);
    bool vsync() const;
    void toggle_vsync();
    float ui_scale() const;
    bool set_ui_scale(float v);

    static constexpr char kProfileNameKey[] = "profile.name";
    static constexpr char kProfileRaceKey[] = "profile.race";
    static constexpr char kProfileMatchesKey[] = "profile.matches";
    static constexpr char kProfileWinsKey[] = "profile.wins";
    std::string profile_name() const;
    bool set_profile_name(const std::string& n);
    LobbyRace profile_race() const;
    void cycle_profile_race(int dir);
    void record_match(bool won);
    bool set_profile_race(LobbyRace r);

    static constexpr char kLobbyMapKey[] = "lobby.map";
    static constexpr char kLobbyOpponentRaceKey[] = "lobby.opponent_race";
    static constexpr char kLastOutcomeKey[] = "last.outcome";
    static constexpr char kLastMapKey[] = "last.map";
    static constexpr char kLastRacesKey[] = "last.races";
    static constexpr char kLastTicksKey[] = "last.ticks";
    static constexpr char kLastReplayKey[] = "last.replay";
    static constexpr char kLastTimeKey[] = "last.time";
    static constexpr char kLastTierKey[] = "last.tier";
    static constexpr char kProfilePortraitKey[] = "profile.portrait";
    static constexpr char kHotkeyBadgesKey[] = "ui.hotkey_badges";
    static constexpr char kBgMotionKey[] = "display.bg_motion";
    std::string lobby_map() const { return get(kLobbyMapKey); }
    bool set_lobby_map(const std::string& path) { return set(kLobbyMapKey, path); }
    LobbyRace opponent_race() const;
    bool set_opponent_race(LobbyRace r);
    std::string profile_portrait() const { return get(kProfilePortraitKey); }
    bool set_profile_portrait(const std::string& ref) { return set(kProfilePortraitKey, ref); }
    bool hotkey_badges() const { return get_bool(kHotkeyBadgesKey, true); }
    bool bg_motion() const { return get_bool(kBgMotionKey, true); }

    static constexpr char kPortraitTextureKey[] = "perf.portrait_texture";
    static constexpr char kConsoleTextureKey[] = "perf.console_texture";
    static constexpr char kPortraitRateKey[] = "perf.portrait_rate";
    static constexpr char kShadowQualityKey[] = "perf.shadows";
    static constexpr char kRenderScaleKey[] = "perf.render_scale";
    static constexpr char kShadowMapKey[] = "perf.shadow_map";
    static constexpr char kTerrainQualityKey[] = "perf.terrain";
    static constexpr char kDoodadDensityKey[] = "perf.doodads";
    struct Preset {
        const char* name;
        int shadows, shadow_map, terrain, doodads, render_scale;
    };
    static constexpr int kPresetCount = 4;
    static constexpr Preset kPresets[kPresetCount] = {
        {"Ultra", 0, 0, 0, 0, 0}, {"High", 0, 1, 1, 0, 0}, {"Medium", 1, 1, 1, 1, 0}, {"Low", 1, 2, 2, 2, 1}};
    static constexpr int kDefaultPreset = 1;
    struct Level {
        const char* token;
        const char* name;
        int value;
    };
    static const std::vector<Level>& portrait_texture_levels();
    static const std::vector<Level>& console_texture_levels();
    static const std::vector<Level>& portrait_rate_levels();
    static const std::vector<Level>& shadow_quality_levels();
    static const std::vector<Level>& render_scale_levels();
    static constexpr int kPortraitTextureDefault = 1;
    static constexpr int kConsoleTextureDefault = 0;
    static constexpr int kPortraitRateDefault = 1;
    static const std::vector<Level>& shadow_map_levels();
    static const std::vector<Level>& terrain_quality_levels();
    static const std::vector<Level>& doodad_density_levels();
    static const std::vector<std::string>& preset_names();
    static constexpr int kShadowQualityDefault = kPresets[kDefaultPreset].shadows;
    static constexpr int kRenderScaleDefault = kPresets[kDefaultPreset].render_scale;
    static constexpr int kShadowMapDefault = kPresets[kDefaultPreset].shadow_map;
    static constexpr int kTerrainQualityDefault = kPresets[kDefaultPreset].terrain;
    static constexpr int kDoodadDensityDefault = kPresets[kDefaultPreset].doodads;
    bool has(const std::string& key) const { return !get(key).empty(); }
    int level(const char* key, const std::vector<Level>& levels, int dflt) const;
    bool set_level(const char* key, const std::vector<Level>& levels, int index);
    int portrait_texture_level() const { return level(kPortraitTextureKey, portrait_texture_levels(), kPortraitTextureDefault); }
    int console_texture_level() const { return level(kConsoleTextureKey, console_texture_levels(), kConsoleTextureDefault); }
    int portrait_rate_level() const { return level(kPortraitRateKey, portrait_rate_levels(), kPortraitRateDefault); }
    int shadow_quality_level() const { return level(kShadowQualityKey, shadow_quality_levels(), kShadowQualityDefault); }
    int render_scale_level() const { return level(kRenderScaleKey, render_scale_levels(), kRenderScaleDefault); }
    int shadow_map_level() const { return level(kShadowMapKey, shadow_map_levels(), kShadowMapDefault); }
    int terrain_quality_level() const { return level(kTerrainQualityKey, terrain_quality_levels(), kTerrainQualityDefault); }
    int doodad_density_level() const { return level(kDoodadDensityKey, doodad_density_levels(), kDoodadDensityDefault); }
    int quality_preset() const;
    bool set_quality_preset(int preset);
    render::RenderQuality scene_quality() const;
    static render::RenderQuality preset_quality(int preset);
    static bool apply_quality_spec(const std::string& spec, render::RenderQuality& q);
    struct LastMatch {
        std::string outcome, map, replay, time;
        int race[2] = {0, 0};
        long long ticks = 0;
        int tier = 0;
        bool valid() const { return outcome == "victory" || outcome == "defeat"; }
    };
    LastMatch last_match() const;
    void set_last_match(const LastMatch& m);

private:
    Get get_;
    Set set_;
    std::map<std::string, std::string> mem_;
};

struct ShellFx {
    static constexpr float kHoverInMs = 110.0f;
    static constexpr float kHoverOutMs = 220.0f;
    static constexpr float kFadeMs = 180.0f;
    static constexpr float kToastMs = 4200.0f;
    static constexpr float kTipDelayMs = 450.0f;
    static constexpr float kPulseMs = 1400.0f;
    static constexpr int kMaxToasts = 3;

    struct Toast {
        std::string text;
        float ttl_ms = kToastMs;
        bool error = false;
    };

    float hover(const std::string& key) const;
    void tick(float dt_ms, const std::string& hovered);
    void on_screen_change() { fade_ms = 0.0f; focus = -1; tip_ms = 0.0f; }
    float fade() const { return fade_ms >= kFadeMs ? 1.0f : fade_ms / kFadeMs; }
    float pulse() const;
    bool tip_due() const { return tip_ms >= kTipDelayMs; }
    void toast(std::string text, bool error);
    const std::vector<Toast>& toasts() const { return toasts_; }

    int focus = -1;
    void focus_tab(int count, int dir);
    void focus_arrow(const std::vector<ui::Rect>& rects, int dx, int dy);

    float clock_ms = 0.0f;
    float fade_ms = kFadeMs;
    float tip_ms = 0.0f;
    std::string hovered;
    std::string tip_key;

private:
    std::map<std::string, float> hover_;
    std::vector<Toast> toasts_;
};

struct ShellKeys {
    bool tab = false, shift_tab = false, enter = false, space = false, backspace = false;
    bool left = false, right = false, up = false, down = false;
    bool page_up = false, page_down = false, home = false, end = false;
    char letter = 0;
    bool del = false, f5 = false, ctrl_v = false, escape = false, shift = false;
    std::string text;
    bool any() const;
    static ShellKeys read(const render::InputState& in);
};

const char* shell_tooltip(const std::string& key);

}
