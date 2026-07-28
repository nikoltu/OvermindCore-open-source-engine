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

}
