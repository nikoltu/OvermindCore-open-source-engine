#include "sc2/app/control_groups.hpp"

#include <algorithm>
#include <vector>

namespace sc2::app {

namespace {

void normalise(std::vector<uint32_t>& ids) {
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
}

}

void ControlGroups::set(int slot, const std::vector<uint32_t>& selection) {
    if (!valid(slot)) return;
    std::vector<uint32_t> copy = selection;
    normalise(copy);
    slots_[static_cast<std::size_t>(slot)] = std::move(copy);
}

void ControlGroups::append(int slot, const std::vector<uint32_t>& selection) {
    if (!valid(slot)) return;
    std::vector<uint32_t>& dst = slots_[static_cast<std::size_t>(slot)];
    dst.insert(dst.end(), selection.begin(), selection.end());
    normalise(dst);
}

std::vector<uint32_t> ControlGroups::recall(int slot) const {
    if (!valid(slot)) return {};
    return slots_[static_cast<std::size_t>(slot)];
}

void ControlGroups::prune(const sim::World& w) {
    const std::size_t count = w.units().size();
    for (std::vector<uint32_t>& ids : slots_) {
        ids.erase(std::remove_if(ids.begin(), ids.end(),
                                 [&](uint32_t id) {
                                     return id >= count || !w.units()[id].alive;
                                 }),
                  ids.end());
    }
}

}
