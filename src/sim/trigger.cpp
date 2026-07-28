#include "sc2/sim/trigger.hpp"

namespace sc2::sim {

bool eval_unit_count(const World& w, int32_t player, Cmp op, int32_t n) {
    int c = w.alive_count(player);
    switch (op) {
        case Cmp::Ge: return c >= n;
        case Cmp::Le: return c <= n;
        case Cmp::Eq: return c == n;
    }
    return false;
}

bool eval_elapsed(const World& w, int32_t ticks) {
    return w.tick() >= ticks;
}

void TriggerRuntime::mark_active(int32_t p) {
    for (std::size_t i = 0; i < active_players_.size(); ++i) {
        if (active_players_[i] == p) return;
        if (active_players_[i] > p) {
            active_players_.insert(active_players_.begin() + std::ptrdiff_t(i), p);
            return;
        }
    }
    active_players_.push_back(p);
}

bool TriggerRuntime::is_defeated(int32_t p) const {
    for (int32_t d : defeated_)
        if (d == p) return true;
    return false;
}

std::size_t TriggerRuntime::observe(const World& w) {
    const std::size_t before = events_.size();
    const int32_t tick = int32_t(w.tick());

    for (const Unit& u : w.units())
        if (u.alive && u.player >= 0) mark_active(u.player);

    for (int32_t p : active_players_) {
        if (!is_defeated(p) && w.alive_count(p) == 0) {
            defeated_.push_back(p);
            events_.push_back(TriggerEvent{TriggerEvent::Kind::Defeat, p, tick});
        }
    }

    if (!victory_done_ && active_players_.size() >= 2) {
        int live_players = 0;
        int32_t sole = -1;
        for (int32_t p : active_players_) {
            if (w.alive_count(p) > 0) {
                ++live_players;
                sole = p;
            }
        }
        if (live_players == 1) {
            victory_done_ = true;
            events_.push_back(TriggerEvent{TriggerEvent::Kind::Victory, sole, tick});
        }
    }

    return events_.size() - before;
}

int32_t TriggerRuntime::winner() const {
    for (const TriggerEvent& e : events_)
        if (e.kind == TriggerEvent::Kind::Victory) return e.player;
    return -1;
}

bool TriggerRuntime::defeat_fired(int32_t player) const {
    for (const TriggerEvent& e : events_)
        if (e.kind == TriggerEvent::Kind::Defeat && e.player == player) return true;
    return false;
}

}
