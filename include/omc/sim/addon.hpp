#pragma once

#include <cstdint>
#include <unordered_map>

namespace omc::sim {

class AddonSystem {
public:
    enum class Kind { None, TechLab, Reactor };

    static constexpr double kOffsetX = 2.5;
    static constexpr double kOffsetY = -0.5;

    static constexpr int kMineralCost = 50;
    static constexpr int kGasCost = 25;
    static constexpr double kBuildTimeSeconds = 30.0;

    bool attach(std::uint32_t building, Kind kind) {
        if (kind == Kind::None) return false;
        if (addons_.find(building) != addons_.end()) return false;
        addons_[building] = kind;
        return true;
    }

    Kind kind(std::uint32_t building) const {
        auto it = addons_.find(building);
        return it == addons_.end() ? Kind::None : it->second;
    }

    bool has_addon(std::uint32_t building) const { return kind(building) != Kind::None; }

    int parallel_slots(std::uint32_t building) const {
        return kind(building) == Kind::Reactor ? 2 : 1;
    }

    bool tech_gate_met(std::uint32_t building) const {
        return kind(building) == Kind::TechLab;
    }

private:
    std::unordered_map<std::uint32_t, Kind> addons_;
};

}
