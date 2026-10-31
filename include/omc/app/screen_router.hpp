#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace omc::app {

enum class ScreenId {
    Boot,
    Login,
    GlueRoot,
    Home,
    Campaign,
    CampaignDifficulty,
    VersusAI,
    Custom,
    Lobby,
    SkirmishDebug,
    Options,
    LoadGame,
    SaveReplay,
    Loading,
    InGame,
    PauseMenu,
    GameResult,
    Score,
    ReplayBrowse,
    ReplayControls,
    Credits,
    Hotkeys,
    Profile,
    Portraits,
    DataSetup,
    Starting,
    Widgets,
    Lan,
    Count,
};

const char* screen_flow_id(ScreenId id);
ScreenId screen_from_flow_id(const std::string& flow_id);
const char* screen_boot_name(ScreenId id);
ScreenId screen_from_boot_name(const std::string& name);
const char* screen_name(ScreenId id);

enum class TransitionKind { Replace, Push, Close, Pop };

struct FlowEdge {
    ScreenId from;
    const char* trigger;
    ScreenId to;
    TransitionKind kind;
    bool engine_only;
    bool cancel;
};

const std::vector<FlowEdge>& screen_flow_edges();

const std::vector<const char*>& screen_layouts(ScreenId id);

std::string trigger_for_control(ScreenId screen, const std::string& control);

class ScreenRouter {
public:
    ScreenRouter();

    ScreenId top() const { return stack_.back(); }
    std::size_t depth() const { return stack_.size(); }
    ScreenId at(std::size_t i) const { return stack_[i]; }
    bool contains(ScreenId id) const;
    ScreenId beneath() const;

    const FlowEdge* edge_for(const std::string& trigger) const;
    const FlowEdge* cancel_edge() const;

    bool follow(const std::string& trigger);
    bool cancel();

    void replace(ScreenId id);
    void push(ScreenId id);
    void unwind_to(ScreenId id);
    void close_top(ScreenId fallback);

private:
    void apply(const FlowEdge& e);
    std::vector<ScreenId> stack_;
};

}
