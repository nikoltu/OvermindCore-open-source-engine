#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "omc/formats/m3.hpp"

namespace omc::data {
class Catalog;
}

namespace omc::app {

struct ConfirmActorSpec {
    std::string actor;
    std::string model_id;
    bool attack = false;
    int air = -1;
    bool tinted = false;
    std::array<float, 3> tint = {1.0f, 1.0f, 1.0f};
};
std::vector<ConfirmActorSpec> confirmation_actor_specs(const data::Catalog& cat);
const ConfirmActorSpec* pick_confirmation(const std::vector<ConfirmActorSpec>& specs, bool attack, bool air);

struct FlashLook {
    bool found = false;
    std::string actor;
    float alpha = 0.75f;
    float inner_width = 0.15f;
    float outer_width = 0.03f;
    float rotation = 8.0f;
    int segments = 4;
    float solid = 0.4f;
    float hold_s = 1.5f;
};
FlashLook selection_flash_look(const data::Catalog& cat);

double effect_area_radius(const data::Catalog& cat, const std::string& effect);

struct CursorSplatSpec {
    bool found = false;
    std::string actor;
    std::string model_id;
    std::string effect;
    double radius = 0.0;
};
std::unordered_map<std::string, std::string> cursor_splat_actors(const data::Catalog& cat);
CursorSplatSpec ability_cursor_splat(const data::Catalog& cat, const std::string& abil);
CursorSplatSpec ability_cursor_splat(const data::Catalog& cat, const std::unordered_map<std::string, std::string>& actors,
                                     const std::string& abil);

struct OrderDisplay {
    bool found = false;
    std::string ability;
    std::string display_type;
    std::array<std::uint8_t, 4> argb = {255, 0, 255, 0};
    std::string model_path;
    std::string line_texture;
};
OrderDisplay ability_order_display(const data::Catalog& cat, const std::string& unit_id, const std::string& abil_tag,
                                   int index);
std::string command_harness_model(const data::Catalog& cat, const std::string& display_type, const std::string& race);

struct PathLook {
    float width = 0.15f;
    float tile = 0.8f;
    float fade_point = 0.9f;
    float fade_alpha = 0.25f;
    std::string texture = "Assets/Textures/WayPointLine.dds";
    std::array<std::uint8_t, 4> argb = {255, 255, 255, 255};
    std::string step_model;
    float step_scale = 1.0f;
};
PathLook waypoint_path_look(const data::Catalog& cat);

std::string placeholder_actor(const data::Catalog& cat, const std::string& unit_id);
float placeholder_opacity(const data::Catalog& cat, const std::string& placeholder_actor_id, float fallback);

struct OrderCueState {
    enum Kind { kMove = 0, kPatrol, kAttack, kConfirm, kKinds };
    struct Look {
        bool resolved = false;
        std::array<float, 3> rgb = {0.0f, 1.0f, 0.0f};
        const formats::Model* marker = nullptr;
        int seq = -1;
        float len_ms = 0.0f;
    };
    struct GhostLook {
        const formats::Model* model = nullptr;
        std::array<float, 3> scale = {1.0f, 1.0f, 1.0f};
        float face = 0.0f;
        float opacity = 0.5f;
        int seq = -1;
    };
    std::unordered_map<std::string, GhostLook> ghosts;
    std::unordered_map<std::string, const formats::Model*> harness;
    std::unordered_map<std::string, std::array<float, 3>> rally_rgb;
    bool loaded = false;
    std::vector<ConfirmActorSpec> confirms;
    FlashLook flash;
    PathLook path;
    std::unordered_map<std::string, std::string> splat_actor_by_abil;

    struct Marker {
        bool active = false;
        const formats::Model* model = nullptr;
        std::string model_id;
        float x = 0.0f, y = 0.0f, z = 0.0f;
        double start_ms = 0.0;
        float len_ms = 0.0f;
        int seq = -1;
        bool attack = false;
        std::array<float, 3> tint = {1.0f, 1.0f, 1.0f};
        int count = 0;
    } marker;

    struct Flash {
        bool active = false;
        std::uint32_t target = 0xFFFFFFFFu;
        double start_ms = 0.0;
        std::array<float, 3> rgb = {0.0f, 1.0f, 0.0f};
        bool enemy = false;
        int count = 0;
    } target_flash;
    formats::Model flash_ring;
    bool flash_ring_built = false;

    struct Cursor {
        bool active = false;
        std::string abil;
        float x = 0.0f, y = 0.0f;
        double radius = 0.0;
        std::string actor;
        const formats::Model* model = nullptr;
        bool quad = false;
        float scale = 1.0f;
    } cursor;
    struct CursorAsset {
        CursorSplatSpec spec;
        formats::Model quad;
        const formats::Model* mesh = nullptr;
        float mesh_scale = 1.0f;
        bool has_quad = false;
    };
    std::unordered_map<std::string, CursorAsset> cursor_assets;

    struct Placeholder {
        std::uint32_t worker = 0xFFFFFFFFu;
        float x = 0.0f, y = 0.0f;
        std::string unit_id;
    };
    std::vector<Placeholder> issued;

    std::unordered_map<std::string, std::array<Look, kKinds>> looks;

    std::size_t placeholders_drawn = 0;
    std::size_t path_legs_drawn = 0;
    std::size_t path_markers_drawn = 0;
    std::size_t cursor_drawn = 0;
    float last_path_half_px = 0.0f;
    float last_rally_half_px = 0.0f;
};

}
