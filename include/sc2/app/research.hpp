#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/sim/fixed.hpp"

namespace sc2::app {

class ResearchState {
public:
    using Fixed = sim::Fixed;

    void begin(int player, const std::string& id, Fixed delay) {
        if (has(player, id)) return;
        for (const Pending& p : pending_)
            if (p.player == player && p.id == id) return;
        pending_.push_back(Pending{player, id, delay});
    }

    void grant(int player, const std::string& id) {
        if (!has(player, id)) done_[player].push_back(id);
    }

    void advance(Fixed dt) {
        for (std::size_t i = 0; i < pending_.size();) {
            pending_[i].timer = pending_[i].timer - dt;
            if (pending_[i].timer.raw() <= 0) {
                grant(pending_[i].player, pending_[i].id);
                pending_.erase(pending_.begin() + i);
            } else {
                ++i;
            }
        }
    }

    const std::vector<std::string>& researched(int player) const {
        auto it = done_.find(player);
        return it == done_.end() ? kEmpty : it->second;
    }

    bool has(int player, const std::string& id) const {
        auto it = done_.find(player);
        if (it == done_.end()) return false;
        for (const std::string& u : it->second)
            if (u == id) return true;
        return false;
    }

    int in_progress() const { return int(pending_.size()); }

private:
    struct Pending {
        int player;
        std::string id;
        Fixed timer;
    };
    std::unordered_map<int, std::vector<std::string>> done_;
    std::vector<Pending> pending_;
    inline static const std::vector<std::string> kEmpty{};
};

}
