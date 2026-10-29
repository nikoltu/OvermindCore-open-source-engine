#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "omc/formats/m3.hpp"
#include "omc/render/mat4.hpp"

namespace omc::render {

struct ClipLayer {
    uint32_t seq = 0;
    float time_ms = 0.0f;
};
using ClipLayers = std::vector<ClipLayer>;

int select_sequence(const formats::Model& model,
                    const std::vector<std::string>& tokens, uint64_t seed = 0);

int select_variation(const formats::Model& model, const std::string& token, uint64_t seed, double clock_ms, float* time_ms);

struct VariationTakes {
    std::vector<int> cands;
    uint64_t total = 0;
};
VariationTakes variation_takes(const formats::Model& model, const std::string& token);

struct VariationWalk {
    bool valid = false;
    uint64_t seed = 0;
    double clock_ms = 0.0;
    uint64_t loop = 0;
    double consumed_ms = 0.0;
};
int select_variation(const formats::Model& model, const VariationTakes& takes, uint64_t seed, double clock_ms, float* time_ms,
                     VariationWalk& walk);
int select_take(const formats::Model& model, int seq, uint64_t seed, uint32_t loop);

std::vector<Mat4> pose(const formats::Model& model, uint32_t seq, float time_ms);
bool region_visible(const formats::Model& model, int seq, float time_ms, std::size_t region, const ClipLayers* global = nullptr);
std::vector<Mat4> pose(const formats::Model& model, uint32_t seq, float time_ms, int yaw_bone, float yaw_rad,
                       const ClipLayers* global = nullptr);

std::vector<Mat4> pose_world(const formats::Model& model, uint32_t seq, float time_ms);
std::vector<Mat4> pose_world(const formats::Model& model, uint32_t seq, float time_ms, int yaw_bone, float yaw_rad,
                             const ClipLayers* global = nullptr);

bool posed_bounds(const formats::Model& model, int seq, float time_ms, float mn[3], float mx[3]);

void collect_events(const formats::Model& model, uint32_t seq, float t0, float t1,
                    std::vector<formats::M3Event>& out);

int attachment_index(const formats::Model& model, const std::string& name);

void animated_layer_field(const formats::Model& model, uint32_t seq, uint16_t interp, uint16_t flags, uint32_t anim_id,
                          float time_ms, int comps, const float* fallback, float* out,
                          const ClipLayers* global = nullptr);
float animated_scalar(const formats::Model& model, uint32_t seq, const formats::M3AnimRef& ref,
                      float time_ms, float fallback, const ClipLayers* global = nullptr);

bool attachment_transform(const formats::Model& model, const std::string& name,
                          uint32_t seq, float time_ms, Mat4& out);

}
