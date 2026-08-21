#pragma once

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sc2/core/xml.hpp"
#include "sc2/data/strings.hpp"

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
        std::vector<std::uint16_t> own_decl;
        std::uint16_t decl_seen = 0;
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

    void load_strings(const casc::Storage& store);

    void load_root_files(const casc::Storage& store,
                         const std::vector<std::string>& roots,
                         const std::vector<std::string>& basenames);

    void load_campaign(const casc::Storage& store,
                       const std::vector<std::string>& campaign_roots,
                       const std::string& mission_local_root = "");

    void load_strings_text(const std::string& text) { strings_.load_text(text); }
    const StringTable& strings() const { return strings_; }

    std::optional<std::string> button_display_name(const std::string& button_id) const;
    std::optional<std::string> unit_display_name(const std::string& unit_id) const;
    std::optional<std::string> ability_display_name(const std::string& ability_id) const;

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

    std::vector<std::string> array_links_legacy(const std::string& family,
                                                const std::string& id,
                                                const std::string& field) const;

    int own_declaration_count(const std::string& family, const std::string& id) const {
        const Entry* e = find(family, id);
        return e ? int(e->decl_seen) : 0;
    }

    std::vector<std::string> array_links_twomode(const std::string& family,
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

    std::optional<double> weapon_radius_split_no_splash(const std::string& weapon_id) const;

    struct PointDefense {
        double radius = 0;
        int max = 0;
    };
    std::optional<PointDefense> point_defense_of(const std::string& unit_id) const;

    std::optional<int> weapon_hit_count(const std::string& weapon_id) const;

    std::string ability_command_requirement(const std::string& abil_id,
                                            const std::string& cmd_index) const;

    std::vector<std::pair<std::string, std::string>>
    unit_ability_commands(const std::string& unit_id) const;

    std::optional<std::string> button_icon(const std::string& button_id) const;

    struct AbilityCardButton {
        std::string ability;
        std::string command;
        std::string face;
        int row = -1;
        int col = -1;
    };
    std::vector<AbilityCardButton> unit_ability_card_buttons(const std::string& unit_id) const;

    struct AbilityAutocast {
        bool offered = false;
        bool on_by_default = false;
        double range = 0.0;
        bool targets_allies = false;
    };
    AbilityAutocast ability_autocast(const std::string& abil_id) const;

    std::map<std::string, int> weapon_attribute_bonus(const std::string& weapon_id) const;

    std::string weapon_effect_id(const std::string& weapon_id) const {
        std::string effect = scalar("CWeapon", weapon_id, "Effect").value_or("");
        if (effect.empty()) effect = weapon_id;
        return effect;
    }

    std::optional<std::string> weapon_damage_effect_id(const std::string& weapon_id) const {
        if (auto d = damage_effect_id(weapon_effect_id(weapon_id), 0)) return d;
        if (auto disp = scalar("CWeapon", weapon_id, "DisplayEffect"); disp && !disp->empty())
            return damage_effect_id(*disp, 0);
        return std::nullopt;
    }

    double weapon_armor_reduction(const std::string& weapon_id) const {
        if (auto id = weapon_damage_effect_id(weapon_id))
            if (auto v = scalar_num("CEffect", *id, "ArmorReduction")) return *v;
        return 1.0;
    }

    double effect_armor_reduction(const std::string& effect_id) const {
        if (auto id = damage_effect_id(effect_id, 0))
            if (auto v = scalar_num("CEffect", *id, "ArmorReduction")) return *v;
        return 1.0;
    }

    std::optional<int> effect_damage(const std::string& effect_id) const {
        return effect_damage(effect_id, 0);
    }

    std::optional<double> effect_damage_f(const std::string& effect_id) const {
        if (auto id = damage_effect_id(effect_id, 0)) return scalar_num("CEffect", *id, "Amount");
        return std::nullopt;
    }

    std::string effect_subclass(const std::string& effect_id) const {
        const Entry* e = find("CEffect", effect_id);
        if (!e) return {};
        const std::string& t = e->tag;
        return t.rfind("CEffect", 0) == 0 ? t.substr(7) : t;
    }

    std::vector<std::string> effect_set_children(const std::string& effect_id) const {
        return array_links("CEffect", effect_id, "EffectArray");
    }

    struct EnumAreaSpec {
        std::string child;
        double radius = 0;
    };
    std::optional<EnumAreaSpec> effect_enum_area(const std::string& effect_id) const {
        std::map<long, EnumAreaSpec> slots;
        long counter = 0;
        for (const core::XmlNode& n : resolved("CEffect", effect_id)) {
            if (n.name != "AreaArray") continue;
            long idx = counter++;
            if (const std::string* ix = n.attr("index"); ix && !ix->empty()) {
                char* end = nullptr;
                long v = std::strtol(ix->c_str(), &end, 10);
                if (end != ix->c_str()) {
                    idx = v;
                    counter = v + 1;
                }
            }
            EnumAreaSpec& s = slots[idx];
            if (const std::string* rm = n.attr("removed"); rm && !rm->empty() && *rm != "0") {
                s = EnumAreaSpec{};
                continue;
            }
            if (const std::string* ef = n.attr("Effect"); ef && !ef->empty()) s.child = *ef;
            if (const std::string* r = n.attr("Radius"); r && !r->empty())
                s.radius = std::strtod(r->c_str(), nullptr);
        }
        if (auto it = slots.find(0); it != slots.end() && !it->second.child.empty())
            return it->second;
        return std::nullopt;
    }

    struct BounceSpec {
        double range = 0;
        std::vector<double> amounts;
    };
    std::optional<BounceSpec> weapon_bounce(const std::string& weapon_id) const {
        const std::string root = weapon_effect_id(weapon_id);
        if (effect_subclass(root) != "LaunchMissile") return std::nullopt;
        BounceSpec spec;
        std::string cur = scalar("CEffect", root, "ImpactEffect").value_or("");
        bool primary = true;
        for (int hop = 0; hop < 8 && !cur.empty(); ++hop) {
            std::string dmg_effect, enum_effect;
            const std::string sc = effect_subclass(cur);
            if (sc == "Set") {
                for (const std::string& c : effect_set_children(cur)) {
                    const std::string cs = effect_subclass(c);
                    if (cs == "Damage" && dmg_effect.empty()) dmg_effect = c;
                    else if (cs == "EnumArea" && enum_effect.empty()) enum_effect = c;
                }
            } else if (sc == "Damage") {
                dmg_effect = cur;
            } else {
                break;
            }
            if (dmg_effect.empty()) break;
            const double amt = effect_damage_f(dmg_effect).value_or(0.0);
            if (!primary) spec.amounts.push_back(amt);
            primary = false;
            if (enum_effect.empty()) break;
            auto ea = effect_enum_area(enum_effect);
            if (!ea) break;
            if (effect_subclass(ea->child) != "LaunchMissile") break;
            if (spec.range == 0) spec.range = ea->radius;
            cur = scalar("CEffect", ea->child, "ImpactEffect").value_or("");
        }
        if (spec.amounts.empty() || spec.range == 0) return std::nullopt;
        return spec;
    }

    std::uint8_t effect_search_affiliation(const std::string& effect_id) const {
        auto sf = scalar("CEffect", effect_id, "SearchFilters");
        if (!sf || sf->empty()) return 4 | 8 | 16;
        auto bit = [](const std::string& tok) -> std::uint8_t {
            if (tok == "Self") return 1;
            if (tok == "Player") return 2;
            if (tok == "Ally") return 4;
            if (tok == "Enemy") return 8;
            if (tok == "Neutral") return 16;
            return 0;
        };
        auto split = [&](const std::string& part, std::uint8_t& rel_bits) {
            size_t i = 0;
            while (i < part.size()) {
                size_t j = part.find(',', i);
                if (j == std::string::npos) j = part.size();
                rel_bits |= bit(part.substr(i, j - i));
                i = j + 1;
            }
        };
        const size_t semi = sf->find(';');
        const std::string req = semi == std::string::npos ? *sf : sf->substr(0, semi);
        const std::string exc = semi == std::string::npos ? std::string{} : sf->substr(semi + 1);
        std::uint8_t req_bits = 0, exc_bits = 0;
        split(req, req_bits);
        split(exc, exc_bits);
        std::uint8_t allowed = std::uint8_t((1 | 2 | 4 | 8 | 16) & ~exc_bits);
        if (req_bits) allowed = std::uint8_t(allowed & req_bits);
        return allowed;
    }

    bool effect_search_excludes_structure(const std::string& effect_id) const {
        auto sf = scalar("CEffect", effect_id, "SearchFilters");
        if (!sf || sf->empty()) return false;
        const std::size_t semi = sf->find(';');
        if (semi == std::string::npos) return false;
        const std::string exc = sf->substr(semi + 1);
        std::size_t i = 0;
        while (i < exc.size()) {
            std::size_t j = exc.find(',', i);
            if (j == std::string::npos) j = exc.size();
            if (exc.compare(i, j - i, "Structure") == 0) return true;
            i = j + 1;
        }
        return false;
    }

    std::optional<double> effect_life_change(const std::string& effect_id) const {
        return effect_life_change(effect_id, 0);
    }

    std::optional<double> effect_energy_fraction(const std::string& effect_id) const {
        return effect_energy_fraction(effect_id, 0);
    }

    std::optional<double> effect_flat_energy_change(const std::string& effect_id) const;

    struct BehaviorBuff {
        double duration = 0;
        double move_speed_mult = 1.0;
        double attack_speed_mult = 1.0;
        bool root = false;
        bool stun = false;
        double dmg_per_tick = 0;
        double dmg_period = 0;
        bool control_transfer = false;
    };
    std::optional<BehaviorBuff> behavior_buff(const std::string& behavior_id) const;

    std::optional<std::string> effect_control_transfer_behavior(const std::string& effect_id) const;

    struct DamageResponse {
        double fraction = 1.0;
        double clamp_max = 0.0;
        double modify_limit = 0.0;
        double cooldown = 0.0;
        double clamp_min = 0.0;
        double minimum = 0.0;
        double modify_amount = 0.0;
        bool excludes_spell = false;
        bool present = false;
    };
    DamageResponse behavior_damage_response(const std::string& behavior_id) const;

    std::optional<std::string> effect_apply_behavior(const std::string& effect_id) const {
        return effect_apply_behavior(effect_id, 0);
    }

    std::optional<std::string> effect_movement_slow_behavior(const std::string& effect_id) const;

    struct CreateUnitSpec {
        std::string unit;
        int count = 1;
        std::string spawn_effect;
    };

    std::optional<CreateUnitSpec> effect_create_unit(const std::string& effect_id) const {
        return effect_create_unit(effect_id, 0);
    }

    struct AttackSpawnSpec {
        std::string unit;
        int count = 0;
        double life = 0;
    };
    std::optional<AttackSpawnSpec> attack_spawn_of(const std::string& unit_id) const;

    struct InterceptorMagSpec {
        std::string unit;
        int count = 0;
        int count_max = 0;
        double leash = 0;
        double build_time = 0;
    };
    std::optional<InterceptorMagSpec> interceptor_magazine_of(const std::string& unit_id) const;

    struct NovaLaunchSpec {
        std::string ability;
        std::string unit;
        double speed = 0;
        double radius = 0;
        double damage = 0;
        double shield_bonus = 0;
        double duration = 0;
    };
    std::optional<NovaLaunchSpec> nova_launch_of(const std::string& unit_id) const;

    struct PersistentSpec {
        double amount = 0;
        double radius = 0;
        double period = 0;
        int count = 0;
        int line_count = 0;
        double line_first = 0;
        double line_last = 0;
        double line_step = 0;
        bool line_from_source = false;
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

    struct MergeSpec {
        std::string target;
        double time = 0;
    };
    std::optional<MergeSpec> merge_spec(const std::string& abil_id) const;

    struct BuildSpec {
        std::string unit;
        double energy = 0;
    };
    std::optional<BuildSpec> build_spec(const std::string& abil_id) const;

    int footprint_place_cells(const std::string& footprint_id) const;

    struct SpawnBehavior {
        std::string unit;
        int max_count = 0;
        double period = 0;
        int hard_cap = 0;
    };
    std::optional<SpawnBehavior> spawn_behavior(const std::string& behavior_id) const;

    std::optional<SpawnBehavior> unit_spawn_behavior(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorSpawn") continue;
            if (auto sb = spawn_behavior(beh)) return sb;
        }
        return std::nullopt;
    }

    std::optional<double> effect_vital_fraction(const std::string& effect_id,
                                                const std::string& vital) const;

    std::optional<double> repair_time(const std::string& unit_id) const {
        if (auto v = scalar_num("CUnit", unit_id, "RepairTime")) return *v;
        return std::nullopt;
    }

    float unit_display_height(const std::string& unit_id) const {
        if (auto v = scalar_num("CUnit", unit_id, "Height")) return float(*v);
        return 0.0f;
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

    double footprint_place_radius(const std::string& fp_id) const {
        double result = 0;
        for (const core::XmlNode& layer : resolved("CFootprint", fp_id)) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != "Place") continue;
            const std::string* area = layer.attr("Area");
            int minx = 0, miny = 0, maxx = 0, maxy = 0;
            if (!area || std::sscanf(area->c_str(), "%d,%d,%d,%d", &minx, &miny, &maxx, &maxy) != 4)
                continue;
            double best = 0;
            int seq = 0;
            for (const core::XmlNode& row : layer.children) {
                if (row.name != "Rows") continue;
                const std::string* v = row.attr("value");
                const std::string& bits = v ? *v : row.text;
                const std::string* ri = row.attr("index");
                const int r = ri ? std::atoi(ri->c_str()) : seq;
                for (std::size_t col = 0; col < bits.size(); ++col) {
                    const char ch = bits[col];
                    if (ch == '.' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                    const double dx = minx + double(col);
                    const double dy = miny + double(r);
                    const double d = std::sqrt(dx * dx + dy * dy);
                    if (d > best) best = d;
                }
                ++seq;
            }
            result = best;
        }
        return result;
    }

    double creep_source_radius(const std::string& unit_id) const {
        double best = 0;
        for (const std::string& beh : array_links_legacy("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorCreepSource") continue;
            if (auto fp = scalar("CBehavior", beh, "Grown")) {
                const double r = footprint_place_radius(*fp);
                if (r > best) best = r;
            }
        }
        return best;
    }

    std::vector<std::pair<int, int>> footprint_place_offsets(const std::string& fp_id) const {
        std::vector<std::pair<int, int>> result;
        for (const core::XmlNode& layer : resolved("CFootprint", fp_id)) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != "Place") continue;
            const std::string* area = layer.attr("Area");
            int minx = 0, miny = 0, maxx = 0, maxy = 0;
            if (!area || std::sscanf(area->c_str(), "%d,%d,%d,%d", &minx, &miny, &maxx, &maxy) != 4)
                continue;
            std::vector<std::pair<int, int>> cells;
            int seq = 0;
            for (const core::XmlNode& row : layer.children) {
                if (row.name != "Rows") continue;
                const std::string* v = row.attr("value");
                const std::string& bits = v ? *v : row.text;
                const std::string* ri = row.attr("index");
                const int r = ri ? std::atoi(ri->c_str()) : seq;
                for (std::size_t col = 0; col < bits.size(); ++col) {
                    const char ch = bits[col];
                    if (ch == '.' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                    cells.emplace_back(minx + int(col), miny + r);
                }
                ++seq;
            }
            result = std::move(cells);
        }
        return result;
    }

    std::vector<std::pair<int, int>> creep_source_footprint_offsets(const std::string& unit_id) const {
        for (const std::string& beh : array_links_legacy("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorCreepSource") continue;
            if (auto fp = scalar("CBehavior", beh, "Grown")) {
                auto cells = footprint_place_offsets(*fp);
                if (!cells.empty()) return cells;
            }
        }
        return {};
    }

    std::vector<std::pair<int, int>> unit_pathing_footprint_offsets(const std::string& unit_id) const {
        auto fp = scalar("CUnit", unit_id, "Footprint");
        if (!fp) return {};
        std::vector<std::pair<int, int>> result;
        auto has = [&](int x, int y) {
            for (const auto& c : result)
                if (c.first == x && c.second == y) return true;
            return false;
        };
        std::string aux;
        for (const core::XmlNode& layer : resolved("CFootprint", *fp)) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != "Place") continue;
            for (const core::XmlNode& s : layer.children)
                if (s.name == "Sets")
                    if (const std::string* c = s.attr("Character"); c && !c->empty())
                        if (aux.find((*c)[0]) == std::string::npos) aux.push_back((*c)[0]);
        }
        for (const core::XmlNode& layer : resolved("CFootprint", *fp)) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != "Place") continue;
            std::vector<std::pair<std::string, int>> rows;
            int seq = 0, maxcols = 0;
            for (const core::XmlNode& row : layer.children) {
                if (row.name != "Rows") continue;
                const std::string* v = row.attr("value");
                const std::string& bits = v ? *v : row.text;
                const std::string* ri = row.attr("index");
                const int r = ri ? std::atoi(ri->c_str()) : seq;
                rows.emplace_back(bits, r);
                if (int(bits.size()) > maxcols) maxcols = int(bits.size());
                ++seq;
            }
            if (rows.empty()) continue;
            int minx, miny, mx, my, mw, mh;
            const std::string* area = layer.attr("Area");
            if (area && std::sscanf(area->c_str(), "%d,%d,%d,%d", &mx, &my, &mw, &mh) == 4) {
                minx = mx;
                miny = my;
            } else {
                minx = -(maxcols / 2);
                miny = -(seq / 2);
            }
            for (const auto& [bits, r] : rows)
                for (std::size_t col = 0; col < bits.size(); ++col) {
                    const char ch = bits[col];
                    if (ch == '.' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                    if (aux.find(ch) != std::string::npos) continue;
                    const int x = minx + int(col), y = miny + r;
                    if (!has(x, y)) result.emplace_back(x, y);
                }
        }
        return result;
    }

    bool unit_can_build_addon(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilBuild") continue;
            if (auto t = scalar("CAbil", ab, "Type"); t && *t == "AddOn") return true;
        }
        return false;
    }

    std::string merge_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (e && e->tag == "CAbilMerge") return ab;
        }
        return {};
    }

    bool structure_requires_vespene(const std::string& unit_id) const {
        if (unit_id.empty()) return false;
        auto fam = families_.find("CAbil");
        if (fam == families_.end()) return false;
        for (const auto& [abil_id, entry] : fam->second) {
            if (entry.tag != "CAbilBuild") continue;
            for (const core::XmlNode& n : resolved("CAbil", abil_id)) {
                if (n.name != "InfoArray") continue;
                const std::string* built = n.attr("Unit");
                if (!built || *built != unit_id) continue;
                auto names_resource_validator = [&](const std::string& v) {
                    if (v.empty()) return false;
                    const Entry* ve = find("CValidator", v);
                    return ve && ve->tag == "CValidatorUnitCompareResourceContents";
                };
                if (const std::string* va = n.attr("ValidatorArray");
                    va && names_resource_validator(*va))
                    return true;
                for (const core::XmlNode& c : n.children) {
                    if (c.name != "ValidatorArray") continue;
                    if (const std::string* v = c.attr("value"); v && names_resource_validator(*v))
                        return true;
                }
            }
        }
        return false;
    }

    std::string burrow_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilMorph") continue;
            if (auto k = scalar("CAbil", ab, "ActorKey"); k && *k == "BurrowDown") return ab;
        }
        return {};
    }

    std::string burrowed_variant(const std::string& unit_id) const {
        const std::string ab = burrow_ability(unit_id);
        if (ab.empty()) return {};
        if (auto ms = morph_spec(ab)) return ms->target;
        return {};
    }

    std::string liftoff_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilMorph") continue;
            if (auto k = scalar("CAbil", ab, "ActorKey"); k && *k == "LiftOffLand") return ab;
        }
        return {};
    }

    std::string depot_lower_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilMorph") continue;
            for (const core::XmlNode& n : resolved("CAbil", ab))
                if (n.name == "CmdButtonArray" && n.attr_or("index", "") == "Execute" &&
                    n.attr_or("DefaultButtonFace", "") == "Lower")
                    return ab;
        }
        return {};
    }

    struct PhaseShiftInfo {
        bool found = false;
        std::string ability;
        std::string shade;
        double duration = 0;
    };
    std::vector<std::string> walk_effect_set(const std::string& start) const {
        std::vector<std::string> out;
        if (start.empty()) return out;
        std::vector<std::string> stack{start};
        out.reserve(8);
        auto seen = [&out](const std::string& s) {
            for (const std::string& o : out) if (o == s) return true;
            return false;
        };
        while (!stack.empty()) {
            std::string cur = stack.back();
            stack.pop_back();
            if (seen(cur)) continue;
            out.push_back(cur);
            const Entry* e = find("CEffect", cur);
            if (e && e->tag == "CEffectSet")
                for (const std::string& c : array_links("CEffect", cur, "EffectArray"))
                    if (!c.empty() && !seen(c)) stack.push_back(c);
        }
        return out;
    }
    struct PhaseShiftSpec {
        bool found = false;
        std::string shade;
        double duration = 0;
    };
    PhaseShiftSpec phase_shift_spec(const std::string& abil_id) const {
        const Entry* ae = find("CAbil", abil_id);
        if (!ae || ae->tag != "CAbilEffectTarget") return {};
        const std::string root = scalar("CAbil", abil_id, "Effect").value_or("");
        if (root.empty()) return {};
        std::string shade, spawn_eff;
        for (const std::string& eid : walk_effect_set(root)) {
            const Entry* ee = find("CEffect", eid);
            if (ee && ee->tag == "CEffectCreateUnit") {
                shade = scalar("CEffect", eid, "SpawnUnit").value_or("");
                spawn_eff = scalar("CEffect", eid, "SpawnEffect").value_or("");
                break;
            }
        }
        if (shade.empty() || spawn_eff.empty()) return {};
        for (const std::string& eid : walk_effect_set(spawn_eff)) {
            const Entry* ee = find("CEffect", eid);
            if (!ee || ee->tag != "CEffectApplyBehavior") continue;
            const std::string beh = scalar("CEffect", eid, "Behavior").value_or("");
            if (beh.empty()) continue;
            auto dur = scalar_num("CBehavior", beh, "Duration");
            const std::string expire = scalar("CBehavior", beh, "ExpireEffect").value_or("");
            if (!dur || *dur <= 0 || expire.empty()) continue;
            for (const std::string& teid : walk_effect_set(expire)) {
                const Entry* te = find("CEffect", teid);
                if (te && te->tag == "CEffectTeleport") return {true, shade, *dur};
            }
        }
        return {};
    }
    PhaseShiftInfo phase_shift_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            if (auto ps = phase_shift_spec(ab); ps.found) return {true, ab, ps.shade, ps.duration};
        }
        return {};
    }

    struct MassRecallInfo {
        bool found = false;
        std::string ability;
        double radius = 0;
    };
    MassRecallInfo mass_recall_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* ae = find("CAbil", ab);
            if (!ae || ae->tag != "CAbilEffectTarget") continue;
            const std::string root = scalar("CAbil", ab, "Effect").value_or("");
            if (root.empty()) continue;
            double radius = 0;
            bool has_teleport = false;
            std::vector<std::string> stack{root}, seen;
            seen.reserve(16);
            auto mark = [&seen](const std::string& s) {
                for (const std::string& o : seen)
                    if (o == s) return true;
                seen.push_back(s);
                return false;
            };
            while (!stack.empty() && seen.size() < 64) {
                const std::string cur = stack.back();
                stack.pop_back();
                if (cur.empty() || mark(cur)) continue;
                const Entry* e = find("CEffect", cur);
                if (!e) continue;
                if (e->tag == "CEffectTeleport") {
                    has_teleport = true;
                } else if (e->tag == "CEffectSet") {
                    for (const std::string& c : array_links("CEffect", cur, "EffectArray"))
                        if (!c.empty()) stack.push_back(c);
                } else if (e->tag == "CEffectEnumArea") {
                    std::string applied;
                    for (const core::XmlNode& n : resolved("CEffect", cur))
                        if (n.name == "AreaArray") {
                            if (radius <= 0)
                                if (const std::string* r = n.attr("Radius"))
                                    radius = std::strtod(r->c_str(), nullptr);
                            if (const std::string* a = n.attr("Effect")) applied = *a;
                            break;
                        }
                    if (!applied.empty()) stack.push_back(applied);
                } else if (e->tag == "CEffectApplyBehavior") {
                    const std::string beh = scalar("CEffect", cur, "Behavior").value_or("");
                    if (!beh.empty())
                        for (const char* k : {"ExpireEffect", "FinalEffect", "PeriodicEffect"}) {
                            const std::string nx = scalar("CBehavior", beh, k).value_or("");
                            if (!nx.empty()) stack.push_back(nx);
                        }
                }
            }
            if (radius > 0 && has_teleport) return {true, ab, radius};
        }
        return {};
    }

    struct RecallSpec {
        std::string search_effect;
        double radius = 0;
    };
    std::optional<RecallSpec> recall_spec(const std::string& abil_id) const {
        const Entry* ae = find("CAbil", abil_id);
        if (!ae || ae->tag != "CAbilEffectTarget") return std::nullopt;
        const std::string root = scalar("CAbil", abil_id, "Effect").value_or("");
        if (root.empty() || effect_subclass(root) != "EnumArea") return std::nullopt;
        auto ea = effect_enum_area(root);
        if (!ea || ea->radius <= 0) return std::nullopt;
        bool has_teleport = false;
        std::vector<std::string> stack{root}, seen;
        seen.reserve(16);
        auto mark = [&seen](const std::string& s) {
            for (const std::string& o : seen)
                if (o == s) return true;
            seen.push_back(s);
            return false;
        };
        while (!stack.empty() && seen.size() < 64) {
            const std::string cur = stack.back();
            stack.pop_back();
            if (cur.empty() || mark(cur)) continue;
            const Entry* e = find("CEffect", cur);
            if (!e) continue;
            if (e->tag == "CEffectTeleport") {
                has_teleport = true;
            } else if (e->tag == "CEffectSet") {
                for (const std::string& c : array_links("CEffect", cur, "EffectArray"))
                    if (!c.empty()) stack.push_back(c);
            } else if (e->tag == "CEffectEnumArea") {
                for (const core::XmlNode& n : resolved("CEffect", cur))
                    if (n.name == "AreaArray") {
                        if (const std::string* a = n.attr("Effect")) {
                            if (!a->empty()) stack.push_back(*a);
                        }
                        break;
                    }
            } else if (e->tag == "CEffectApplyBehavior") {
                const std::string beh = scalar("CEffect", cur, "Behavior").value_or("");
                if (!beh.empty())
                    for (const char* k : {"ExpireEffect", "FinalEffect", "PeriodicEffect"}) {
                        const std::string nx = scalar("CBehavior", beh, k).value_or("");
                        if (!nx.empty()) stack.push_back(nx);
                    }
            }
        }
        if (!has_teleport) return std::nullopt;
        return RecallSpec{root, ea->radius};
    }

    bool effect_flag_set(const std::string& id, const char* name) const {
        for (const core::XmlNode& c : resolved("CEffect", id))
            if (c.name == "Flags" && c.attr_or("index", "") == name && c.attr_or("value", "0") != "0")
                return true;
        return false;
    }
    bool behavior_has_state(const std::string& beh, const char* name) const {
        for (const core::XmlNode& m : resolved("CBehavior", beh)) {
            if (m.name != "Modification") continue;
            for (const core::XmlNode& s : m.children)
                if (s.name == "StateFlags" && s.attr_or("index", "") == name &&
                    s.attr_or("value", "0") != "0")
                    return true;
        }
        return false;
    }
    bool persistent_applies_stun(const std::string& root) const {
        std::vector<std::string> starts;
        if (auto s = scalar("CEffect", root, "InitialEffect"); s && !s->empty()) starts.push_back(*s);
        for (const core::XmlNode& c : resolved("CEffect", root))
            if (c.name == "PeriodicEffectArray") {
                std::string v = c.attr_or("value", "");
                if (v.empty()) v = c.attr_or("Link", "");
                if (!v.empty()) starts.push_back(v);
            }
        for (const std::string& st : starts)
            for (const std::string& eid : walk_effect_set(st)) {
                const Entry* ee = find("CEffect", eid);
                if (!ee || ee->tag != "CEffectApplyBehavior") continue;
                const std::string beh = scalar("CEffect", eid, "Behavior").value_or("");
                if (!beh.empty() && behavior_has_state(beh, "Stun")) return true;
            }
        return false;
    }
    bool behavior_raises_air(const std::string& beh) const {
        for (const core::XmlNode& m : resolved("CBehavior", beh)) {
            if (m.name != "Modification") continue;
            for (const core::XmlNode& s : m.children)
                if (s.name == "PlaneDelta" && s.attr_or("index", "") == "Air" &&
                    s.attr_or("value", "0") != "0")
                    return true;
        }
        return false;
    }
    bool persistent_applies_airlift(const std::string& root) const {
        std::vector<std::string> starts;
        if (auto s = scalar("CEffect", root, "InitialEffect"); s && !s->empty()) starts.push_back(*s);
        for (const core::XmlNode& c : resolved("CEffect", root))
            if (c.name == "PeriodicEffectArray") {
                std::string v = c.attr_or("value", "");
                if (v.empty()) v = c.attr_or("Link", "");
                if (!v.empty()) starts.push_back(v);
            }
        for (const std::string& st : starts)
            for (const std::string& eid : walk_effect_set(st)) {
                const Entry* ee = find("CEffect", eid);
                if (!ee || ee->tag != "CEffectApplyBehavior") continue;
                const std::string beh = scalar("CEffect", eid, "Behavior").value_or("");
                if (!beh.empty() && behavior_raises_air(beh)) return true;
            }
        return false;
    }

    struct RevelationInfo {
        bool found = false;
        std::string ability;
        double search_radius = 0;
        double reveal_radius = 0;
        double duration = 0;
    };
    RevelationInfo reveal_ability(const std::string& abil_id) const {
        if (abil_id.empty() || !find("CAbil", abil_id)) return {};
        std::string root = scalar("CAbil", abil_id, "Effect").value_or("");
        if (root.empty() && find("CEffect", abil_id)) root = abil_id;
        if (root.empty()) return {};
        {
            double area_radius = 0, reveal_radius = 0, reveal_dur = 0;
            bool has_reveal = false;
            std::vector<std::string> stack{root}, seen;
            seen.reserve(16);
            auto mark = [&seen](const std::string& s) {
                for (const std::string& o : seen)
                    if (o == s) return true;
                seen.push_back(s);
                return false;
            };
            while (!stack.empty() && seen.size() < 128) {
                const std::string cur = stack.back();
                stack.pop_back();
                if (cur.empty() || mark(cur)) continue;
                const Entry* e = find("CEffect", cur);
                if (!e) continue;
                if (e->tag == "CEffectCreatePersistent") {
                    if (auto rr = scalar_num("CEffect", cur, "RevealRadius"); rr && *rr > 0) {
                        has_reveal = true;
                        reveal_radius = *rr;
                        if (auto ed = scalar_num("CEffect", cur, "ExpireDelay")) reveal_dur = *ed;
                    }
                    if (auto s = scalar("CEffect", cur, "InitialEffect"); s && !s->empty()) stack.push_back(*s);
                } else if (e->tag == "CEffectSet") {
                    for (const std::string& c : array_links_legacy("CEffect", cur, "EffectArray"))
                        if (!c.empty()) stack.push_back(c);
                } else if (e->tag == "CEffectEnumArea") {
                    std::string applied;
                    for (const core::XmlNode& n : resolved("CEffect", cur))
                        if (n.name == "AreaArray") {
                            if (area_radius <= 0)
                                if (const std::string* r = n.attr("Radius"))
                                    area_radius = std::strtod(r->c_str(), nullptr);
                            if (const std::string* a = n.attr("Effect")) applied = *a;
                            break;
                        }
                    if (!applied.empty()) stack.push_back(applied);
                } else if (e->tag == "CEffectApplyBehavior") {
                    const std::string beh = scalar("CEffect", cur, "Behavior").value_or("");
                    if (!beh.empty())
                        for (const char* k : {"InitialEffect", "FinalEffect", "ExpireEffect", "PeriodicEffect"}) {
                            const std::string nx = scalar("CBehavior", beh, k).value_or("");
                            if (!nx.empty()) stack.push_back(nx);
                        }
                }
            }
            if (has_reveal && reveal_dur > 0)
                return {true, abil_id, area_radius, reveal_radius, reveal_dur};
        }
        return {};
    }

    RevelationInfo oracle_revelation_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* ae = find("CAbil", ab);
            if (!ae || ae->tag != "CAbilEffectTarget") continue;
            RevelationInfo r = reveal_ability(ab);
            if (r.found && r.search_radius > 0) return r;
        }
        return {};
    }

    struct GravitonBeamInfo {
        bool found = false;
        std::string ability;
        double duration = 0;
    };
    GravitonBeamInfo graviton_beam_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* ae = find("CAbil", ab);
            if (!ae || ae->tag != "CAbilEffectTarget") continue;
            std::string root = scalar("CAbil", ab, "Effect").value_or("");
            if (root.empty()) root = ab;
            const Entry* re = find("CEffect", root);
            if (!re || re->tag != "CEffectCreatePersistent") continue;
            if (!effect_flag_set(root, "Channeled")) continue;
            auto pc = scalar_num("CEffect", root, "PeriodCount");
            auto pp = scalar_num("CEffect", root, "PeriodicPeriodArray");
            if (!pc || !pp || *pc <= 0 || *pp <= 0) continue;
            if (!persistent_applies_stun(root)) continue;
            return {true, ab, (*pc) * (*pp)};
        }
        return {};
    }

    std::optional<double> yankback_mover_flighttime(const std::string& mover_id) const {
        if (mover_id.empty()) return std::nullopt;
        bool restore_height = false;
        std::optional<double> flight;
        for (const core::XmlNode& n : resolved("CMover", mover_id)) {
            if (n.name == "Flags" && n.attr_or("index", "") == "RestoreHeight" &&
                n.attr_or("value", "0") != "0")
                restore_height = true;
            if (n.name == "MotionPhases")
                for (const core::XmlNode& c : n.children)
                    if (c.name == "FlightTime")
                        if (const std::string* v = c.attr("value"); v && !v->empty()) {
                            const double d = std::strtod(v->c_str(), nullptr);
                            if (d > 0) flight = d;
                        }
        }
        return (restore_height && flight) ? flight : std::nullopt;
    }

    std::optional<double> yank_pull_of_ability(const std::string& ab) const {
        std::string root = scalar("CAbil", ab, "Effect").value_or("");
        if (root.empty()) root = ab;
        std::optional<double> pull;
        std::vector<std::string> stack{root}, seen;
        auto seen_before = [&seen](const std::string& s) {
            for (const std::string& o : seen) if (o == s) return true;
            return false;
        };
        while (!stack.empty() && seen.size() < 256) {
            const std::string cur = stack.back();
            stack.pop_back();
            if (cur.empty() || seen_before(cur)) continue;
            seen.push_back(cur);
            const Entry* ee = find("CEffect", cur);
            if (!ee) continue;
            if (ee->tag == "CEffectLaunchMissile") {
                std::string mover;
                for (const core::XmlNode& n : resolved("CEffect", cur))
                    if (n.name == "Movers") {
                        std::string m = n.attr_or("Link", "");
                        if (m.empty()) m = n.attr_or("value", "");
                        if (!m.empty()) mover = m;
                    }
                if (auto ft = yankback_mover_flighttime(mover)) { pull = ft; break; }
            }
            for (const core::XmlNode& n : resolved("CEffect", cur)) {
                std::string id;
                if (n.name == "EffectArray" || n.name == "PeriodicEffectArray") {
                    id = n.attr_or("value", "");
                    if (id.empty()) id = n.attr_or("Link", "");
                } else if (n.name == "CaseArray") {
                    id = n.attr_or("Effect", "");
                } else if (n.name == "CaseDefault" || n.name == "Effect" || n.name == "SpawnEffect" ||
                           n.name == "ImpactEffect" || n.name == "FinishEffect" ||
                           n.name == "InitialEffect") {
                    id = n.attr_or("value", "");
                    if (id.empty()) id = n.attr_or("Effect", "");
                }
                if (!id.empty() && !seen_before(id)) stack.push_back(id);
            }
        }
        return pull;
    }

    struct AbductInfo {
        bool found = false;
        std::string ability;
        double duration = 0;
        double range = 0;
    };
    AbductInfo abduct_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* ae = find("CAbil", ab);
            if (!ae || ae->tag != "CAbilEffectTarget") continue;
            auto pull = yank_pull_of_ability(ab);
            if (!pull || *pull <= 0) continue;
            const double range = scalar_num("CAbil", ab, "Range").value_or(0.0);
            return {true, ab, *pull, range};
        }
        return {};
    }

    std::optional<double> abduct_pull_time(const std::string& abil_id) const {
        const Entry* ae = find("CAbil", abil_id);
        if (!ae || ae->tag != "CAbilEffectTarget") return std::nullopt;
        auto pull = yank_pull_of_ability(abil_id);
        return (pull && *pull > 0) ? pull : std::nullopt;
    }

    struct InjectInfo {
        bool found = false;
        std::string ability;
        int count = 0;
        double delay = 0;
        double stun = 0;
    };
    struct InjectSpec {
        bool found = false;
        int count = 0;
        double delay = 0;
        double stun = 0;
    };
    InjectSpec inject_spec(const std::string& abil_id) const {
        const Entry* ae = find("CAbil", abil_id);
        if (!ae || ae->tag != "CAbilEffectTarget") return {};
        auto walk = [this](const std::string& root, auto&& visit) {
            std::vector<std::string> stack{root}, seen;
            auto seen_before = [&seen](const std::string& s) {
                for (const std::string& o : seen) if (o == s) return true;
                return false;
            };
            while (!stack.empty() && seen.size() < 256) {
                const std::string cur = stack.back();
                stack.pop_back();
                if (cur.empty() || seen_before(cur)) continue;
                seen.push_back(cur);
                const Entry* ee = find("CEffect", cur);
                if (!ee) continue;
                visit(cur, *ee);
                for (const core::XmlNode& n : resolved("CEffect", cur)) {
                    std::string id;
                    if (n.name == "EffectArray" || n.name == "PeriodicEffectArray") {
                        id = n.attr_or("value", "");
                        if (id.empty()) id = n.attr_or("Link", "");
                    } else if (n.name == "CaseArray") {
                        id = n.attr_or("Effect", "");
                    } else if (n.name == "CaseDefault" || n.name == "Effect" || n.name == "SpawnEffect" ||
                               n.name == "ImpactEffect" || n.name == "FinishEffect" ||
                               n.name == "InitialEffect") {
                        id = n.attr_or("value", "");
                        if (id.empty()) id = n.attr_or("Effect", "");
                    }
                    if (!id.empty() && !seen_before(id)) stack.push_back(id);
                }
            }
        };
        std::string root = scalar("CAbil", abil_id, "Effect").value_or("");
        if (root.empty()) root = abil_id;
        double delay = 0, stun = 0;
        std::string expire;
        walk(root, [&](const std::string& cur, const Entry& ee) {
            if (ee.tag != "CEffectApplyBehavior") return;
            const std::string buff = scalar("CEffect", cur, "Behavior").value_or("");
            if (buff.empty()) return;
            const Entry* be = find("CBehavior", buff);
            if (!be || be->tag != "CBehaviorBuff") return;
            const double dur = scalar_num("CBehavior", buff, "Duration").value_or(0.0);
            if (dur <= 0) return;
            const std::string exp = scalar("CBehavior", buff, "ExpireEffect").value_or("");
            if (!exp.empty()) {
                if (delay <= 0) { delay = dur; expire = exp; }
            } else if (stun <= 0) {
                stun = dur;
            }
        });
        if (delay <= 0 || expire.empty()) return {};
        int count = 0;
        walk(expire, [&](const std::string& cur, const Entry& ee) {
            if (count > 0) return;
            if (ee.tag != "CEffectApplyBehavior") return;
            if (auto c = scalar_num("CEffect", cur, "Count")) count = int(*c);
        });
        if (count <= 0) return {};
        return {true, count, delay, stun};
    }
    InjectInfo inject_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            if (auto iv = inject_spec(ab); iv.found) return {true, ab, iv.count, iv.delay, iv.stun};
        }
        return {};
    }

    struct MuleCalldownSpec { bool found = false; std::string unit; double life = 0; };
    MuleCalldownSpec mule_calldown_spec(const std::string& abil_id) const {
        const Entry* ae = find("CAbil", abil_id);
        if (!ae || ae->tag != "CAbilEffectTarget") return {};
        const std::string root = scalar("CAbil", abil_id, "Effect").value_or("");
        if (root.empty()) return {};
        auto is_harvester = [this](const std::string& uid) {
            for (const std::string& ab : array_links("CUnit", uid, "AbilArray")) {
                const Entry* e = ab.empty() ? nullptr : find("CAbil", ab);
                if (e && e->tag == "CAbilHarvest") return true;
            }
            return false;
        };
        std::string unit;
        double life = 0;
        std::vector<std::string> stack{root}, seen;
        while (!stack.empty() && seen.size() < 256) {
            const std::string cur = stack.back();
            stack.pop_back();
            if (cur.empty() || std::find(seen.begin(), seen.end(), cur) != seen.end()) continue;
            seen.push_back(cur);
            const Entry* ee = find("CEffect", cur);
            if (!ee) continue;
            if (ee->tag == "CEffectCreateUnit") {
                const std::string su = scalar("CEffect", cur, "SpawnUnit").value_or("");
                if (unit.empty() && !su.empty() && is_harvester(su)) unit = su;
            } else if (ee->tag == "CEffectApplyBehavior") {
                const std::string buff = scalar("CEffect", cur, "Behavior").value_or("");
                if (!buff.empty())
                    if (const Entry* be = find("CBehavior", buff); be && be->tag == "CBehaviorBuff")
                        if (const double dur = scalar_num("CBehavior", buff, "Duration").value_or(0.0);
                            dur > life && !scalar("CBehavior", buff, "ExpireEffect").value_or("").empty())
                            life = dur;
            }
            for (const core::XmlNode& n : resolved("CEffect", cur))
                if (n.name == "EffectArray" || n.name == "PeriodicEffectArray" || n.name == "CaseArray" ||
                    n.name == "CaseDefault" || n.name == "Effect" || n.name == "SpawnEffect" ||
                    n.name == "FinalEffect" || n.name == "ImpactEffect") {
                    std::string id = n.attr_or("value", "");
                    if (id.empty()) id = n.attr_or("Effect", "");
                    if (id.empty()) id = n.attr_or("Link", "");
                    if (!id.empty()) stack.push_back(id);
                }
        }
        if (unit.empty()) return {};
        return {true, unit, life};
    }

    struct CreepTumorInfo {
        bool found = false;
        std::string ability;
        std::string unit;
        double energy = 0;
    };
    CreepTumorInfo creep_tumor_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* ae = find("CAbil", ab);
            if (!ae || ae->tag != "CAbilBuild") continue;
            for (const core::XmlNode& n : resolved("CAbil", ab)) {
                if (n.name != "InfoArray") continue;
                const std::string built = n.attr_or("Unit", "");
                if (built.empty()) continue;
                if (creep_source_radius(built) <= 0) continue;
                double energy = 0;
                for (const core::XmlNode& g : n.children)
                    if (g.name == "Vital" && g.attr_or("index", "") == "Energy") {
                        const std::string v = g.attr_or("value", "");
                        if (!v.empty()) energy = std::atof(v.c_str());
                    }
                if (energy <= 0) continue;
                return {true, ab, built, energy};
            }
        }
        return {};
    }

    struct AutoTurretInfo {
        bool found = false;
        std::string ability;
        std::string unit;
        double energy = 0;
        double life = 0;
    };
    AutoTurretInfo auto_turret_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* ae = find("CAbil", ab);
            if (!ae || ae->tag != "CAbilEffectTarget") continue;
            std::string eff;
            for (const core::XmlNode& n : resolved("CAbil", ab))
                if (n.name == "Effect") {
                    const std::string v = n.attr_or("value", "");
                    if (!v.empty()) eff = v;
                }
            if (eff.empty()) continue;
            const auto cu = effect_create_unit(eff);
            if (!cu || cu->unit.empty()) continue;
            bool places = false;
            for (const core::XmlNode& n : resolved("CAbil", ab))
                if (n.name == "PlaceUnit" && n.attr_or("value", "") == cu->unit) places = true;
            if (!places) continue;
            double energy = 0;
            for (const core::XmlNode& c : resolved("CAbil", ab)) {
                if (c.name != "Cost") continue;
                for (const core::XmlNode& g : c.children)
                    if (g.name == "Vital" && g.attr_or("index", "") == "Energy") {
                        const std::string v = g.attr_or("value", "");
                        if (!v.empty()) energy = std::atof(v.c_str());
                    }
            }
            double life = 0;
            if (!cu->spawn_effect.empty())
                if (auto beh = effect_apply_behavior(cu->spawn_effect))
                    if (auto bb = behavior_buff(*beh))
                        life = bb->duration;
            return {true, ab, cu->unit, energy, life};
        }
        return {};
    }

    struct InfestedTerranInfo {
        bool found = false;
        std::string ability;
        std::string egg;
        std::string unit;
        int count = 1;
        double energy = 0;
        double life = 0;
    };
    InfestedTerranInfo infested_terran_spec(const std::string& abil_id) const {
        const Entry* ae = find("CAbil", abil_id);
        if (!ae || ae->tag != "CAbilEffectTarget") return {};
        std::string eff;
        for (const core::XmlNode& n : resolved("CAbil", abil_id))
            if (n.name == "Effect") {
                const std::string v = n.attr_or("value", "");
                if (!v.empty()) eff = v;
            }
        if (eff.empty()) return {};
        const auto cu = effect_create_unit(eff);
        if (!cu || cu->unit.empty()) return {};
        const std::string egg = cu->unit;
        std::string combat, finish_effect;
        for (const std::string& m : array_links("CUnit", egg, "AbilArray")) {
            if (m.empty()) continue;
            const Entry* me = find("CAbil", m);
            if (!me || me->tag != "CAbilMorph") continue;
            for (const core::XmlNode& info : resolved("CAbil", m)) {
                if (info.name != "InfoArray") continue;
                std::string u = info.attr_or("Unit", "");
                if (u.empty())
                    for (const core::XmlNode& ch : info.children)
                        if (ch.name == "Unit") {
                            if (const std::string* v = ch.attr("value")) u = *v;
                            break;
                        }
                if (u.empty()) continue;
                combat = u;
                for (const core::XmlNode& sec : info.children) {
                    if (sec.name != "SectionArray") continue;
                    for (const core::XmlNode& fx : sec.children)
                        if (fx.name == "EffectArray" && fx.attr_or("index", "") == "Finish") {
                            const std::string v = fx.attr_or("value", "");
                            if (!v.empty()) finish_effect = v;
                        }
                }
            }
            if (!combat.empty()) break;
        }
        if (combat.empty()) return {};
        double energy = 0;
        for (const core::XmlNode& c : resolved("CAbil", abil_id)) {
            if (c.name != "Cost") continue;
            for (const core::XmlNode& g : c.children)
                if (g.name == "Vital" && g.attr_or("index", "") == "Energy") {
                    const std::string v = g.attr_or("value", "");
                    if (!v.empty()) energy = std::atof(v.c_str());
                }
        }
        double life = 0;
        if (!finish_effect.empty())
            if (auto beh = effect_apply_behavior(finish_effect))
                if (auto bb = behavior_buff(*beh))
                    life = bb->duration;
        return {true, abil_id, egg, combat, cu->count, energy, life};
    }
    InfestedTerranInfo infested_terran_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            if (auto s = infested_terran_spec(ab); s.found) return s;
        }
        return {};
    }

    struct TechTrain {
        std::string unit;
        double time = 0;
    };
    TechTrain techlab_train_product(const std::string& producer_id) const {
        for (const std::string& ab : array_links("CUnit", producer_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilTrain") continue;
            for (const core::XmlNode& info : resolved("CAbil", ab)) {
                if (info.name != "InfoArray") continue;
                std::string product = info.attr_or("Unit", "");
                if (product.empty())
                    for (const core::XmlNode& ch : info.children)
                        if (ch.name == "Unit") {
                            if (const std::string* v = ch.attr("value")) product = *v;
                            break;
                        }
                if (product.empty()) continue;
                std::string req;
                for (const core::XmlNode& ch : info.children)
                    if (ch.name == "Button") {
                        req = ch.attr_or("Requirements", "");
                        break;
                    }
                if (req.find("Attached") != std::string::npos && req.find("TechLab") != std::string::npos) {
                    const std::string t = info.attr_or("Time", "");
                    return {product, t.empty() ? 0.0 : std::atof(t.c_str())};
                }
            }
        }
        return {};
    }

    struct TrainProduct {
        std::string unit;
        std::string ability;
        std::string command;
        std::string face;
        int row = -1, col = -1;
        double time = 0;
        bool requires_techlab = false;
    };
    std::vector<TrainProduct> unit_train_products(const std::string& producer_id) const {
        std::vector<TrainProduct> out;
        for (const AbilityCardButton& b : unit_ability_card_buttons(producer_id)) {
            const Entry* e = find("CAbil", b.ability);
            if (!e || e->tag != "CAbilTrain") continue;
            if (b.command.empty()) continue;
            TrainProduct p;
            p.ability = b.ability;
            p.command = b.command;
            p.face = b.face;
            p.row = b.row;
            p.col = b.col;
            for (const core::XmlNode& info : resolved("CAbil", b.ability)) {
                if (info.name != "InfoArray") continue;
                if (info.attr_or("index", "") != b.command) continue;
                std::string unit = info.attr_or("Unit", "");
                if (const std::string t = info.attr_or("Time", ""); !t.empty()) p.time = std::atof(t.c_str());
                for (const core::XmlNode& ch : info.children) {
                    if (ch.name == "Unit") {
                        if (const std::string* v = ch.attr("value")) unit = *v;
                    } else if (ch.name == "Time") {
                        if (const std::string* v = ch.attr("value")) p.time = std::atof(v->c_str());
                    } else if (ch.name == "Button") {
                        const std::string req = ch.attr_or("Requirements", "");
                        if (req.find("Attached") != std::string::npos &&
                            req.find("TechLab") != std::string::npos)
                            p.requires_techlab = true;
                    }
                }
                p.unit = std::move(unit);
                break;
            }
            if (p.unit.empty()) continue;
            out.push_back(std::move(p));
        }
        return out;
    }

    struct BuildProduct {
        std::string unit;
        std::string ability;
        std::string command;
        std::string face;
        int row = -1, col = -1;
        double time = 0;
        bool is_gas = false;
    };
    std::vector<BuildProduct> unit_build_products(const std::string& worker_id) const {
        std::vector<BuildProduct> out;
        for (const AbilityCardButton& b : unit_ability_card_buttons(worker_id)) {
            const Entry* e = find("CAbil", b.ability);
            if (!e || e->tag != "CAbilBuild") continue;
            if (b.command.empty()) continue;
            BuildProduct p;
            p.ability = b.ability;
            p.command = b.command;
            p.face = b.face;
            p.row = b.row;
            p.col = b.col;
            bool unit_set = false;
            for (const core::XmlNode& info : resolved("CAbil", b.ability)) {
                if (info.name != "InfoArray") continue;
                if (info.attr_or("index", "") != b.command) continue;
                if (const std::string* u = info.attr("Unit")) { p.unit = *u; unit_set = true; }
                if (const std::string t = info.attr_or("Time", ""); !t.empty()) p.time = std::atof(t.c_str());
                for (const core::XmlNode& ch : info.children) {
                    if (ch.name == "Unit") {
                        if (const std::string* v = ch.attr("value")) { p.unit = *v; unit_set = true; }
                    } else if (ch.name == "Time") {
                        if (const std::string* v = ch.attr("value")) p.time = std::atof(v->c_str());
                    } else if (ch.name == "ValidatorArray") {
                        const std::string v = ch.attr_or("value", "");
                        if (v.find("Vespene") != std::string::npos) p.is_gas = true;
                    }
                }
            }
            if (!unit_set || p.unit.empty()) continue;
            out.push_back(std::move(p));
        }
        return out;
    }

    struct ResearchInfo {
        bool found = false;
        int minerals = 0;
        int gas = 0;
        double time = 0;
    };
    ResearchInfo research_info(const std::string& upgrade_id) const {
        for (const std::string& ab : ids("CAbil")) {
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilResearch") continue;
            for (const core::XmlNode& info : resolved("CAbil", ab)) {
                if (info.name != "InfoArray") continue;
                std::string up = info.attr_or("Upgrade", "");
                if (up.empty())
                    for (const core::XmlNode& ch : info.children)
                        if (ch.name == "Upgrade") {
                            if (const std::string* v = ch.attr("value")) up = *v;
                            break;
                        }
                if (up != upgrade_id) continue;
                ResearchInfo r;
                r.found = true;
                if (const std::string t = info.attr_or("Time", ""); !t.empty())
                    r.time = std::atof(t.c_str());
                for (const core::XmlNode& ch : info.children) {
                    if (ch.name == "Time") {
                        if (const std::string* v = ch.attr("value")) r.time = std::atof(v->c_str());
                    } else if (ch.name == "Resource") {
                        const std::string idx = ch.attr_or("index", "");
                        const std::string val = ch.attr_or("value", "");
                        if (idx == "Minerals") r.minerals = std::atoi(val.c_str());
                        else if (idx == "Vespene") r.gas = std::atoi(val.c_str());
                    }
                }
                return r;
            }
        }
        return {};
    }

    int resource_harvest_amount(const std::string& unit_id) const {
        for (const std::string& beh : array_links_legacy("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            if (auto a = scalar_num("CBehavior", beh, "HarvestAmount")) return int(*a);
            return 5;
        }
        return 0;
    }

    std::optional<double> resource_harvest_time(const std::string& unit_id) const {
        for (const std::string& beh : array_links_legacy("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            return scalar_num("CBehavior", beh, "HarvestTime");
        }
        return std::nullopt;
    }

    int resource_content(const std::string& unit_id) const {
        for (const std::string& beh : array_links_legacy("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            if (auto a = scalar_num("CBehavior", beh, "Contents")) return int(*a);
            return 0;
        }
        return 0;
    }

    int transport_capacity(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilTransport") continue;
            if (auto s = scalar_num("CAbil", ab, "TotalCargoSpace")) return int(*s);
            return 0;
        }
        return 0;
    }

    bool requires_creep(const std::string& unit_id) const {
        auto fp = scalar("CUnit", unit_id, "PlacementFootprint");
        if (!fp || fp->empty()) return false;
        bool result = false;
        for (const core::XmlNode& layer : resolved("CFootprint", *fp)) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != "Check") continue;
            bool creep_here = false;
            std::function<void(const core::XmlNode&)> visit = [&](const core::XmlNode& n) {
                if (n.name == "Positive") {
                    const std::string* idx = n.attr("index");
                    const std::string* val = n.attr("value");
                    if (idx && *idx == "Creep" && (!val || *val != "0")) creep_here = true;
                }
                for (const core::XmlNode& c : n.children) visit(c);
            };
            for (const core::XmlNode& c : layer.children) visit(c);
            result = creep_here;
        }
        return result;
    }

    double power_source_radius(const std::string& unit_id) const {
        double best = 0;
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorPowerSource") continue;
            if (scalar("CBehavior", beh, "PowerLevel")) continue;
            if (scalar("CBehavior", beh, "Requirements")) continue;
            if (auto r = scalar_num("CBehavior", beh, "Radius"); r && *r > best) best = *r;
        }
        return best;
    }

    bool requires_power(const std::string& unit_id) const {
        bool relay = false;
        bool primary_field = false;
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e) continue;
            if (e->tag == "CBehaviorPowerUser") return true;
            if (e->tag == "CBehaviorPowerSource")
                (scalar("CBehavior", beh, "PowerLevel") ? relay : primary_field) = true;
        }
        return relay && !primary_field;
    }

    double creep_source_period(const std::string& unit_id) const {
        for (const std::string& beh : array_links_legacy("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorCreepSource") continue;
            if (auto p = scalar_num("CBehavior", beh, "Period")) return *p;
        }
        return 0;
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

    struct ArcSplash {
        double arc_deg;
        double radius;
        double frac;
    };
    std::vector<ArcSplash> weapon_arc_splash(const std::string& weapon_id) const;

    std::optional<double> missile_speed(const std::string& weapon_id) const;

    const std::vector<core::XmlNode>& singleton_fields(const std::string& family) const;

    std::optional<std::string> tag_default_scalar(const std::string& tag,
                                                  const std::string& field) const;

private:
    struct TwoSlot { std::string link; bool present = false; bool dead = false; };
    std::vector<TwoSlot> resolve_array_slots(const std::string& family,
                                             const std::string& id,
                                             const std::string& field) const;

    using IdMap = std::unordered_map<std::string, Entry>;
    std::unordered_map<std::string, IdMap> families_;
    std::unordered_map<std::string, std::string> consts_;
    std::unordered_map<std::string, std::vector<core::XmlNode>> singletons_;
    std::unordered_map<std::string, std::vector<core::XmlNode>> tag_defaults_;
    StringTable strings_;

    mutable std::unordered_map<std::string, std::vector<core::XmlNode>> resolved_cache_;

    template <class T, class Extract>
    std::optional<T> walk_effect_first(
        const std::string& effect_id, int depth, Extract&& extract,
        const std::function<std::optional<T>(const std::string&, int)>& trailing = {}) const {
        if (depth > 32) return std::nullopt;
        const Entry* e = find("CEffect", effect_id);
        if (!e) return std::nullopt;
        bool terminal = false;
        if (std::optional<T> here = extract(effect_id, e->tag, depth, terminal); terminal) return here;
        for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
            if (auto link = scalar("CEffect", effect_id, f); link && !link->empty())
                if (auto d = walk_effect_first<T>(*link, depth + 1, extract, trailing)) return d;
        for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
            for (const std::string& link : array_links("CEffect", effect_id, f))
                if (!link.empty())
                    if (auto d = walk_effect_first<T>(link, depth + 1, extract, trailing)) return d;
        if (trailing)
            if (auto d = trailing(effect_id, depth)) return d;
        return std::nullopt;
    }

    template <class T>
    std::optional<T> walk_damage_tree(
        const std::string& effect_id, int depth,
        const std::function<std::optional<T>(const std::string&)>& at_damage,
        const std::function<T(T, const std::string&)>& periodic,
        const std::string& prefer = {}) const {
        if (depth > 32) return std::nullopt;
        const Entry* e = find("CEffect", effect_id);
        if (!e) return std::nullopt;
        const std::string& tag = e->tag;
        if (tag == "CEffectDamage")
            return (prefer.empty() || effect_id == prefer) ? at_damage(effect_id) : std::nullopt;
        auto follow = [&](const char* f) -> std::optional<T> {
            if (auto link = scalar("CEffect", effect_id, f); link && !link->empty())
                return walk_damage_tree<T>(*link, depth + 1, at_damage, periodic, prefer);
            return std::nullopt;
        };
        auto follow_array = [&](const char* f) -> std::optional<T> {
            for (const std::string& link : array_links("CEffect", effect_id, f))
                if (!link.empty())
                    if (auto d = walk_damage_tree<T>(link, depth + 1, at_damage, periodic, prefer)) return d;
            return std::nullopt;
        };
        if (tag == "CEffectLaunchMissile") {
            if (auto d = follow("ImpactEffect")) return d;
        } else if (tag == "CEffectCreatePersistent") {
            if (auto d = follow_array("PeriodicEffectArray")) return periodic(*d, effect_id);
            if (auto d = follow("InitialEffect")) return d;
        } else if (tag == "CEffectSet") {
            if (auto d = follow_array("EffectArray")) return d;
        } else if (tag == "CEffectSwitch") {
            for (const std::string& ce : switch_effects(effect_id))
                if (auto d = walk_damage_tree<T>(ce, depth + 1, at_damage, periodic, prefer)) return d;
        }
        for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
            if (auto d = follow(f)) return d;
        for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
            if (auto d = follow_array(f)) return d;
        return std::nullopt;
    }

    std::optional<int> effect_damage(const std::string& effect_id,
                                     int depth,
                                     const std::string& prefer = {}) const;
    std::optional<int> effect_hit_count(const std::string& effect_id,
                                        int depth,
                                        const std::string& prefer = {}) const;
    std::optional<double> effect_life_change(const std::string& effect_id, int depth) const;
    std::optional<double> effect_energy_fraction(const std::string& effect_id, int depth) const;
    std::optional<std::string> effect_apply_behavior(const std::string& effect_id, int depth) const;
    std::optional<CreateUnitSpec> effect_create_unit(const std::string& effect_id, int depth) const;
    std::optional<PersistentSpec> effect_persistent(const std::string& effect_id, int depth) const;
    std::optional<double> apply_behavior_duration(const std::string& effect_id, int depth) const;
    std::optional<std::pair<double, double>> area_damage_radius(const std::string& effect_id,
                                                               int depth) const;
    std::optional<std::string> damage_effect_id(const std::string& effect_id,
                                                int depth) const;
    std::vector<std::string> switch_effects(const std::string& effect_id) const;
    static std::string expand_id(std::string v, const std::string& id);
};

}
