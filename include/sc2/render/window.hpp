#pragma once

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

#include "sc2/render/scene.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::render {

struct WindowUnavailable : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct WindowResult {
    bool created = false;
    uint32_t frames_presented = 0;
    uint32_t width = 0, height = 0;
    bool validation_active = false;
    uint32_t validation_errors = 0;
};

WindowResult render_window(const std::vector<SceneInstance>& instances,
                           const Camera* cam = nullptr,
                           const casc::Storage* storage = nullptr,
                           uint32_t width = 1280, uint32_t height = 720,
                           uint32_t frames = 3,
                           const std::string& texture_root = "");

}
