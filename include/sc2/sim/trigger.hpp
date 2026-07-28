#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::sim {

enum class Cmp { Ge, Le, Eq };

bool eval_unit_count(const World& w, int32_t player, Cmp op, int32_t n);

bool eval_elapsed(const World& w, int32_t ticks);

struct TriggerEvent {
    enum class Kind { Victory, Defeat };
    Kind kind;
    int32_t player;
    int32_t tick;
};

class TriggerRuntime {
public:
    std::size_t observe(const World& w);

    const std::vector<TriggerEvent>& events() const { return events_; }

    bool victory_fired() const { return victory_done_; }
    int32_t winner() const;
    bool defeat_fired(int32_t player) const;

private:
    std::vector<TriggerEvent> events_;
    std::vector<int32_t> active_players_;
    std::vector<int32_t> defeated_;
    bool victory_done_ = false;

    void mark_active(int32_t p);
    bool is_defeated(int32_t p) const;
};

}
