#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/core/xml.hpp"

namespace sc2::casc {
class Storage;
}

namespace sc2::data {

class Catalog {
public:
    struct Entry {
        std::string tag;
        std::string id;
        std::string parent;
        bool is_default = false;
        std::vector<core::XmlNode> own;
        std::vector<std::pair<std::string, std::string>> attrs;

        const std::string* attr(const std::string& key) const {
            for (const auto& [k, v] : attrs)
                if (k == key) return &v;
            return nullptr;
        }
    };

    void load_melee(const casc::Storage& store);

    void load_files(const casc::Storage& store,
                    const std::vector<std::string>& basenames);

    void ingest(const std::vector<uint8_t>& xml);

    const Entry* find(const std::string& family, const std::string& id) const;

    std::vector<std::string> ids(const std::string& family) const;

    const std::vector<core::XmlNode>& resolved(const std::string& family,
                                               const std::string& id) const;

    std::optional<std::string> scalar(const std::string& family,
                                      const std::string& id,
                                      const std::string& field) const;
    std::optional<double> scalar_num(const std::string& family,
                                     const std::string& id,
                                     const std::string& field) const;

    std::vector<std::string> array_links(const std::string& family,
                                         const std::string& id,
                                         const std::string& field) const;

    static std::string family_of(const std::string& tag);

    const std::string* const_value(const std::string& id) const;
    const std::unordered_map<std::string, std::string>& consts() const {
        return consts_;
    }

    struct UnitStats {
        double life_max = 0, life_armor = 0, shields_max = 0, speed = 0;
        std::optional<double> radius;
        std::vector<int> weapon_damage;
        double weapon_period = 0, weapon_range = 0;
    };
    UnitStats unit_stats(const std::string& unit_id) const;

    std::optional<int> weapon_damage(const std::string& weapon_id) const;

private:
    using IdMap = std::unordered_map<std::string, Entry>;
    std::unordered_map<std::string, IdMap> families_;
    std::unordered_map<std::string, std::string> consts_;

    mutable std::unordered_map<std::string, std::vector<core::XmlNode>> resolved_cache_;

    std::optional<int> effect_damage(const std::string& effect_id,
                                     int depth) const;
    static std::string expand_id(std::string v, const std::string& id);
};

}
