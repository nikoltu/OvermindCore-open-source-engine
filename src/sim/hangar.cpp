#include "omc/sim/world.hpp"

namespace omc::sim {

namespace {

template <class V>
auto* find_carrier(V& carriers, uint32_t id) {
    for (auto& c : carriers)
        if (c.carrier == id) return &c;
    return static_cast<decltype(&carriers[0])>(nullptr);
}

}

int32_t World::hangar_queued(uint32_t carrier) const {
    if (carriers_.empty()) return 0;
    const Carrier* c = find_carrier(carriers_, carrier);
    return c && c->queue_size > 0 ? c->queued : 0;
}

int32_t World::hangar_stock(uint32_t carrier) const {
    const Carrier* c = find_carrier(carriers_, carrier);
    return c ? c->count : 0;
}

bool World::hangar_auto_build(uint32_t carrier) const {
    const Carrier* c = find_carrier(carriers_, carrier);
    return c && c->queue_size > 0 && c->auto_build;
}

bool World::set_hangar_auto_build(uint32_t carrier, bool on) {
    Carrier* c = find_carrier(carriers_, carrier);
    if (!c || c->queue_size <= 0) return false;
    c->auto_build = on;
    return true;
}

bool World::hangar_enqueue(uint32_t carrier) {
    Carrier* c = find_carrier(carriers_, carrier);
    if (!c || c->queue_size <= 0 || carrier >= units_.size() || !units_[carrier].alive) return false;
    if (c->queued >= c->queue_size || c->count + c->queued >= c->count_max) return false;
    Player& pp = player(units_[carrier].player);
    if (pp.minerals < c->minerals || pp.gas < c->gas) return false;
    pp.minerals -= c->minerals;
    pp.gas -= c->gas;
    if (c->queued == 0) c->build_timer = c->build_time;
    ++c->queued;
    return true;
}

bool World::hangar_cancel_slot(uint32_t carrier, int slot) {
    Carrier* c = find_carrier(carriers_, carrier);
    if (!c || c->queue_size <= 0 || slot < 0 || slot >= c->queued || carrier >= units_.size()) return false;
    Player& pp = player(units_[carrier].player);
    pp.minerals += c->minerals;
    pp.gas += c->gas;
    if (slot == 0 || c->queued == 1) c->build_timer = c->build_time;
    --c->queued;
    return true;
}

void World::hangar_view(uint32_t carrier, std::vector<QueueSlotView>& out) const {
    if (carriers_.empty()) return;
    const Carrier* c = find_carrier(carriers_, carrier);
    if (!c || c->queue_size <= 0 || !c->show_status || c->queued <= 0) return;
    if (c->proto_idx < 0 || c->proto_idx >= int32_t(spawn_protos_.size())) return;
    for (int32_t i = 0; i < c->queued; ++i) {
        QueueSlotView v;
        v.catalog_id = spawn_protos_[c->proto_idx].catalog_id;
        if (i == 0 && c->build_time.raw() > 0) {
            const double f = 1.0 - c->build_timer.to_double() / c->build_time.to_double();
            v.progress = float(f < 0.0 ? 0.0 : (f > 1.0 ? 1.0 : f));
        }
        out.push_back(std::move(v));
    }
}

void World::step_hangar(Carrier& c, Fixed dt) {
    int32_t lost = 0;
    for (std::size_t i = 0; i < c.wing.size();) {
        const uint32_t id = c.wing[i];
        if (id >= units_.size() || !units_[id].alive) {
            c.wing.erase(c.wing.begin() + std::ptrdiff_t(i));
            ++lost;
        } else {
            ++i;
        }
    }
    c.count = c.count > lost ? c.count - lost : 0;
    if (c.auto_build)
        while (c.queued < c.queue_size && c.count + c.queued < c.count_max)
            if (!hangar_enqueue(c.carrier)) break;
    if (c.queued <= 0 || c.build_time.raw() <= 0) return;
    c.build_timer = c.build_timer - dt;
    if (c.build_timer.raw() > 0) return;
    ++c.count;
    --c.queued;
    c.build_timer = c.build_time;
}

}
