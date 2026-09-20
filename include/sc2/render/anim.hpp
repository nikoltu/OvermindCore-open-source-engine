#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"

namespace sc2::render {

int select_sequence(const formats::Model& model,
                    const std::vector<std::string>& tokens, uint64_t seed = 0);

std::vector<Mat4> pose(const formats::Model& model, uint32_t seq, float time_ms);

std::vector<Mat4> pose_world(const formats::Model& model, uint32_t seq, float time_ms);

bool posed_bounds(const formats::Model& model, int seq, float time_ms, float mn[3], float mx[3]);

void collect_events(const formats::Model& model, uint32_t seq, float t0, float t1,
                    std::vector<formats::M3Event>& out);

int attachment_index(const formats::Model& model, const std::string& name);

void animated_layer_field(const formats::Model& model, uint32_t seq, uint16_t interp, uint16_t flags, uint32_t anim_id,
                          float time_ms, int comps, const float* fallback, float* out);
float animated_scalar(const formats::Model& model, uint32_t seq, const formats::M3AnimRef& ref,
                      float time_ms, float fallback);

bool attachment_transform(const formats::Model& model, const std::string& name,
                          uint32_t seq, float time_ms, Mat4& out);

}
