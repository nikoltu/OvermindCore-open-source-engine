#pragma once

#include <string>
#include <utility>
#include <vector>

#include "sc2/ai/melee_ai.hpp"
#include "sc2/app/ability.hpp"
#include "sc2/app/research.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/upgrades.hpp"
#include "sc2/sim/fixed.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::app {

class MeleeResearchDriver {
public:
    using Fixed = sim::Fixed;

    struct ProtoNames {
        std::string army;
        std::string worker;
        std::string producer;
        std::string supply;
    };

    MeleeResearchDriver(const data::Catalog& cat, const data::UpgradeCatalog& up)
        : cat_(&cat), up_(&up) {}

    void set_world(sim::World* w) { world_ = w; }

    void bind(ai::MeleeAI& ai, ProtoNames names) {
        binds_.push_back(Bind{&ai, std::move(names)});
    }

    void begin(int player, const std::string& id, Fixed delay) { rs_.begin(player, id, delay); }

    void grant(int player, const std::string& id) {
        const bool was = rs_.has(player, id);
        rs_.grant(player, id);
        if (!was) apply(player, id);
    }

    void advance(Fixed dt) {
        for (const std::pair<int, std::string>& done : rs_.advance(dt))
            apply(done.first, done.second);
    }

    const std::vector<std::string>& researched(int player) const { return rs_.researched(player); }
    bool has(int player, const std::string& id) const { return rs_.has(player, id); }
    bool pending(int player, const std::string& id) const { return rs_.pending(player, id); }
    int in_progress() const { return rs_.in_progress(); }

private:
    struct Bind {
        ai::MeleeAI* ai;
        ProtoNames names;
    };

    void apply(int player, const std::string& id) {
        const std::vector<std::string> just{id};
        for (Bind& b : binds_) {
            if (!b.ai || b.ai->player() != player) continue;
            ai::MeleeConfig& cfg = b.ai->config();
            apply_one(b.names.army, cfg.army_unit, just);
            apply_one(b.names.worker, cfg.worker_unit, just);
            apply_one(b.names.producer, cfg.producer_unit, just);
            apply_one(b.names.supply, cfg.supply_unit, just);
        }
        if (world_) apply_upgrade_to_live(*world_, *cat_, *up_, id, player, nullptr);
    }

    void apply_one(const std::string& name, ai::BuildItem& item,
                   const std::vector<std::string>& just) {
        if (name.empty()) return;
        apply_unit_upgrades(*cat_, *up_, name, just, item.proto);
    }

    const data::Catalog* cat_;
    const data::UpgradeCatalog* up_;
    ResearchState rs_;
    std::vector<Bind> binds_;
    sim::World* world_ = nullptr;
};

}
