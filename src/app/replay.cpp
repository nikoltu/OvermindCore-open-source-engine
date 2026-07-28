#include "sc2/app/replay.hpp"

namespace sc2::app {

ReplayTrace replay(const OrderLog& log) {
    sim::World w(log.seed);
    for (const sim::Unit& u : log.initial_units) w.add_unit(u);

    ReplayTrace trace;
    trace.hashes.reserve(size_t(log.total_ticks));

    size_t oi = 0;
    for (int t = 0; t < log.total_ticks; ++t) {
        while (oi < log.orders.size() && log.orders[oi].tick == t) {
            const TimedOrder& to = log.orders[oi++];
            w.issue(to.unit_id, to.order, to.queued);
        }
        w.step();
        trace.hashes.push_back(w.state_hash());
    }
    return trace;
}

}
