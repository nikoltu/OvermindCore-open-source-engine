#pragma once

#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::app {

struct TimedOrder {
    int tick = 0;
    uint32_t unit_id = 0;
    sim::Order order;
    bool queued = false;
};

struct OrderLog {
    uint64_t seed = 1;
    std::vector<sim::Unit> initial_units;
    std::vector<TimedOrder> orders;
    int total_ticks = 0;
};

struct ReplayTrace {
    std::vector<uint64_t> hashes;

    uint64_t final_hash() const { return hashes.empty() ? 0 : hashes.back(); }
    size_t size() const { return hashes.size(); }
    bool operator==(const ReplayTrace& o) const { return hashes == o.hashes; }
    bool operator!=(const ReplayTrace& o) const { return hashes != o.hashes; }
};

inline void record_order(OrderLog& log, const sim::World& w, uint32_t id,
                         const sim::Order& o, bool queued = false) {
    log.orders.push_back(TimedOrder{w.tick(), id, o, queued});
}

class Recorder {
public:
    explicit Recorder(uint64_t seed = 1) : world_(seed) { log_.seed = seed; }

    uint32_t add_unit(sim::Unit u) {
        log_.initial_units.push_back(u);
        return world_.add_unit(u);
    }

    void issue(uint32_t id, const sim::Order& o, bool queued = false) {
        record_order(log_, world_, id, o, queued);
        world_.issue(id, o, queued);
    }

    void step() {
        world_.step();
        log_.total_ticks = world_.tick();
        trace_.hashes.push_back(world_.state_hash());
    }
    void run(int ticks) {
        for (int i = 0; i < ticks; ++i) step();
    }

    const OrderLog& log() const { return log_; }
    const ReplayTrace& trace() const { return trace_; }
    sim::World& world() { return world_; }
    const sim::World& world() const { return world_; }
    int tick() const { return world_.tick(); }

private:
    sim::World world_;
    OrderLog log_;
    ReplayTrace trace_;
};

ReplayTrace replay(const OrderLog& log);

}
