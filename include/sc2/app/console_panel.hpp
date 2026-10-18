#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/casc/storage.hpp"

namespace sc2::data {
class Catalog;
struct EnvLight;
}

namespace sc2::formats {
struct Model;
}

namespace sc2::app {

struct ConsoleTexture {
    std::uint32_t w = 0, h = 0;
    std::vector<std::uint8_t> rgba;
    bool ok() const { return w && h && rgba.size() == std::size_t(w) * h * 4; }
};

const char* const* console_asset_roots();

constexpr std::uint32_t kConsoleFrameWidth = 992;

ConsoleTexture load_console(casc::Storage& store, int race, const data::Catalog* cat = nullptr);

inline float console_ortho_half_h(float model_span_x, float frame_w, float frame_h) {
    if (!(model_span_x > 0.0f) || !(frame_w > 0.0f) || !(frame_h > 0.0f)) return 0.0f;
    return 0.5f * model_span_x * (frame_h / frame_w);
}

struct ConsoleView {
    float half_h = 0.0f;
    float centre_z = 0.0f;
    std::uint32_t height = 0;
    const data::EnvLight* map_env = nullptr;

    float frame_w = 0.0f;
    float frame_h = 0.0f;
};

inline float& portrait_time_ms() {
    static float t = 0.0f;
    return t;
}
struct PortraitDof {
    float focus = 0.0f, start = 0.0f, end = 0.0f, amount = 0.0f, px_scale = 1.0f;
};
inline PortraitDof& portrait_dof() {
    static PortraitDof d;
    return d;
}
std::vector<std::uint8_t> render_portrait(casc::Storage& store, const formats::Model& model, float fov_rad,
                                          int cam_bone, float znear, float zfar, std::uint32_t w, std::uint32_t h,
                                          const data::EnvLight* env = nullptr);
ConsoleTexture render_console_models(casc::Storage& store, const data::Catalog& cat,
                                     const std::string& skin_id, std::uint32_t width,
                                     const ConsoleView& view = {});

const char* console_skin_id_for_race(int race);

ConsoleTexture load_terran_console(casc::Storage& store);

void composite_console(std::vector<std::uint8_t>& img, std::uint32_t view_w,
                       std::uint32_t view_h, const std::uint8_t* rgba, int tw, int th);

}
