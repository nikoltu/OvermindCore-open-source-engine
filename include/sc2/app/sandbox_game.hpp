#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "sc2/app/melee_game.hpp"
#include "sc2/app/test_map.hpp"
#include "sc2/render/input.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::casc {
class Storage;
}
namespace sc2::data {
class Catalog;
}
namespace sc2::formats {
struct Model;
}

namespace sc2::app {

enum class Race { Terran, Protoss, Zerg };

enum class LobbyRace { Terran, Protoss, Zerg, Random };
inline LobbyRace lobby_race_cycle(LobbyRace r) {
    return r == LobbyRace::Terran ? LobbyRace::Protoss : r == LobbyRace::Protoss ? LobbyRace::Zerg
         : r == LobbyRace::Zerg ? LobbyRace::Random : LobbyRace::Terran;
}
inline const char* lobby_race_name(LobbyRace r) {
    return r == LobbyRace::Terran ? "Terran" : r == LobbyRace::Protoss ? "Protoss" : r == LobbyRace::Zerg ? "Zerg" : "Random";
}
inline Race resolve_lobby_race(LobbyRace pick, std::uint64_t seed, int slot) {
    if (pick != LobbyRace::Random) return Race(int(pick));
    std::uint64_t z = seed + 0x9E3779B97F4A7C15ull * std::uint64_t(slot + 1);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    return Race(int(z % 3));
}

struct RaceOpening {
    const char* town_hall;
    const char* worker;
    const char* army;
    const char* race_name;
};
RaceOpening race_opening(Race r);

class SandboxGame {
public:
    SandboxGame(data::Catalog& cat, MeleeModels& models, casc::Storage& store,
                const TestMap& map);

    void reset(Race race);
    Race race() const { return race_; }
    MeleeGame* game() { return game_.get(); }

    bool step(const render::InputState& in, float dt_ms,
              std::vector<render::SceneInstance>& scene, render::Camera& cam);

    void build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                       std::vector<std::uint8_t>& out);

private:
    struct Button {
        int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
        std::string label;
        std::string token;
        bool hit(float mx, float my) const {
            return mx >= float(x0) && mx < float(x1) && my >= float(y0) && my < float(y1);
        }
    };
    std::vector<Button> spawn_buttons(std::uint32_t view_w, std::uint32_t view_h) const;
    std::vector<Button> race_buttons(std::uint32_t view_w, std::uint32_t view_h) const;
    void spawn_token(const std::string& token);

    data::Catalog& cat_;
    MeleeModels& models_;
    casc::Storage& store_;
    const TestMap& map_;

    std::unique_ptr<MeleeGame> game_;
    Race race_ = Race::Terran;
    sim::Fixed marker_x_, marker_y_;

    bool debug_ = false;
    bool prev_f4_ = false;
    bool prev_left_ = false;
    int spawn_seq_ = 0;
};

}
