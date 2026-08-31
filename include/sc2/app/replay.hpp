#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::app {

struct TimedOrder {
    int tick = 0;
    uint32_t unit_id = 0;
    sim::Order order;
    bool queued = false;
    bool is_stop = false;
    bool is_command = false;
    sim::Command command;
};

enum class RecordKind : uint8_t { Order, Stop, Command };
inline RecordKind record_kind(const TimedOrder& t) {
    if (t.is_command) return RecordKind::Command;
    return t.is_stop ? RecordKind::Stop : RecordKind::Order;
}

void apply_timed_order(sim::World& w, const TimedOrder& t);

struct OrderLog {
    uint64_t seed = 1;
    std::string map_id;
    std::vector<int32_t> player_races;
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
    TimedOrder t;
    t.tick = w.tick();
    t.unit_id = id;
    t.order = o;
    t.queued = queued;
    log.orders.push_back(t);
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

class LogRecorder : public sim::OrderSink {
public:
    explicit LogRecorder(OrderLog& log) : log_(log) {}
    void on_order(int tick, uint32_t id, const sim::Order& o, bool queued) override {
        TimedOrder t;
        t.tick = tick;
        t.unit_id = id;
        t.order = o;
        t.queued = queued;
        log_.orders.push_back(t);
    }
    void on_stop(int tick, uint32_t id) override {
        TimedOrder t;
        t.tick = tick;
        t.unit_id = id;
        t.is_stop = true;
        log_.orders.push_back(t);
    }
    void on_command(int tick, uint32_t id, const sim::Command& c) override {
        TimedOrder t;
        t.tick = tick;
        t.unit_id = id;
        t.is_command = true;
        t.command = c;
        log_.orders.push_back(t);
    }

private:
    OrderLog& log_;
};

inline constexpr char kOrderLogMagic[8] = {'S', 'C', '2', 'R', 'L', 'O', 'G', '\0'};
inline constexpr uint32_t kOrderLogVersion = 3;

void write_order_log(const OrderLog& log, std::vector<uint8_t>& out);
bool read_order_log(const uint8_t* data, size_t len, OrderLog& out);
bool write_order_log_file(const OrderLog& log, const std::string& path);
bool read_order_log_file(const std::string& path, OrderLog& out);

}
