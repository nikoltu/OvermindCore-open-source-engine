#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

struct RevealPoint {
    Fixed x, y;
    std::uint32_t unit = kNoTarget;
};

class ParasiteSystem {
public:
    bool attach(const World& world, std::uint32_t target, std::int32_t spy) {
        if (target >= world.units().size() || !world.units()[target].alive) return false;
        tags_[target] = spy;
        return true;
    }
    void cleanse(std::uint32_t target) { tags_.erase(target); }
    bool parasited(std::uint32_t target) const { return tags_.find(target) != tags_.end(); }
    std::size_t count() const { return tags_.size(); }

    std::vector<RevealPoint> revealed_for(const World& world, std::int32_t spy) const {
        std::vector<RevealPoint> out;
        for (const auto& kv : tags_) {
            if (kv.second != spy) continue;
            const std::uint32_t id = kv.first;
            if (id >= world.units().size() || !world.units()[id].alive) continue;
            const Unit& u = world.units()[id];
            out.push_back(RevealPoint{u.x, u.y, id});
        }
        return out;
    }

    void prune(const World& world) {
        for (auto it = tags_.begin(); it != tags_.end();) {
            if (it->first >= world.units().size() || !world.units()[it->first].alive)
                it = tags_.erase(it);
            else
                ++it;
        }
    }

private:
    std::unordered_map<std::uint32_t, std::int32_t> tags_;
};

}
