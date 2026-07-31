#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "sc2/sim/world.hpp"

namespace sc2::app {

class ControlGroups {
public:
    static constexpr int kSlotCount = 10;

    void set(int slot, const std::vector<uint32_t>& selection);

    void append(int slot, const std::vector<uint32_t>& selection);

    std::vector<uint32_t> recall(int slot) const;

    void prune(const sim::World& w);

private:
    static bool valid(int slot) { return slot >= 0 && slot < kSlotCount; }

    std::array<std::vector<uint32_t>, kSlotCount> slots_;
};

}
