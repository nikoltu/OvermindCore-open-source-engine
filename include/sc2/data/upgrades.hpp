#pragma once

#include <cstddef>
#include <cstdint>
#include "sc2/core/xml.hpp"
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::casc {
class Storage;
}

namespace sc2::data {

struct UpgradeEffect {
    enum class Op { Add, Subtract, Multiply, Set };

    std::string catalog;
    std::string entry;
    std::string field;
    std::string field_index;
    std::string reference;
    std::string value;
    Op op = Op::Add;

    double numeric_value() const;
    bool is_numeric() const;
    double delta() const;

    double apply(double cur) const;
};

class UpgradeCatalog {
public:
    void load_melee(const casc::Storage& store);

    void load_files(const casc::Storage& store,
                    const std::vector<std::string>& basenames);

    const std::vector<std::string>& load_skips() const { return load_skips_; }

    void ingest(const std::vector<uint8_t>& xml);

    std::size_t size() const { return upgrades_.size(); }
    bool contains(const std::string& id) const;
    std::vector<std::string> ids() const;

    const std::vector<UpgradeEffect>& resolve(const std::string& id) const;

private:
    std::vector<std::string> load_skips_;

    struct Write {
        core::XmlNode node;
        std::uint16_t decl = 0;
    };
    struct Upgrade {
        std::string id;
        std::string parent;
        std::vector<Write> writes;
        std::uint16_t decl_seen = 0;
    };
    struct RSlot {
        bool has_ref = false;
        bool dead = false;
        UpgradeEffect eff;
    };
    std::vector<RSlot> resolve_slots(const std::string& id, int depth) const;

    Upgrade& upgrade(const std::string& id);

    std::unordered_map<std::string, Upgrade> upgrades_;
    std::vector<std::string> order_;
    mutable std::unordered_map<std::string, std::vector<UpgradeEffect>> resolved_cache_;
};

inline double fold_upgrade_field(const UpgradeCatalog& up,
                                 const std::vector<std::string>& researched, const char* catalog,
                                 const std::string& entry, const std::string& field, double base) {
    double v = base;
    for (const std::string& upg : researched) {
        if (!up.contains(upg)) continue;
        for (const UpgradeEffect& e : up.resolve(upg))
            if (e.catalog == catalog && e.entry == entry && e.field == field) v = e.apply(v);
    }
    return v;
}

}
