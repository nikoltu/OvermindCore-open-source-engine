#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "omc/app/ability.hpp"
#include "omc/data/catalog.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

struct MorphStepPlan {
    std::string unit;
    sim::Fixed at;
    bool type = true;
    bool vitals = false;
};

inline double morph_lock_seconds(const data::Catalog::MorphSpec& ms, bool lock_order) {
    return lock_order ? std::max(ms.morph_time, ms.order_time) : ms.morph_time;
}

inline std::vector<MorphStepPlan> morph_step_plan(const data::Catalog::MorphSpec& ms, double lock_s) {
    std::vector<MorphStepPlan> out;
    for (const data::Catalog::MorphSpec::Stage& st : ms.stages) {
        const sim::Fixed at = sim::Fixed::from_double(std::min(st.type_at, lock_s));
        const sim::Fixed vat = sim::Fixed::from_double(std::min(st.vitals_at, lock_s));
        if (vat <= at) {
            out.push_back({st.unit, at, true, true});
        } else {
            out.push_back({st.unit, at, true, false});
            out.push_back({st.unit, vat, false, true});
        }
    }
    return out;
}

inline std::optional<sim::BehaviorInstance> morph_behavior_instance(const data::Catalog& cat, const std::string& beh) {
    const auto bb = cat.behavior_buff(beh);
    if (!bb) return std::nullopt;
    sim::BehaviorInstance b;
    b.src = behavior_id_hash(beh);
    b.permanent = true;
    b.speed_mult = sim::Fixed::from_double(bb->move_speed_mult);
    b.root = bb->root;
    b.stun = bb->stun;
    b.suppress_attack = bb->suppress_attack;
    b.invulnerable = bb->invulnerable;
    b.uncommandable = bb->uncommandable;
    b.untargetable = bb->untargetable;
    b.suppress_cloak = bb->suppress_cloak;
    b.disable_abils = bb->disable_abils;
    b.disable_production = bb->disable_production;
    b.disable_spawner = bb->disable_spawner;
    b.disable_morph = bb->disable_morph;
    b.disable_transport = bb->disable_transport;
    for (const std::string& a : bb->disabled_abils) b.disabled_abils.push_back(behavior_id_hash(a));
    for (const std::string& w : bb->disabled_weapons) b.disabled_weapons.push_back(behavior_id_hash(w));
    b.benign = bb->benign;
    b.passive = bb->passive;
    return b;
}

inline sim::MorphExtras morph_extras(const data::Catalog& cat, const data::Catalog::MorphSpec& ms, bool start_delay,
                                     bool behaviors) {
    sim::MorphExtras x;
    if (start_delay) {
        x.delay_min = sim::Fixed::from_double(std::max(0.0, ms.random_delay_min));
        x.delay_max = sim::Fixed::from_double(std::max(0.0, ms.random_delay_max));
    }
    if (behaviors) {
        for (const std::string& beh : ms.behavior_on)
            if (auto b = morph_behavior_instance(cat, beh)) x.behaviors_on.push_back(std::move(*b));
        for (const std::string& beh : ms.behavior_off) x.behaviors_off.push_back(behavior_id_hash(beh));
    }
    return x;
}

}
