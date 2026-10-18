#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sc2/data/environment.hpp"
#include "sc2/render/fog_field.hpp"
#include "sc2/render/input.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/render/window.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::render {

using GameFrameFn = std::function<bool(const InputState& in, float dt_ms,
                                       std::vector<SceneInstance>& scene, Camera& cam,
                                       std::vector<uint8_t>& overlay_rgba)>;

void set_os_cursor_hidden(bool hidden);

WindowResult run_game_window(const GameFrameFn& frame,
                             const casc::Storage* storage = nullptr,
                             uint32_t width = 1280, uint32_t height = 720,
                             const std::string& texture_root = "",
                             uint32_t max_frames = 0,
                             const std::function<data::EnvLight()>& env_provider = {},
                             float shadow_clip = 0.0f,
                             const std::function<const FogField*()>& fog_provider = {});

}
