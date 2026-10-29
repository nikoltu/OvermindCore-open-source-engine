#pragma once

#include <cstdint>
#include <vector>

namespace omc::sim {

class World;
struct Unit;

struct PlayerScore {
    std::int32_t units_alive = 0;
    std::int32_t structures_alive = 0;
    std::int32_t units_produced = 0;
    std::int32_t units_lost = 0;
    std::int32_t structures_built = 0;
    std::int32_t structures_lost = 0;
    std::int32_t minerals = 0;
    std::int32_t gas = 0;
    bool defeated = false;
    bool ever_had_assets = false;
};

class ScoreKeeper {
public:
    template <typename IsStructure>
    void observe(const World& world, IsStructure is_structure);

    const PlayerScore& player(int p) const {
        static const PlayerScore kEmpty;
        return (p >= 0 && std::size_t(p) < scores_.size()) ? scores_[p] : kEmpty;
    }
    std::size_t players() const { return scores_.size(); }

    int living_players() const {
        int n = 0;
        for (const PlayerScore& s : scores_)
            if (s.ever_had_assets && !s.defeated) ++n;
        return n;
    }
    int winner() const {
        int w = -1, n = 0;
        for (std::size_t p = 0; p < scores_.size(); ++p)
            if (scores_[p].ever_had_assets && !scores_[p].defeated) {
                ++n;
                w = int(p);
            }
        return n == 1 ? w : -1;
    }

private:
    PlayerScore& slot(int p) {
        if (std::size_t(p) >= scores_.size()) scores_.resize(std::size_t(p) + 1);
        return scores_[p];
    }

    std::vector<PlayerScore> scores_;
    struct Shadow {
        bool alive = false;
        std::int32_t player = -1;
        bool structure = false;
        bool seen = false;
    };
    std::vector<Shadow> shadow_;
};

}

#include "omc/sim/world.hpp"

namespace omc::sim {

template <typename IsStructure>
void ScoreKeeper::observe(const World& world, IsStructure is_structure) {
    const std::vector<Unit>& us = world.units();
    if (shadow_.size() < us.size()) shadow_.resize(us.size());

    for (std::size_t i = 0; i < us.size(); ++i) {
        const Unit& u = us[i];
        Shadow& sh = shadow_[i];
        const bool structure = is_structure(u);

        if (!sh.seen) {
            sh.seen = true;
            sh.player = u.player;
            sh.structure = structure;
            sh.alive = u.alive;
            if (u.alive && u.player >= 0) {
                PlayerScore& s = slot(u.player);
                s.ever_had_assets = true;
                if (structure) ++s.structures_built;
                else ++s.units_produced;
            }
            continue;
        }
        if (sh.alive && !u.alive && sh.player >= 0) {
            PlayerScore& s = slot(sh.player);
            if (sh.structure) ++s.structures_lost;
            else ++s.units_lost;
        }
        sh.alive = u.alive;
    }

    for (PlayerScore& s : scores_) {
        s.units_alive = 0;
        s.structures_alive = 0;
    }
    for (const Unit& u : us) {
        if (!u.alive || u.player < 0) continue;
        PlayerScore& s = slot(u.player);
        if (is_structure(u)) ++s.structures_alive;
        else ++s.units_alive;
    }
    for (std::size_t p = 0; p < scores_.size(); ++p) {
        PlayerScore& s = scores_[p];
        s.minerals = world.player(int(p)).minerals;
        s.gas = world.player(int(p)).gas;
        if (s.ever_had_assets && s.units_alive == 0 && s.structures_alive == 0)
            s.defeated = true;
    }
}

}
