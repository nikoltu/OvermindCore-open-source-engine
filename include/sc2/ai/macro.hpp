#pragma once

#include <string>
#include <vector>

#include "sc2/data/catalog.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/data/production.hpp"
#include "sc2/sim/world.hpp"

namespace sc2::ai {

struct BaseHandles {
    uint32_t command_center = sim::kNoTarget;
    uint32_t producer = sim::kNoTarget;
    uint32_t mineral = sim::kNoTarget;
    std::vector<uint32_t> workers;
    sim::Fixed attack_x, attack_y;
};

struct MacroConfig {
    std::string army_unit = "Marine";
    std::string producer_ability = "BarracksTrain";
    std::string supply_unit = "SupplyDepot";
    int max_pending = 4;
    sim::Fixed build_time = sim::Fixed::from_int(2);
};

class MacroController {
public:
    MacroController(const data::Catalog& cat, const data::Production& prod,
                    int player, MacroConfig cfg = {});

    void set_base(BaseHandles base) { base_ = std::move(base); }
    void update(sim::World& w);

private:
    bool is_worker(uint32_t id) const;

    const data::Catalog& cat_;
    const data::Production& prod_;
    int player_;
    MacroConfig cfg_;
    BaseHandles base_;
    sim::Unit army_proto_, supply_proto_;
    data::UnitCost army_cost_, supply_cost_;
};

}
