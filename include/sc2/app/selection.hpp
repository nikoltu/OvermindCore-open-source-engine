#pragma once

#include <algorithm>
#include <cstdint>
#include <vector>

namespace sc2::app {

constexpr uint32_t kNoUnit = 0xFFFFFFFFu;

struct Subgroup {
    uint32_t type = 0;
    int priority = 0;
    std::vector<uint32_t> units;
    uint32_t lead = kNoUnit;
};

struct SelectionGroups {
    std::vector<Subgroup> subgroups;
    std::size_t active = 0;

    static SelectionGroups build(const std::vector<uint32_t>& units,
                                 const std::vector<uint32_t>& type,
                                 const std::vector<int>& priority) {
        std::vector<Subgroup> by_type;
        for (std::size_t i = 0; i < units.size(); ++i) {
            uint32_t t = i < type.size() ? type[i] : 0;
            int pr = i < priority.size() ? priority[i] : 0;
            Subgroup* s = nullptr;
            for (Subgroup& e : by_type)
                if (e.type == t) {
                    s = &e;
                    break;
                }
            if (!s) {
                by_type.push_back(Subgroup{t, pr, {}, kNoUnit});
                s = &by_type.back();
            }
            s->units.push_back(units[i]);
            if (pr > s->priority) s->priority = pr;
        }
        for (Subgroup& s : by_type) {
            std::sort(s.units.begin(), s.units.end());
            s.lead = s.units.empty() ? kNoUnit : s.units.front();
        }
        std::sort(by_type.begin(), by_type.end(), [](const Subgroup& a, const Subgroup& b) {
            if (a.priority != b.priority) return a.priority > b.priority;
            return a.type < b.type;
        });
        SelectionGroups g;
        g.subgroups = std::move(by_type);
        return g;
    }

    uint32_t active_lead() const {
        if (subgroups.empty()) return kNoUnit;
        std::size_t a = active < subgroups.size() ? active : 0;
        return subgroups[a].lead;
    }
    void cycle() {
        if (!subgroups.empty()) active = (active + 1) % subgroups.size();
    }
    std::size_t count() const { return subgroups.size(); }
    std::size_t total_units() const {
        std::size_t n = 0;
        for (const Subgroup& s : subgroups) n += s.units.size();
        return n;
    }
};

}
