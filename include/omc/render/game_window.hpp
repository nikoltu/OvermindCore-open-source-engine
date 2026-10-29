#pragma once

#include <functional>
#include <string>
#include <vector>

#include "omc/data/environment.hpp"
#include "omc/render/fog_field.hpp"
#include "omc/render/input.hpp"
#include "omc/render/scene.hpp"
#include "omc/render/window.hpp"

namespace omc::casc {
class Storage;
}

namespace omc::render {

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

using OffscreenDumpFn = std::function<void(uint32_t width, uint32_t height, const std::vector<uint8_t>& scene_rgba,
                                            const std::vector<uint8_t>& overlay_texture_rgba, const std::vector<uint8_t>& overlay_cpu_rgba)>;

WindowResult run_game_offscreen(const GameFrameFn& frame, const casc::Storage* storage, uint32_t width, uint32_t height, uint32_t frames,
                                const std::function<data::EnvLight()>& env_provider = {}, float shadow_clip = 0.0f,
                                const std::function<const FogField*()>& fog_provider = {}, const OffscreenDumpFn& dump_last_frame = {},
                                const std::function<bool()>& dump_this_frame = {});

}
