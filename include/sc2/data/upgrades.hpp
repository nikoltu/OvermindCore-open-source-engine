#pragma once

#include <cstddef>
#include <cstdint>
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

    void ingest(const std::vector<uint8_t>& xml);

    std::size_t size() const { return upgrades_.size(); }
    bool contains(const std::string& id) const;
    std::vector<std::string> ids() const;

    const std::vector<UpgradeEffect>& resolve(const std::string& id) const;

private:
    struct Slot {
        std::string key;
        bool alive = true;
        bool has_ref = false;
        UpgradeEffect eff;
    };
    struct Upgrade {
        std::string id;
        std::string parent;
        std::vector<Slot> slots;
        std::unordered_map<std::string, std::size_t> pos;
        int next_auto = 0;
    };

    Upgrade& upgrade(const std::string& id);

    std::unordered_map<std::string, Upgrade> upgrades_;
    mutable std::unordered_map<std::string, std::vector<UpgradeEffect>> resolved_cache_;
};

}
