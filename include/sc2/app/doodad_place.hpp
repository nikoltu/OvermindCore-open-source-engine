#pragma once

#include "sc2/formats/placed_objects.hpp"
#include "sc2/render/mat4.hpp"

namespace sc2::app {

inline render::Mat4 doodad_world_matrix(const formats::PlacedDoodad& d) {
    return render::translation(d.x, d.y, d.z) * render::rotation_z(d.rotation) *
           render::scaling(d.sx, d.sy, d.sz);
}

}
