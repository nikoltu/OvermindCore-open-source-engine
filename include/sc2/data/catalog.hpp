#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
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

    std::optional<std::string> weapon_damage_effect_id(const std::string& weapon_id) const {
        std::string effect = scalar("CWeapon", weapon_id, "Effect").value_or("");
        if (effect.empty()) effect = weapon_id;
        return damage_effect_id(effect, 0);
    }

    std::optional<int> effect_damage(const std::string& effect_id) const {
        return effect_damage(effect_id, 0);
    }

    std::optional<double> effect_damage_f(const std::string& effect_id) const {
        if (auto id = damage_effect_id(effect_id, 0)) return scalar_num("CEffect", *id, "Amount");
        return std::nullopt;
    }

    std::optional<double> effect_life_change(const std::string& effect_id) const {
        return effect_life_change(effect_id, 0);
    }

    std::optional<double> effect_energy_fraction(const std::string& effect_id) const {
        return effect_energy_fraction(effect_id, 0);
    }

    struct BehaviorBuff {
        double duration = 0;
        double move_speed_mult = 1.0;
        double attack_speed_mult = 1.0;
        bool root = false;
        bool stun = false;
        double dmg_per_tick = 0;
        double dmg_period = 0;
    };
    std::optional<BehaviorBuff> behavior_buff(const std::string& behavior_id) const;

    std::optional<std::string> effect_apply_behavior(const std::string& effect_id) const {
        return effect_apply_behavior(effect_id, 0);
    }

    struct CreateUnitSpec {
        std::string unit;
        int count = 1;
        std::string spawn_effect;
    };

    std::optional<CreateUnitSpec> effect_create_unit(const std::string& effect_id) const {
        return effect_create_unit(effect_id, 0);
    }

    struct PersistentSpec {
        double amount = 0;
        double radius = 0;
        double period = 0;
        int count = 0;
    };
    std::optional<PersistentSpec> effect_persistent(const std::string& effect_id) const {
        return effect_persistent(effect_id, 0);
    }

    struct MorphSpec {
        std::string target;
        double morph_time = 0;
        std::vector<std::string> chain;
        std::string finish_effect;
    };
    std::optional<MorphSpec> morph_spec(const std::string& abil_id) const;

    struct SpawnBehavior {
        std::string unit;
        int max_count = 0;
        double period = 0;
        int hard_cap = 0;
    };
    std::optional<SpawnBehavior> spawn_behavior(const std::string& behavior_id) const;

    std::optional<double> effect_vital_fraction(const std::string& effect_id,
                                                const std::string& vital) const;

    std::optional<double> repair_time(const std::string& unit_id) const {
        if (auto v = scalar_num("CUnit", unit_id, "RepairTime")) return *v;
        return std::nullopt;
    }

    std::optional<int> cargo_size(const std::string& unit_id) const {
        if (auto v = scalar_num("CUnit", unit_id, "CargoSize")) return int(*v);
        return std::nullopt;
    }

    double detector_range(const std::string& unit_id) const {
        double best = 0;
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            for (const core::XmlNode& n : resolved("CBehavior", beh))
                if (n.name == "Modification")
                    if (const std::string* v = n.attr("Detect")) {
                        const double d = std::atof(v->c_str());
                        if (d > best) best = d;
                    }
        }
        return best;
    }

    double radar_range(const std::string& unit_id) const {
        double best = 0;
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            for (const core::XmlNode& n : resolved("CBehavior", beh))
                if (n.name == "Modification")
                    if (const std::string* v = n.attr("Radar")) {
                        const double d = std::atof(v->c_str());
                        if (d > best) best = d;
                    }
        }
        return best;
    }

    struct ShieldRegen { double rate = 0, delay = 0; };
    ShieldRegen shield_regen(const std::string& unit_id) const {
        ShieldRegen s;
        s.rate = scalar_num("CUnit", unit_id, "ShieldRegenRate").value_or(0.0);
        s.delay = scalar_num("CUnit", unit_id, "ShieldRegenDelay").value_or(0.0);
        return s;
    }

    std::optional<double> cloak_drain(const std::string& behavior_id) const {
        for (const core::XmlNode& n : resolved("CBehavior", behavior_id)) {
            if (n.name != "Modification") continue;
            for (const core::XmlNode& v : n.children)
                if (v.name == "VitalRegenArray" && v.attr_or("index", "") == "Energy")
                    if (const std::string* val = v.attr("value")) {
                        const double d = std::atof(val->c_str());
                        if (d < 0) return -d;
                    }
        }
        return std::nullopt;
    }

    std::vector<std::pair<double, double>> weapon_splash(const std::string& weapon_id) const;

    const std::vector<core::XmlNode>& singleton_fields(const std::string& family) const;

private:
    using IdMap = std::unordered_map<std::string, Entry>;
    std::unordered_map<std::string, IdMap> families_;
    std::unordered_map<std::string, std::string> consts_;
    std::unordered_map<std::string, std::vector<core::XmlNode>> singletons_;

    mutable std::unordered_map<std::string, std::vector<core::XmlNode>> resolved_cache_;

    std::optional<int> effect_damage(const std::string& effect_id,
                                     int depth) const;
    std::optional<double> effect_life_change(const std::string& effect_id, int depth) const;
    std::optional<double> effect_energy_fraction(const std::string& effect_id, int depth) const;
    std::optional<std::string> effect_apply_behavior(const std::string& effect_id, int depth) const;
    std::optional<CreateUnitSpec> effect_create_unit(const std::string& effect_id, int depth) const;
    std::optional<PersistentSpec> effect_persistent(const std::string& effect_id, int depth) const;
    std::optional<std::pair<double, double>> area_damage_radius(const std::string& effect_id,
                                                               int depth) const;
    std::optional<std::string> damage_effect_id(const std::string& effect_id,
                                                int depth) const;
    std::vector<std::string> switch_effects(const std::string& effect_id) const;
    static std::string expand_id(std::string v, const std::string& id);
};

}
