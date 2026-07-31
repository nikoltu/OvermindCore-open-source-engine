#pragma once

namespace sc2::render {

enum class Lod { High, Medium, Low, Billboard, Culled };

inline Lod lod_for_distance(float dist, float high, float med, float low, float cull) {
    if (dist < high) return Lod::High;
    if (dist < med) return Lod::Medium;
    if (dist < low) return Lod::Low;
    if (dist < cull) return Lod::Billboard;
    return Lod::Culled;
}

inline int lod_anim_stride(Lod lod) {
    switch (lod) {
        case Lod::High: return 1;
        case Lod::Medium: return 2;
        case Lod::Low: return 4;
        case Lod::Billboard: return 8;
        case Lod::Culled: return 0;
    }
    return 1;
}

}
