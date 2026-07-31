#pragma once

#include <cstdint>
#include <vector>

#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"
#include "sc2/sim/nav.hpp"

namespace sc2::app {

struct TestMap {
    enum class ResourceKind { Mineral, Geyser };

    struct Resource {
        render::Vec3 position;
        ResourceKind kind = ResourceKind::Mineral;
    };

    struct Base {
        render::Vec3 town;
        std::vector<Resource> resources;

        int count(ResourceKind k) const {
            int n = 0;
            for (const Resource& r : resources)
                if (r.kind == k) ++n;
            return n;
        }
        int mineral_count() const { return count(ResourceKind::Mineral); }
        int geyser_count() const { return count(ResourceKind::Geyser); }
    };

    struct PlayerSites {
        Base main;
        Base natural;
    };

    float width = 0.0f;
    float height = 0.0f;
    std::vector<PlayerSites> players;
    render::Vec3 contested;
    formats::Model ground;
    sim::NavGrid nav;

    static constexpr int kMineralsPerBase = 8;
    static constexpr int kGeysersPerBase = 1;

    static constexpr float kBaseMineralDistance = 6.0f;
    static constexpr float kBaseGeyserDistance = 6.75f;

    static TestMap generate(float width, float height, int subdiv = 16);

    render::Vec3 center() const { return {width * 0.5f, height * 0.5f, 0.0f}; }
};

}
