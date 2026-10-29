#pragma once

#include <string>
#include <utility>
#include <vector>

#include "omc/ai/melee_ai.hpp"
#include "omc/app/ability.hpp"
#include "omc/data/catalog.hpp"
#include "omc/data/upgrades.hpp"
#include "omc/sim/fixed.hpp"
#include "omc/sim/world.hpp"

namespace omc::app {

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

    static const std::vector<std::string>& none() {
        static const std::vector<std::string> kNone;
        return kNone;
    }

    void begin(int player, const std::string& id, Fixed delay, std::uint32_t producer = sim::kNoTarget,
               int minerals = 0, int gas = 0) {
        if (world_) world_->begin_research(player, id, delay, producer, minerals, gas);
    }

    void grant(int player, const std::string& id) {
        if (!world_) return;
        const bool was = world_->has_upgrade(player, id);
        world_->grant_upgrade(player, id);
        if (!was) apply(player, id);
    }

    void apply_completed(bool counted = false) {
        if (!world_) return;
        just_completed_.clear();
        for (const std::pair<std::int32_t, std::string>& done : world_->take_completed_research()) {
            apply(int(done.first), done.second, counted);
            just_completed_.push_back(done);
        }
        if (counted) fold_lagging_upgrades(*world_, *cat_, *up_);
    }
    const std::vector<std::pair<std::int32_t, std::string>>& just_completed() const { return just_completed_; }

    const std::vector<std::string>& researched(int player) const {
        return world_ ? world_->researched(player) : none();
    }
    bool has(int player, const std::string& id) const {
        return world_ && world_->has_upgrade(player, id);
    }
    bool pending(int player, const std::string& id) const {
        return world_ && world_->research_pending(player, id);
    }
    int in_progress() const { return world_ ? world_->research_in_progress() : 0; }

    const data::UpgradeCatalog* upgrades() const { return up_; }

private:
    std::vector<std::pair<std::int32_t, std::string>> just_completed_;
    struct Bind {
        ai::MeleeAI* ai;
        ProtoNames names;
    };

    void apply(int player, const std::string& id, bool counted = false) {
        const std::vector<std::string> just{id};
        const std::uint32_t count = world_ ? std::uint32_t(world_->researched(player).size()) : 0u;
        for (Bind& b : binds_) {
            if (!b.ai || b.ai->player() != player) continue;
            ai::MeleeConfig& cfg = b.ai->config();
            apply_one(b.names.army, cfg.army_unit, just, counted, count);
            apply_one(b.names.worker, cfg.worker_unit, just, counted, count);
            apply_one(b.names.producer, cfg.producer_unit, just, counted, count);
            apply_one(b.names.supply, cfg.supply_unit, just, counted, count);
        }
        if (world_) apply_upgrade_to_live(*world_, *cat_, *up_, id, player, nullptr, !counted);
    }

    void apply_one(const std::string& name, ai::BuildItem& item,
                   const std::vector<std::string>& just, bool counted, std::uint32_t count) {
        if (name.empty()) return;
        apply_unit_upgrades(*cat_, *up_, name, just, item.proto);
        if (counted) item.proto.upgrades_folded = count;
    }

    const data::Catalog* cat_;
    const data::UpgradeCatalog* up_;
    std::vector<Bind> binds_;
    sim::World* world_ = nullptr;
};

}
