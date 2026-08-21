#include "sc2/app/replay.hpp"

#include <cstring>
#include <fstream>
#include <iterator>

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
            if (to.is_stop)
                w.stop(to.unit_id);
            else
                w.issue(to.unit_id, to.order, to.queued);
        }
        w.step();
        trace.hashes.push_back(w.state_hash());
    }
    return trace;
}

namespace {

void put_u8(std::vector<uint8_t>& o, uint8_t v) { o.push_back(v); }
void put_u32(std::vector<uint8_t>& o, uint32_t v) {
    o.push_back(uint8_t(v));
    o.push_back(uint8_t(v >> 8));
    o.push_back(uint8_t(v >> 16));
    o.push_back(uint8_t(v >> 24));
}
void put_i32(std::vector<uint8_t>& o, int32_t v) { put_u32(o, uint32_t(v)); }
void put_u64(std::vector<uint8_t>& o, uint64_t v) {
    for (int i = 0; i < 8; ++i) o.push_back(uint8_t(v >> (8 * i)));
}
void put_fx(std::vector<uint8_t>& o, sim::Fixed v) { put_i32(o, v.raw()); }
void put_str(std::vector<uint8_t>& o, const std::string& s) {
    put_u32(o, uint32_t(s.size()));
    o.insert(o.end(), s.begin(), s.end());
}

struct Reader {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    bool avail(size_t n) {
        if (!ok || size_t(end - p) < n) { ok = false; return false; }
        return true;
    }
    uint8_t u8() { return avail(1) ? *p++ : 0; }
    uint32_t u32() {
        if (!avail(4)) return 0;
        uint32_t v = uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
        p += 4;
        return v;
    }
    int32_t i32() { return int32_t(u32()); }
    uint64_t u64() {
        uint64_t v = 0;
        if (!avail(8)) return 0;
        for (int i = 0; i < 8; ++i) v |= uint64_t(p[i]) << (8 * i);
        p += 8;
        return v;
    }
    sim::Fixed fx() { return sim::Fixed::from_raw(i32()); }
    std::string str() {
        uint32_t n = u32();
        if (!avail(n)) return {};
        std::string s(reinterpret_cast<const char*>(p), n);
        p += n;
        return s;
    }
};

void put_order(std::vector<uint8_t>& o, const sim::Order& r) {
    put_u32(o, uint32_t(r.type));
    put_fx(o, r.tx);
    put_fx(o, r.ty);
    put_u32(o, r.target);
    put_u32(o, r.ability);
    put_fx(o, r.px);
    put_fx(o, r.py);
}
sim::Order get_order(Reader& r) {
    sim::Order o;
    o.type = sim::OrderType(r.u32());
    o.tx = r.fx();
    o.ty = r.fx();
    o.target = r.u32();
    o.ability = r.u32();
    o.px = r.fx();
    o.py = r.fx();
    return o;
}

void put_unit(std::vector<uint8_t>& o, const sim::Unit& u) {
    put_u32(o, u.id);
    put_i32(o, u.player);
    put_i32(o, u.type_id);
    put_str(o, u.catalog_id);
    put_fx(o, u.x);
    put_fx(o, u.y);
    put_fx(o, u.hp);
    put_fx(o, u.hp_max);
    put_fx(o, u.shields);
    put_fx(o, u.shields_max);
    put_fx(o, u.armor);
    put_fx(o, u.shield_armor);
    put_fx(o, u.shield_regen_rate);
    put_fx(o, u.shield_regen_delay);
    put_fx(o, u.wpn_damage);
    put_fx(o, u.wpn_period);
    put_fx(o, u.wpn_range);
    put_fx(o, u.wpn_min_range);
    put_fx(o, u.wpn_damage_point);
    put_fx(o, u.wpn_projectile_speed);
    put_fx(o, u.wpn_backswing);
    put_i32(o, u.wpn_hits);
    put_u32(o, u.wpn_bonus_attr);
    put_fx(o, u.wpn_bonus_amount);
    put_u32(o, u.wpn_target_filter);
    put_fx(o, u.wpn2_damage);
    put_fx(o, u.wpn2_period);
    put_fx(o, u.wpn2_range);
    put_u32(o, u.wpn2_target_filter);
    put_u32(o, u.wpn2_bonus_attr);
    put_fx(o, u.wpn2_bonus_amount);
    put_fx(o, u.speed);
    put_fx(o, u.radius);
    put_fx(o, u.sep_radius);
    put_fx(o, u.inner_radius);
    put_fx(o, u.acquire_range);
    put_fx(o, u.sight);
    put_fx(o, u.atk_speed_mult);
    put_fx(o, u.damage_reduction);
    put_fx(o, u.cooldown);
    put_u32(o, u.target);
    put_u32(o, u.attributes);
    put_i32(o, u.target_priority);
    put_fx(o, u.energy);
    put_fx(o, u.energy_max);
    put_fx(o, u.energy_regen);
    put_fx(o, u.supply_provides);
    put_fx(o, u.supply_uses);
    put_i32(o, u.resource_left);
    put_i32(o, u.miners_max);
    put_i32(o, u.yield_amount);
    put_i32(o, u.carry_amount);
    put_fx(o, u.mine_time);
    put_u8(o, u.alive ? 1 : 0);
    put_u8(o, u.flying ? 1 : 0);
    put_u8(o, u.is_worker ? 1 : 0);
    put_u8(o, u.hold_fire ? 1 : 0);
    put_u8(o, u.detector ? 1 : 0);
    put_u8(o, u.cloaked ? 1 : 0);
    put_u8(o, u.burrowed ? 1 : 0);
    put_u8(o, u.is_gas ? 1 : 0);
    put_u8(o, u.gathers_gas ? 1 : 0);
    put_u32(o, uint32_t(u.orders.size()));
    for (const sim::Order& r : u.orders) put_order(o, r);
}
sim::Unit get_unit(Reader& r) {
    sim::Unit u;
    u.id = r.u32();
    u.player = r.i32();
    u.type_id = r.i32();
    u.catalog_id = r.str();
    u.x = r.fx();
    u.y = r.fx();
    u.hp = r.fx();
    u.hp_max = r.fx();
    u.shields = r.fx();
    u.shields_max = r.fx();
    u.armor = r.fx();
    u.shield_armor = r.fx();
    u.shield_regen_rate = r.fx();
    u.shield_regen_delay = r.fx();
    u.wpn_damage = r.fx();
    u.wpn_period = r.fx();
    u.wpn_range = r.fx();
    u.wpn_min_range = r.fx();
    u.wpn_damage_point = r.fx();
    u.wpn_projectile_speed = r.fx();
    u.wpn_backswing = r.fx();
    u.wpn_hits = r.i32();
    u.wpn_bonus_attr = r.u32();
    u.wpn_bonus_amount = r.fx();
    u.wpn_target_filter = r.u32();
    u.wpn2_damage = r.fx();
    u.wpn2_period = r.fx();
    u.wpn2_range = r.fx();
    u.wpn2_target_filter = r.u32();
    u.wpn2_bonus_attr = r.u32();
    u.wpn2_bonus_amount = r.fx();
    u.speed = r.fx();
    u.radius = r.fx();
    u.sep_radius = r.fx();
    u.inner_radius = r.fx();
    u.acquire_range = r.fx();
    u.sight = r.fx();
    u.atk_speed_mult = r.fx();
    u.damage_reduction = r.fx();
    u.cooldown = r.fx();
    u.target = r.u32();
    u.attributes = r.u32();
    u.target_priority = r.i32();
    u.energy = r.fx();
    u.energy_max = r.fx();
    u.energy_regen = r.fx();
    u.supply_provides = r.fx();
    u.supply_uses = r.fx();
    u.resource_left = r.i32();
    u.miners_max = r.i32();
    u.yield_amount = r.i32();
    u.carry_amount = r.i32();
    u.mine_time = r.fx();
    u.alive = r.u8() != 0;
    u.flying = r.u8() != 0;
    u.is_worker = r.u8() != 0;
    u.hold_fire = r.u8() != 0;
    u.detector = r.u8() != 0;
    u.cloaked = r.u8() != 0;
    u.burrowed = r.u8() != 0;
    u.is_gas = r.u8() != 0;
    u.gathers_gas = r.u8() != 0;
    uint32_t no = r.u32();
    for (uint32_t i = 0; i < no && r.ok; ++i) u.orders.push_back(get_order(r));
    return u;
}

}

void write_order_log(const OrderLog& log, std::vector<uint8_t>& out) {
    out.insert(out.end(), kOrderLogMagic, kOrderLogMagic + 8);
    put_u32(out, kOrderLogVersion);
    put_u64(out, log.seed);
    put_str(out, log.map_id);
    put_u32(out, uint32_t(log.player_races.size()));
    for (int32_t r : log.player_races) put_i32(out, r);
    put_i32(out, log.total_ticks);
    put_u32(out, uint32_t(log.initial_units.size()));
    for (const sim::Unit& u : log.initial_units) put_unit(out, u);
    put_u32(out, uint32_t(log.orders.size()));
    for (const TimedOrder& t : log.orders) {
        put_i32(out, t.tick);
        put_u32(out, t.unit_id);
        put_u8(out, t.queued ? 1 : 0);
        put_u8(out, t.is_stop ? 1 : 0);
        put_order(out, t.order);
    }
    return;
}

bool read_order_log(const uint8_t* data, size_t len, OrderLog& out) {
    if (len < 12 || std::memcmp(data, kOrderLogMagic, 8) != 0) return false;
    Reader r{data + 8, data + len};
    uint32_t version = r.u32();
    if (version != 1 && version != kOrderLogVersion) return false;
    OrderLog log;
    log.seed = r.u64();
    if (version >= 2) {
        log.map_id = r.str();
        uint32_t nr = r.u32();
        for (uint32_t i = 0; i < nr && r.ok; ++i) log.player_races.push_back(r.i32());
    }
    log.total_ticks = r.i32();
    uint32_t nu = r.u32();
    for (uint32_t i = 0; i < nu && r.ok; ++i) log.initial_units.push_back(get_unit(r));
    uint32_t no = r.u32();
    for (uint32_t i = 0; i < no && r.ok; ++i) {
        TimedOrder t;
        t.tick = r.i32();
        t.unit_id = r.u32();
        t.queued = r.u8() != 0;
        t.is_stop = r.u8() != 0;
        t.order = get_order(r);
        log.orders.push_back(t);
    }
    if (!r.ok) return false;
    out = std::move(log);
    return true;
}

bool write_order_log_file(const OrderLog& log, const std::string& path) {
    std::vector<uint8_t> buf;
    write_order_log(log, buf);
    std::ofstream os(path, std::ios::binary);
    if (!os) return false;
    os.write(reinterpret_cast<const char*>(buf.data()), std::streamsize(buf.size()));
    return bool(os);
}

bool read_order_log_file(const std::string& path, OrderLog& out) {
    std::ifstream is(path, std::ios::binary);
    if (!is) return false;
    std::vector<uint8_t> buf((std::istreambuf_iterator<char>(is)), std::istreambuf_iterator<char>());
    return read_order_log(buf.data(), buf.size(), out);
}

}
