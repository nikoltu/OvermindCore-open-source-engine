#include "sc2/app/shell_ui.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "sc2/ai/difficulty.hpp"
#include "sc2/app/ui_settings.hpp"

namespace sc2::app {

std::string ShellSettings::get(const std::string& key) const {
    if (get_) return get_(key);
    auto it = mem_.find(key);
    return it == mem_.end() ? std::string() : it->second;
}

bool ShellSettings::set(const std::string& key, const std::string& value) {
    if (set_) return set_(key, value);
    mem_[key] = value;
    return true;
}

bool ShellSettings::get_bool(const std::string& key, bool dflt) const { return parse_bool(get(key), dflt); }

float ShellSettings::get_float(const std::string& key, float dflt) const {
    const std::string raw = get(key);
    if (raw.empty()) return dflt;
    char* end = nullptr;
    const float v = std::strtof(raw.c_str(), &end);
    if (end == raw.c_str() || !(v == v)) return dflt;
    return v;
}

int ShellSettings::get_int(const std::string& key, int dflt) const { return parse_int(get(key), dflt); }

const std::vector<ShellSettings::Bus>& ShellSettings::buses() {
    static const std::vector<Bus> k = {
        {"sound.master", "Master Volume", {}},
        {"sound.music", "Music", {"Music"}},
        {"sound.effects", "Sound Effects",
         {"Build", "Combat", "Death", "Flames", "Foley", "Gather", "Movement", "Spell", "SPieces", "SEmitters", "Other"}},
        {"sound.voice", "Voices", {"Voice", "Dialogue", "Ready"}},
        {"sound.ui", "Interface Sounds", {"UI", "Alert"}},
        {"sound.ambient", "Ambient Sounds", {"Ambient", "SAmbient", "Doodad"}},
    };
    return k;
}

bool ShellSettings::set_bus_gain(const Bus& b, float g) {
    g = std::max(0.0f, std::min(1.0f, g));
    char buf[16];
    std::snprintf(buf, sizeof buf, "%.2f", double(g));
    return set(b.key, buf);
}

const std::vector<const char*>& ShellSettings::unit_status_names() {
    static const std::vector<const char*> k = {"Normal", "Targeting", "Selected", "Damaged", "Always"};
    return k;
}

render::UnitOverlay::StatusBarOptions::Mode ShellSettings::unit_status_mode() const {
    using M = render::UnitOverlay::StatusBarOptions::Mode;
    const int i = std::max(0, std::min(4, get_int(kUnitStatusKey, 0)));
    return M(i);
}

void ShellSettings::cycle_unit_status(int dir) {
    const int n = int(unit_status_names().size());
    const int i = (std::max(0, std::min(n - 1, get_int(kUnitStatusKey, 0))) + dir + n) % n;
    set(kUnitStatusKey, std::to_string(i));
}

GameClock::Speed ShellSettings::game_speed() const {
    const int i = std::max(0, std::min(4, get_int(kGameSpeedKey, int(GameClock::Speed::Faster))));
    return GameClock::Speed(i);
}

void ShellSettings::cycle_game_speed(int dir) {
    const int i = std::max(0, std::min(4, int(game_speed()) + dir));
    set(kGameSpeedKey, std::to_string(i));
}

const char* ShellSettings::speed_name(GameClock::Speed s) {
    switch (s) {
        case GameClock::Speed::Slower: return "Slower";
        case GameClock::Speed::Slow: return "Slow";
        case GameClock::Speed::Normal: return "Normal";
        case GameClock::Speed::Fast: return "Fast";
        case GameClock::Speed::Faster: default: return "Faster";
    }
}

int ShellSettings::ai_difficulty() const {
    return std::max(1, std::min(7, get_int(kAiDifficultyKey, int(ai::Difficulty::Medium))));
}

void ShellSettings::cycle_ai_difficulty(int dir) {
    const int i = ((ai_difficulty() - 1 + dir + 7) % 7) + 1;
    set(kAiDifficultyKey, std::to_string(i));
}

const char* ShellSettings::difficulty_name(int tier) {
    if (const ai::DifficultyRung* r = ai::difficulty_rung(ai::Difficulty(std::max(1, std::min(7, tier))))) return r->label;
    return "Medium";
}

const std::vector<ShellSettings::Profile>& ShellSettings::hotkey_profiles() {
    static const std::vector<Profile> k = {
        {"0_Default", "Standard"},          {"1_NameRightSide", "Standard for Lefties"},
        {"2_GridLeftSide", "Grid"},         {"3_GridRightSide", "Grid for Lefties"},
        {"4_Classic", "Classic"},           {"5_enUSDefault", "US Standard"},
        {"6_enUSDefaultLeft", "US Standard for Lefties"},
    };
    return k;
}

int ShellSettings::hotkey_profile() const {
    const std::string v = get(kHotkeyProfileKey);
    const auto& p = hotkey_profiles();
    for (std::size_t i = 0; i < p.size(); ++i)
        if (v == p[i].id || v == p[i].name) return int(i);
    return 0;
}

void ShellSettings::cycle_hotkey_profile(int dir) {
    const int n = int(hotkey_profiles().size());
    const int i = (hotkey_profile() + dir + n) % n;
    set(kHotkeyProfileKey, hotkey_profiles()[std::size_t(i)].id);
}

int ShellSettings::display_mode() const {
    switch (parse_display_kind(get(kDisplayModeKey))) {
        case DisplayMode::Kind::Windowed: return 0;
        case DisplayMode::Kind::ExclusiveFullscreen: return 2;
        case DisplayMode::Kind::BorderlessFullscreen: default: return 1;
    }
}

void ShellSettings::cycle_display_mode(int dir) {
    static const char* const kTokens[] = {"windowed", "borderless", "exclusive"};
    const int i = (display_mode() + dir + 3) % 3;
    set(kDisplayModeKey, kTokens[i]);
}

const char* ShellSettings::display_mode_name(int m) {
    return m == 0 ? "Windowed" : m == 2 ? "Fullscreen" : "Borderless";
}

bool ShellSettings::vsync() const { return get_bool(kDisplayVsyncKey, true); }
void ShellSettings::toggle_vsync() { set(kDisplayVsyncKey, vsync() ? "0" : "1"); }
float ShellSettings::ui_scale() const { return parse_ui_scale(get(kUiScaleKey)); }
bool ShellSettings::set_ui_scale(float v) {
    v = std::max(kUiScaleMin, std::min(kUiScaleMax, v));
    return set(kUiScaleKey, format_ui_scale(v));
}

std::string ShellSettings::profile_name() const {
    const std::string n = get(kProfileNameKey);
    return n.empty() ? std::string("PLAYER") : n;
}
bool ShellSettings::set_profile_name(const std::string& n) { return set(kProfileNameKey, n); }
LobbyRace ShellSettings::profile_race() const {
    return LobbyRace(std::max(0, std::min(3, get_int(kProfileRaceKey, int(LobbyRace::Terran)))));
}
void ShellSettings::cycle_profile_race(int dir) {
    const int i = (int(profile_race()) + dir + 4) % 4;
    set(kProfileRaceKey, std::to_string(i));
}
void ShellSettings::record_match(bool won) {
    set(kProfileMatchesKey, std::to_string(get_int(kProfileMatchesKey, 0) + 1));
    if (won) set(kProfileWinsKey, std::to_string(get_int(kProfileWinsKey, 0) + 1));
}

bool ShellSettings::set_profile_race(LobbyRace r) { return set(kProfileRaceKey, std::to_string(int(r))); }
LobbyRace ShellSettings::opponent_race() const {
    return LobbyRace(std::max(0, std::min(3, get_int(kLobbyOpponentRaceKey, int(LobbyRace::Terran)))));
}
bool ShellSettings::set_opponent_race(LobbyRace r) { return set(kLobbyOpponentRaceKey, std::to_string(int(r))); }

ShellSettings::LastMatch ShellSettings::last_match() const {
    LastMatch m;
    m.outcome = get(kLastOutcomeKey);
    m.map = get(kLastMapKey);
    m.replay = get(kLastReplayKey);
    m.time = get(kLastTimeKey);
    const std::string races = get(kLastRacesKey);
    int a = 0, b = 0;
    if (std::sscanf(races.c_str(), "%d,%d", &a, &b) == 2) {
        m.race[0] = std::max(0, std::min(2, a));
        m.race[1] = std::max(0, std::min(2, b));
    }
    m.ticks = std::atoll(get(kLastTicksKey).c_str());
    if (m.ticks < 0) m.ticks = 0;
    m.tier = std::max(0, std::min(7, get_int(kLastTierKey, 0)));
    return m;
}

void ShellSettings::set_last_match(const LastMatch& m) {
    set(kLastOutcomeKey, m.outcome);
    set(kLastMapKey, m.map);
    set(kLastRacesKey, std::to_string(m.race[0]) + "," + std::to_string(m.race[1]));
    set(kLastTicksKey, std::to_string(m.ticks));
    set(kLastReplayKey, m.replay);
    set(kLastTimeKey, m.time);
    set(kLastTierKey, std::to_string(m.tier));
}

float ShellFx::hover(const std::string& key) const {
    auto it = hover_.find(key);
    return it == hover_.end() ? 0.0f : it->second;
}

void ShellFx::tick(float dt_ms, const std::string& now_hovered) {
    if (dt_ms < 0.0f) dt_ms = 0.0f;
    if (dt_ms > 100.0f) dt_ms = 100.0f;
    clock_ms += dt_ms;
    fade_ms = std::min(kFadeMs, fade_ms + dt_ms);
    if (now_hovered != hovered) {
        hovered = now_hovered;
        tip_ms = 0.0f;
        tip_key.clear();
    } else if (!hovered.empty()) {
        tip_ms += dt_ms;
        if (tip_ms >= kTipDelayMs) tip_key = hovered;
    }
    if (!hovered.empty()) hover_[hovered] = std::min(1.0f, hover_[hovered] + dt_ms / kHoverInMs);
    for (auto it = hover_.begin(); it != hover_.end();) {
        if (it->first != hovered) it->second -= dt_ms / kHoverOutMs;
        if (it->second <= 0.0f) it = hover_.erase(it);
        else ++it;
    }
    for (Toast& t : toasts_) t.ttl_ms -= dt_ms;
    toasts_.erase(std::remove_if(toasts_.begin(), toasts_.end(), [](const Toast& t) { return t.ttl_ms <= 0.0f; }),
                  toasts_.end());
}

float ShellFx::pulse() const {
    const float t = std::fmod(clock_ms, kPulseMs) / kPulseMs;
    return 0.5f - 0.5f * std::cos(t * 6.28318530718f);
}

void ShellFx::toast(std::string text, bool error) {
    for (Toast& t : toasts_)
        if (t.text == text) { t.ttl_ms = kToastMs; t.error = error; return; }
    if (int(toasts_.size()) >= kMaxToasts) toasts_.erase(toasts_.begin());
    toasts_.push_back({std::move(text), kToastMs, error});
}

void ShellFx::focus_tab(int count, int dir) {
    if (count <= 0) { focus = -1; return; }
    if (focus < 0) focus = dir > 0 ? 0 : count - 1;
    else focus = (focus + dir + count) % count;
}

void ShellFx::focus_arrow(const std::vector<ui::Rect>& rects, int dx, int dy) {
    if (rects.empty()) { focus = -1; return; }
    if (focus < 0 || focus >= int(rects.size())) { focus = 0; return; }
    const ui::Rect& from = rects[std::size_t(focus)];
    const float fx = from.x + from.w * 0.5f, fy = from.y + from.h * 0.5f;
    int best = -1;
    float best_d = 0.0f;
    for (std::size_t i = 0; i < rects.size(); ++i) {
        if (int(i) == focus) continue;
        const ui::Rect& r = rects[i];
        const float cx = r.x + r.w * 0.5f, cy = r.y + r.h * 0.5f;
        const float ddx = cx - fx, ddy = cy - fy;
        const float along = ddx * float(dx) + ddy * float(dy);
        if (along <= 0.5f) continue;
        const float across = std::fabs(ddx * float(dy)) + std::fabs(ddy * float(dx));
        const float d = along + across * 2.0f;
        if (best < 0 || d < best_d) { best = int(i); best_d = d; }
    }
    if (best >= 0) focus = best;
}

ShellKeys ShellKeys::read(const render::InputState& in) {
    ShellKeys k;
    for (const std::string& t : in.key_tokens) {
        if (t == "Tab") { if (in.shift) k.shift_tab = true; else k.tab = true; }
        else if (t == "Space") k.space = true;
        else if (t == "Backspace") k.backspace = true;
        else if (t == "PageUp") k.page_up = true;
        else if (t == "PageDown") k.page_down = true;
        else if (t == "Home") k.home = true;
        else if (t == "End") k.end = true;
        else if (t == "Delete") k.del = true;
        else if (t == "F5") k.f5 = true;
    }
    k.escape = in.quit;
    k.shift = in.shift;
    k.ctrl_v = in.key_ctrl && (in.key_char == 'V' || in.key_char == 'v');
    for (char ch : in.text)
        if (ch >= 0x20 && ch < 0x7F) k.text.push_back(ch);
    k.left = in.key_pan_left;
    k.right = in.key_pan_right;
    k.up = in.key_pan_up;
    k.down = in.key_pan_down;
    k.letter = in.key_char;
    for (const std::string& t : in.key_tokens)
        if (t == "Enter" || t == "Return") k.enter = true;
    return k;
}

bool ShellKeys::any() const {
    return tab || shift_tab || enter || space || backspace || page_up || page_down || home || end || del || f5 || ctrl_v ||
           letter != 0 || !text.empty();
}

const char* shell_tooltip(const std::string& key) {
    struct Tip { const char* key; const char* text; };
    static const Tip kTips[] = {
        {"MultiplayerButton -> Versus AI", "Play: a 1v1 melee against the computer, a custom map, or a replay."},
        {"PlayButton", "Straight into the melee lobby."},
        {"FindMatchButton", "Versus A.I.: pick the map, the races and the difficulty, then start."},
        {"CustomButton", "Custom game: browse every map the install ships and any folder of your own."},
        {"ReplayButton", "Replays: watch a recorded match at its recorded seed."},
        {"SandboxButton", "Sandbox: one race, no opponent, spawn any unit and watch it."},
        {"ProfileButton", "Profile: your name, emblem, preferred race and match record."},
        {"menu/Options", "Options: gameplay, sound, video and hotkeys. Changes apply at once."},
        {"Credits entry", "Who made this client."},
        {"Quit game", "Leave the client (asks first)."},
        {"Confirm quit", "Close the client now."},
        {"Cancel quit", "Stay."},
        {"START GAME", "Lock the slots and load the match (Space)."},
        {"LEAVE LOBBY", "Back to the main menu (Esc)."},
        {"Maps Folder...", "Point the lobby at a folder of your own .SC2Map files; the install's maps stay."},
        {"Prev", "Previous page of maps (Page Up / wheel)."},
        {"Next", "Next page of maps (Page Down / wheel)."},
        {"Player race", "Your race. Random is drawn once, from the match seed."},
        {"AI race", "The computer's race. Random is drawn once, from the match seed."},
        {"AI difficulty", "Very Easy .. Elite: the game's own tiers (worker delay, APM, retreat)."},
        {"Game speed", "Slower .. Faster: how many sim ticks a real second buys. Ladder melee runs at Faster."},
        {"CreateButton", "Open the lobby with this map selected."},
        {"BackButton", "Back (Esc)."},
        {"HotkeysButton", "The hotkey editor."},
        {"CancelButton", "Close (Esc). Every change was applied and saved already."},
        {"rail/Engine", "The settings this client honours: sound buses, unit life bars, game speed, hotkey profile, display."},
        {"opt/sound.master", "Master Volume: every bus at once."},
        {"opt/sound.music", "Music bus."},
        {"opt/sound.effects", "Sound Effects: combat, build, death, movement, spells."},
        {"opt/sound.voice", "Voices: unit responses and dialogue."},
        {"opt/sound.ui", "Interface Sounds: buttons and alerts."},
        {"opt/sound.ambient", "Ambient Sounds: the map and its doodads."},
        {"opt/gameplay.unit_status", "Show Unit Life Bars: Normal (Alt reveals), Targeting, Selected, Damaged, Always."},
        {"opt/gameplay.game_speed", "Game Speed for the next match (also the in-game + / - keys)."},
        {"opt/hotkey.profile", "Hotkey profile: Standard, Grid, Classic and their left-handed forms."},
        {"opt/display.mode", "Display Mode: applied when the window is next created."},
        {"opt/display.vsync", "Vertical Sync: on = FIFO present, off = mailbox / immediate."},
        {"opt/display.ui_scale", "UI scale 0.75 .. 1.50: nudges the reference height every panel is solved against."},
        {"Profile name", "Type letters to rename; Backspace deletes."},
        {"Profile race", "The race the lobby offers you first."},
        {"Watch replay", "Play the selected recording."},
        {"WATCH", "Play the selected recording."},
    };
    for (const Tip& t : kTips)
        if (key == t.key) return t.text;
    return "";
}

}
