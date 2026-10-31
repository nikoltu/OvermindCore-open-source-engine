#include "omc/app/screen_router.hpp"

#include <algorithm>
#include <cstring>

namespace omc::app {

namespace {

const char* const kFlowIds[] = {
    "boot", "login", "glue_root", "home", "campaign", "campaign_difficulty", "versus_ai",
    "custom", "lobby", "skirmish_debug", "options", "load_game", "save_replay", "loading",
    "ingame", "pause_menu", "game_result", "score", "replay_browse", "replay_controls", "credits",
    "",
    "",
    "",
    "",
    "",
    "",
    "",
};
const char* const kEngineNames[] = {"hotkeys", "profile", "portraits", "data_setup", "starting", "widgets", "lan"};
static_assert(sizeof(kEngineNames) / sizeof(kEngineNames[0]) == std::size_t(ScreenId::Count) - std::size_t(ScreenId::Hotkeys),
              "every engine-only ScreenId needs a boot name");
static_assert(sizeof(kFlowIds) / sizeof(kFlowIds[0]) == std::size_t(ScreenId::Count),
              "every ScreenId needs a flow-id row (empty for engine-only)");

const char* const kNames[] = {
    "Boot", "Login", "Main Menu", "Home", "Campaign", "Difficulty", "Versus AI", "Custom Games",
    "Lobby", "Skirmish", "Options", "Load Game", "Save Replay", "Loading", "In Game",
    "Game Menu", "Game Result", "Score", "Replays", "Replay Controls", "Credits", "Hotkeys",
    "Profile", "Portraits", "Game Data", "Starting", "Widgets", "LAN Game",
};
static_assert(sizeof(kNames) / sizeof(kNames[0]) == std::size_t(ScreenId::Count),
              "every ScreenId needs a display name");

using S = ScreenId;
using K = TransitionKind;

const FlowEdge kEdges[] = {
    {S::Boot,              "app init complete",                               S::Login,          K::Replace, false, false},
    {S::Login,             "profile selected / offline auto",                 S::GlueRoot,       K::Replace, false, false},
    {S::GlueRoot,          "HomeButton",                                      S::Home,           K::Replace, false, false},
    {S::GlueRoot,          "CampaignButton",                                  S::Campaign,       K::Replace, false, false},
    {S::GlueRoot,          "MultiplayerButton -> Versus AI",                  S::VersusAI,       K::Replace, false, false},
    {S::GlueRoot,          "CustomButton",                                    S::Custom,         K::Replace, false, false},
    {S::GlueRoot,          "ReplayButton",                                    S::ReplayBrowse,   K::Replace, false, false},
    {S::GlueRoot,          "menu/Options",                                    S::Options,        K::Push,    false, false},
    {S::GlueRoot,          "Credits entry",                                   S::Credits,        K::Replace, false, false},
    {S::Campaign,          "select mission",                                  S::CampaignDifficulty, K::Replace, false, false},
    {S::Campaign,          "Load Game",                                       S::LoadGame,       K::Replace, false, false},
    {S::Campaign,          "BackButton",                                      S::GlueRoot,       K::Pop,     false, true},
    {S::CampaignDifficulty,"Play / Start Mission",                            S::Loading,        K::Replace, false, false},
    {S::VersusAI,          "queue / Find Match vs AI",                        S::Lobby,          K::Replace, false, false},
    {S::Custom,            "select melee map -> Create",                      S::Lobby,          K::Replace, false, false},
    {S::SkirmishDebug,     "PlayButton (debug-gated)",                        S::Loading,        K::Replace, false, false},
    {S::Lobby,             "slot edit (race/team/color/handicap/AI)",         S::Lobby,          K::Replace, false, false},
    {S::Lobby,             "StartButton -> countdown",                        S::Loading,        K::Replace, false, false},
    {S::Lobby,             "Back / leave lobby",                              S::GlueRoot,       K::Pop,     false, true},
    {S::LoadGame,          "Load selected save",                              S::Loading,        K::Replace, false, false},
    {S::Loading,           "load complete",                                   S::InGame,         K::Replace, false, false},
    {S::Loading,           "replay load complete",                            S::ReplayControls, K::Replace, false, false},
    {S::InGame,            "Esc / F10",                                       S::PauseMenu,      K::Push,    false, true},
    {S::PauseMenu,         "Resume",                                          S::InGame,         K::Close,   false, true},
    {S::PauseMenu,         "Options",                                         S::Options,        K::Push,    false, false},
    {S::PauseMenu,         "Load (campaign)",                                 S::LoadGame,       K::Replace, false, false},
    {S::PauseMenu,         "Quit to Main Menu",                               S::GlueRoot,       K::Pop,     false, false},
    {S::PauseMenu,         "Exit (quit app)",                                 S::Boot,           K::Replace, false, false},
    {S::InGame,            "win/lose condition met",                          S::GameResult,     K::Replace, false, false},
    {S::GameResult,        "Continue",                                        S::Score,          K::Replace, false, false},
    {S::GameResult,        "Leave",                                           S::GlueRoot,       K::Pop,     false, true},
    {S::Score,             "Save Replay",                                     S::SaveReplay,     K::Push,    false, false},
    {S::Score,             "Continue / Done",                                 S::GlueRoot,       K::Pop,     false, true},
    {S::ReplayBrowse,      "Watch replay",                                    S::Loading,        K::Replace, false, false},
    {S::ReplayControls,    "end of replay",                                   S::Score,          K::Replace, false, false},
    {S::ReplayControls,    "Leave replay",                                    S::GlueRoot,       K::Pop,     false, true},

    {S::Loading,           "LoadingBarCancelButton",                          S::Lobby,          K::Replace, true,  true},
    {S::Home,              "BackButton",                                      S::GlueRoot,       K::Pop,     true,  true},
    {S::CampaignDifficulty,"CancelButton",                                    S::Campaign,       K::Pop,     true,  true},
    {S::VersusAI,          "BackButton",                                      S::GlueRoot,       K::Pop,     true,  true},
    {S::Custom,            "BackButton",                                      S::GlueRoot,       K::Pop,     true,  true},
    {S::Options,           "CancelButton",                                    S::GlueRoot,       K::Close,   true,  true},
    {S::LoadGame,          "CancelButton",                                    S::GlueRoot,       K::Pop,     true,  true},
    {S::SaveReplay,        "CancelButton",                                    S::Score,          K::Close,   true,  true},
    {S::ReplayBrowse,      "BackButton",                                      S::GlueRoot,       K::Pop,     true,  true},
    {S::Credits,           "CloseButton",                                     S::GlueRoot,       K::Pop,     true,  true},
    {S::SkirmishDebug,     "BackButton",                                      S::GlueRoot,       K::Pop,     true,  true},
    {S::GlueRoot,          "Esc / close client",                              S::Boot,           K::Replace, true,  true},
    {S::Options,           "HotkeysButton",                                   S::Hotkeys,        K::Push,    true,  false},
    {S::Hotkeys,           "CancelButton",                                    S::Options,        K::Close,   true,  true},
    {S::GlueRoot,          "PlayButton",                                      S::Lobby,          K::Replace, true,  false},
    {S::VersusAI,          "CustomButton",                                    S::Custom,         K::Replace, true,  false},
    {S::VersusAI,          "ReplayButton",                                    S::ReplayBrowse,   K::Replace, true,  false},
    {S::Home,              "PlayButton",                                      S::VersusAI,       K::Replace, true,  false},
    {S::GlueRoot,          "ProfileButton",                                   S::Profile,        K::Push,    true,  false},
    {S::Profile,           "BackButton",                                      S::GlueRoot,       K::Close,   true,  true},
    {S::GlueRoot,          "PlayNowButton",                                   S::Loading,        K::Replace, true,  false},
    {S::GlueRoot,          "WatchReplayButton",                               S::Loading,        K::Replace, true,  false},
    {S::Profile,           "PortraitButton",                                  S::Portraits,      K::Push,    true,  false},
    {S::Portraits,         "BackButton",                                      S::Profile,        K::Close,   true,  true},
    {S::GameResult,        "PlayAgainButton",                                 S::Loading,        K::Replace, true,  false},
    {S::GameResult,        "ChangeSetupButton",                               S::Lobby,          K::Replace, true,  false},
    {S::GameResult,        "WatchReplayButton",                               S::Loading,        K::Replace, true,  false},
    {S::Score,             "WatchAgainButton",                                S::Loading,        K::Replace, true,  false},
    {S::Boot,              "game folder known",                               S::Starting,       K::Replace, true,  false},
    {S::Boot,              "first run",                                       S::DataSetup,      K::Replace, true,  false},
    {S::DataSetup,         "ContinueButton",                                  S::Starting,       K::Replace, true,  false},
    {S::DataSetup,         "BackButton",                                      S::Boot,           K::Close,   true,  true},
    {S::Starting,          "startup complete",                                S::GlueRoot,       K::Replace, true,  false},
    {S::Starting,          "ChangeFolderButton",                              S::DataSetup,      K::Replace, true,  false},
    {S::Starting,          "QuitButton",                                      S::Boot,           K::Replace, true,  true},
    {S::Options,           "GameDataButton",                                  S::DataSetup,      K::Push,    true,  false},
    {S::Widgets,           "BackButton",                                      S::GlueRoot,       K::Pop,     true,  true},
    {S::Custom,            "CustomStartButton",                               S::Loading,        K::Replace, true,  false},
    {S::GameResult,        "ChangeCustomButton",                              S::Custom,         K::Replace, true,  false},
    {S::GlueRoot,          "CampaignSoonButton",                              S::Campaign,       K::Replace, true,  false},
    {S::GlueRoot,          "LanButton",                                       S::Lan,            K::Replace, true,  false},
    {S::Lan,               "BackButton",                                      S::GlueRoot,       K::Pop,     true,  true},
    {S::ReplayBrowse,      "ResumeButton",                                    S::Loading,        K::Replace, true,  false},
};

const char* const kLayoutBoot[] = {nullptr};
const char* const kLayoutLogin[] = {"Glue/ScreenLoginUnified.SC2Layout",
                                    "Glue/SelectToonDialog.SC2Layout",
                                    "Glue/CreateToonDialog.SC2Layout", nullptr};
const char* const kLayoutGlueRoot[] = {"Glue/GlueUI.SC2Layout", "Glue/ScreenNavigationSC2.SC2Layout",
                                       "Glue/ScreenBackgroundSC2.SC2Layout",
                                       "Glue/ScreenForegroundSC2.SC2Layout", nullptr};
const char* const kLayoutHome[] = {"Glue/ScreenHome.SC2Layout", "Glue/HomePanelSC2.SC2Layout",
                                   "Glue/WhatsNewPanelSC2.SC2Layout", nullptr};
const char* const kLayoutCampaign[] = {"Glue/ScreenSingle.SC2Layout", "Glue/CampaignPanel.SC2Layout",
                                       "Glue/CampaignSelectionPanel.SC2Layout",
                                       "Glue/CampaignInfoDialog.SC2Layout", nullptr};
const char* const kLayoutCampDiff[] = {"Glue/DifficultySelectionDialog.SC2Layout", nullptr};
const char* const kLayoutVersusAI[] = {"Glue/MatchmakingVersusAIPanel.SC2Layout", nullptr};
const char* const kLayoutCustom[] = {"Glue/ScreenCustom.SC2Layout", "Glue/CustomGamesMelee.SC2Layout",
                                     "Glue/CustomGamesLobbies.SC2Layout",
                                     "Glue/CustomGamesArcade.SC2Layout",
                                     "Glue/CustomGamesCustomCampaigns.SC2Layout", nullptr};
const char* const kLayoutLobby[] = {"Glue/ScreenBattleLobby.SC2Layout",
                                    "Glue/LobbyPlayerSlotsFrame.SC2Layout",
                                    "Glue/LobbyMatchOptionsFrame.SC2Layout",
                                    "Glue/LobbyGameInfoFrame.SC2Layout",
                                    "Glue/LobbyAttributePulldown.SC2Layout",
                                    "Glue/LobbyCountdownPanel.SC2Layout",
                                    "Glue/MapInfoPanel.SC2Layout", nullptr};
const char* const kLayoutSkirmish[] = {"Glue/ScreenSkirmish.SC2Layout", nullptr};
const char* const kLayoutOptions[] = {"Glue/OptionsDialog.SC2Layout",
                                      "Glue/GameplayOptionsPanel.SC2Layout",
                                      "Glue/GraphicsOptionsPanel.SC2Layout",
                                      "Glue/VideoOptionsPanel.SC2Layout",
                                      "Glue/SoundOptionsPanel.SC2Layout",
                                      "Glue/VoiceOptionsPanel.SC2Layout",
                                      "Glue/InputOptionsPanel.SC2Layout",
                                      "Glue/HotkeyDialog.SC2Layout",
                                      "Glue/ColorOptionsPanel.SC2Layout",
                                      "Glue/LanguageOptionsPanel.SC2Layout",
                                      "Glue/ObserverOptionsPanel.SC2Layout",
                                      "Glue/SocialOptionsPanel.SC2Layout",
                                      "Glue/StereoscopicOptionsPanel.SC2Layout", nullptr};
const char* const kLayoutLoadGame[] = {"Glue/LoadGameDialog.SC2Layout",
                                       "Glue/OfflineGameListPanel.SC2Layout", nullptr};
const char* const kLayoutSaveReplay[] = {"Glue/SaveReplayDialog.SC2Layout",
                                         "Glue/RenameReplayDialog.SC2Layout", nullptr};
const char* const kLayoutLoading[] = {"Glue/ScreenLoading.SC2Layout",
                                      "Glue/MultiplayerLoadingPanel.SC2Layout",
                                      "LoadingScreens/CoopLoadingShared.SC2Layout", nullptr};
const char* const kLayoutInGame[] = {"UI/ConsolePanel.SC2Layout", "UI/CommandPanel.SC2Layout",
                                     nullptr};
const char* const kLayoutPause[] = {"UI/GameMenuDialog.SC2Layout", "UI/MenuBar.SC2Layout", nullptr};
const char* const kLayoutGameResult[] = {"UI/VictoryPanel.SC2Layout", "UI/GameResultDialog.SC2Layout",
                                         nullptr};
const char* const kLayoutScore[] = {"Glue/ScreenScore.SC2Layout",
                                    "Glue/ScoreScreenSummaryFrame.SC2Layout",
                                    "Glue/ScoreScreenUnitsFrame.SC2Layout",
                                    "Glue/ScoreScreenGraphsFrame.SC2Layout",
                                    "Glue/ScoreScreenBuildOrderFrame.SC2Layout",
                                    "Glue/ScoreScreenExperienceFrame.SC2Layout",
                                    "Glue/ScoreScreenLadderFrame.SC2Layout",
                                    "Glue/ScoreScreenPerformanceFrame.SC2Layout",
                                    "Glue/ScoreScreenStatsFrameTemplates.SC2Layout", nullptr};
const char* const kLayoutReplayBrowse[] = {"Glue/ScreenReplay.SC2Layout",
                                           "Glue/LobbyReplayInfoFrame.SC2Layout", nullptr};
const char* const kLayoutReplayCtl[] = {"UI/ObserverPanel.SC2Layout", "UI/ReplayPanel.SC2Layout",
                                        nullptr};
const char* const kLayoutCredits[] = {"Glue/ScreenCreditsSC2.SC2Layout",
                                      "Glue/CreditsPanel.SC2Layout", nullptr};
const char* const kLayoutHotkeys[] = {"Glue/HotkeyDialog.SC2Layout", nullptr};
const char* const kLayoutProfile[] = {nullptr};
const char* const kLayoutNone[] = {nullptr};

const char* const* const kLayouts[] = {
    kLayoutBoot,      kLayoutLogin,     kLayoutGlueRoot,   kLayoutHome,        kLayoutCampaign,
    kLayoutCampDiff,  kLayoutVersusAI,  kLayoutCustom,     kLayoutLobby,       kLayoutSkirmish,
    kLayoutOptions,   kLayoutLoadGame,  kLayoutSaveReplay, kLayoutLoading,     kLayoutInGame,
    kLayoutPause,     kLayoutGameResult, kLayoutScore,     kLayoutReplayBrowse, kLayoutReplayCtl,
    kLayoutCredits,   kLayoutHotkeys,   kLayoutProfile,
    kLayoutNone,      kLayoutNone,      kLayoutNone,       kLayoutNone,       kLayoutNone,
};
static_assert(sizeof(kLayouts) / sizeof(kLayouts[0]) == std::size_t(ScreenId::Count),
              "every ScreenId needs a document row");

struct ControlAlias {
    ScreenId screen;
    const char* control;
    const char* trigger;
};
const ControlAlias kAliases[] = {
    {S::GlueRoot, "MultiplayerButton", "MultiplayerButton -> Versus AI"},
    {S::GlueRoot, "OptionsButton", "menu/Options"},
    {S::GlueRoot, "CreditsButton", "Credits entry"},
    {S::GlueRoot, "QuitButton", "Esc / close client"},
    {S::GlueRoot, "ExitButton", "Esc / close client"},
    {S::Campaign, "PlayButton", "select mission"},
    {S::SkirmishDebug, "PlayButton", "PlayButton (debug-gated)"},
    {S::LoadGame, "LoadButton", "Load selected save"},
    {S::Custom, "CreateButton", "select melee map -> Create"},
    {S::VersusAI, "FindMatchButton", "queue / Find Match vs AI"},
    {S::ReplayBrowse, "WatchButton", "Watch replay"},
    {S::Score, "ContinueButton", "Continue / Done"},
    {S::Score, "DoneButton", "Continue / Done"},
    {S::Score, "SaveReplayButton", "Save Replay"},
    {S::GameResult, "CloseButton", "Leave"},
    {S::GameResult, "ContinueButton", "Continue"},
    {S::CampaignDifficulty, "PlayButton", "Play / Start Mission"},
    {S::CampaignDifficulty, "OkButton", "Play / Start Mission"},
    {S::PauseMenu, "ResumeButton", "Resume"},
    {S::PauseMenu, "OptionsButton", "Options"},
    {S::PauseMenu, "QuitButton", "Quit to Main Menu"},
    {S::PauseMenu, "ExitButton", "Exit (quit app)"},
};

const std::vector<FlowEdge>& edges() {
    static const std::vector<FlowEdge> v(std::begin(kEdges), std::end(kEdges));
    return v;
}

}

const char* screen_flow_id(ScreenId id) {
    return id < ScreenId::Count ? kFlowIds[std::size_t(id)] : "";
}

ScreenId screen_from_flow_id(const std::string& flow_id) {
    if (flow_id.empty()) return ScreenId::Count;
    for (std::size_t i = 0; i < std::size_t(ScreenId::Count); ++i)
        if (flow_id == kFlowIds[i]) return ScreenId(i);
    return ScreenId::Count;
}

const char* screen_boot_name(ScreenId id) {
    if (id >= ScreenId::Count) return "";
    return id >= ScreenId::Hotkeys ? kEngineNames[std::size_t(id) - std::size_t(ScreenId::Hotkeys)] : kFlowIds[std::size_t(id)];
}

ScreenId screen_from_boot_name(const std::string& name) {
    if (name.empty()) return ScreenId::Count;
    for (std::size_t i = 0; i < std::size_t(ScreenId::Count); ++i)
        if (name == screen_boot_name(ScreenId(i))) return ScreenId(i);
    return ScreenId::Count;
}

const char* screen_name(ScreenId id) {
    return id < ScreenId::Count ? kNames[std::size_t(id)] : "?";
}

const std::vector<FlowEdge>& screen_flow_edges() { return edges(); }

const std::vector<const char*>& screen_layouts(ScreenId id) {
    static const std::vector<std::vector<const char*>> table = [] {
        std::vector<std::vector<const char*>> t(std::size_t(ScreenId::Count));
        for (std::size_t i = 0; i < t.size(); ++i)
            for (const char* const* p = kLayouts[i]; *p; ++p) t[i].push_back(*p);
        return t;
    }();
    static const std::vector<const char*> none;
    return id < ScreenId::Count ? table[std::size_t(id)] : none;
}

std::string trigger_for_control(ScreenId screen, const std::string& control) {
    for (const ControlAlias& a : kAliases)
        if (a.screen == screen && control == a.control) return a.trigger;
    return control;
}

ScreenRouter::ScreenRouter() : stack_{ScreenId::GlueRoot} {}

bool ScreenRouter::contains(ScreenId id) const {
    return std::find(stack_.begin(), stack_.end(), id) != stack_.end();
}

ScreenId ScreenRouter::beneath() const {
    return stack_.size() >= 2 ? stack_[stack_.size() - 2] : ScreenId::Count;
}

const FlowEdge* ScreenRouter::edge_for(const std::string& trigger) const {
    for (const FlowEdge& e : edges())
        if (e.from == top() && trigger == e.trigger) return &e;
    return nullptr;
}

const FlowEdge* ScreenRouter::cancel_edge() const {
    for (const FlowEdge& e : edges())
        if (e.from == top() && e.cancel) return &e;
    return nullptr;
}

void ScreenRouter::apply(const FlowEdge& e) {
    switch (e.kind) {
        case TransitionKind::Replace: replace(e.to); break;
        case TransitionKind::Push:    push(e.to);    break;
        case TransitionKind::Close:   close_top(e.to); break;
        case TransitionKind::Pop:     unwind_to(e.to); break;
    }
}

bool ScreenRouter::follow(const std::string& trigger) {
    const FlowEdge* e = edge_for(trigger);
    if (!e) return false;
    apply(*e);
    return true;
}

bool ScreenRouter::cancel() {
    const FlowEdge* e = cancel_edge();
    if (!e) return false;
    apply(*e);
    return true;
}

void ScreenRouter::replace(ScreenId id) { stack_.back() = id; }

void ScreenRouter::push(ScreenId id) { stack_.push_back(id); }

void ScreenRouter::close_top(ScreenId fallback) {
    if (stack_.size() > 1)
        stack_.pop_back();
    else
        stack_.back() = fallback;
}

void ScreenRouter::unwind_to(ScreenId id) {
    while (stack_.size() > 1 && stack_.back() != id) stack_.pop_back();
    if (stack_.back() != id) stack_.assign(1, id);
}

}
