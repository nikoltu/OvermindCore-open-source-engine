#pragma once

#include "omc/core/env_once.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "omc/core/xml.hpp"
#include "omc/data/authored.hpp"
#include "omc/data/strings.hpp"

namespace omc::casc {
class Storage;
}

namespace omc::data {

struct TintColorArgs {
    std::array<float, 3> rgb = {1.0f, 1.0f, 1.0f};
    float blend_s = 0.0f;
    std::string mode;
    std::string key;
};

bool parse_tint_color(const std::vector<std::string>& args, TintColorArgs& out);

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

    const std::vector<std::string>& load_skips() const { return load_skips_; }

    static const std::vector<std::string>& unlisted_families() { return unlisted_families_; }
    static void clear_unlisted_families() { unlisted_families_.clear(); }

    void load_strings(const casc::Storage& store);

    void load_root_files(const casc::Storage& store,
                         const std::vector<std::string>& roots,
                         const std::vector<std::string>& basenames);

    void load_campaign(const casc::Storage& store,
                       const std::vector<std::string>& campaign_roots,
                       const std::string& mission_local_root = "");

    void load_missing_only(const casc::Storage& store, const std::vector<std::string>& roots);

    static std::vector<std::string> dependency_data_roots(const casc::Storage& store,
                                                          const std::vector<std::string>& dependencies);
    std::size_t load_dependencies(const casc::Storage& store, const std::vector<std::string>& dependencies);
    const std::vector<std::string>& dependency_roots_loaded() const { return dependency_roots_loaded_; }

    void load_strings_text(const std::string& text) { strings_.load_text(text); }

    void load_ai_scripts(const casc::Storage& store);
    void load_ai_script_text(const std::string& path, std::string text) { ai_scripts_[path] = std::move(text); }
    const std::string* ai_script(const std::string& path) const {
        const auto it = ai_scripts_.find(path);
        return it == ai_scripts_.end() ? nullptr : &it->second;
    }
    const StringTable& strings() const { return strings_; }

    void load_assets(const casc::Storage& store);
    void load_assets_text(const std::string& text) { assets_.load_text(text); }
    std::string resolve_asset(const std::string& ref) const { return assets_.resolve(ref); }
    std::string resolve_asset(const std::string& ref, const std::string& race,
                              const std::string& skin = std::string()) const {
        return assets_.resolve(ref, race, skin);
    }

    struct ConsoleSkinModels {
        std::string minimap, info, command;
        float minimap_pos[3] = {0, 0, 0}, info_pos[3] = {0, 0, 0}, command_pos[3] = {0, 0, 0};
        float minimap_scale[3] = {1, 1, 1}, info_scale[3] = {1, 1, 1}, command_scale[3] = {1, 1, 1};
        bool ok() const { return !minimap.empty() && !info.empty() && !command.empty(); }
    };
    std::string expand_entry_tokens(const std::string& family, const std::string& id,
                                    std::string v) const;
    ConsoleSkinModels console_skin_models(const std::string& skin_id, const std::string& race = std::string()) const;

    std::optional<std::string> console_skin_featured_image(const std::string& skin_id) const;

    std::optional<std::string> button_display_name(const std::string& button_id) const;
    std::optional<std::string> button_hotkey(const std::string& button_id) const;
    void load_hotkeys(const casc::Storage& store, const std::vector<std::string>& roots);
    void load_hotkeys_text(const std::string& text) { hotkeys_.load_text(text); }
    const StringTable& hotkeys() const { return hotkeys_; }
    std::optional<std::string> unit_display_name(const std::string& unit_id) const;
    std::optional<std::string> button_tooltip(const std::string& button_id) const {
        if (button_id.empty()) return std::nullopt;
        const std::string* raw = strings().find("Button/Tooltip/" + button_id);
        if (!raw || raw->empty()) return std::nullopt;
        std::string out;
        out.reserve(raw->size());
        for (std::size_t i = 0; i < raw->size();) {
            if ((*raw)[i] != '<') { out += (*raw)[i++]; continue; }
            const std::size_t end = raw->find('>', i);
            if (end == std::string::npos) { out += (*raw)[i++]; continue; }
            const std::string tag = raw->substr(i, end - i + 1);
            if (tag == "<n/>" || tag == "<N/>") out += '\n';
            i = end + 1;
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == ' ')) out.pop_back();
        if (out.empty()) return std::nullopt;
        return out;
    }
    std::optional<std::string> unit_life_armor_name(const std::string& unit_id) const {
        if (unit_id.empty()) return std::nullopt;
        const auto key = scalar("CUnit", unit_id, "LifeArmorName");
        if (!key || key->empty()) return std::nullopt;
        if (const std::string* text = strings().find(*key); text && !text->empty()) return *text;
        return std::nullopt;
    }
    std::optional<std::string> ability_display_name(const std::string& ability_id) const;

    void ingest(const std::vector<uint8_t>& xml);

    const Entry* find(const std::string& family, const std::string& id) const;

    std::vector<std::string> ids(const std::string& family) const;

    using FieldReadLog = std::set<std::pair<std::string, std::string>>;
    void set_field_read_log(FieldReadLog* log) const { field_read_log_ = log; }
    void set_field_touch_log(FieldReadLog* log) const { field_touch_log_ = log; }
    FieldReadLog authored_field_pairs() const;

    const core::XmlNodeList& resolved(const std::string& family, const std::string& id) const;
    std::size_t memory_bytes(std::size_t* entries, std::size_t* resolved_cache) const;

    std::optional<std::string> scalar(const std::string& family,
                                      const std::string& id,
                                      const std::string& field) const;
    std::optional<double> scalar_num(const std::string& family,
                                     const std::string& id,
                                     const std::string& field) const;

    std::vector<std::string> array_links(const std::string& family,
                                         const std::string& id,
                                         const std::string& field) const;

    int own_declaration_count(const std::string& family, const std::string& id) const {
        const Entry* e = find(family, id);
        return e ? int(e->decl_seen) : 0;
    }

    static std::string family_of(const std::string& tag);

    const std::string* const_value(const std::string& id) const;

    std::string expand_actor_tokens(const std::string& in, const std::string& unit_name,
                                    const std::string& self_id) const;

    std::string actor_unit_name(const std::string& actor_id, const std::string& family) const;
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

    bool ability_flag(const std::string& abil_id, const std::string& flag) const {
        bool on = false;
        for (const core::XmlNode& n : resolved("CAbil", abil_id))
            if (n.name == "FlagArray" && n.attr_or("index", "") == flag)
                on = n.attr_or("value", "") == "1";
        return on;
    }

    std::string ability_command_requirement(const std::string& abil_id,
                                            const std::string& cmd_index) const;

    std::string button_requirement(const std::string& abil_id,
                                   const std::string& cmd_index) const {
        if (const auto row = ability_info_row(abil_id, cmd_index))
            if (row->requirement_written) return row->requirement;
        return ability_command_requirement(abil_id, cmd_index);
    }

    struct CommandCardSlot {
        int index = -1;
        std::string type;
        std::string ability;
        std::string command;
        std::string face;
        std::string submenu_id;
        std::string requirements;
        int row = -1;
        int col = -1;
    };
    struct CommandCardLayout {
        int index = -1;
        std::string card_id;
        std::vector<CommandCardSlot> slots;
    };
    std::vector<CommandCardLayout> unit_command_cards(const std::string& unit_id) const;

    std::vector<std::pair<std::string, std::string>>
    unit_ability_commands(const std::string& unit_id) const;

    std::string cabil_effect_link(const std::string& abil_id) const;

    std::optional<std::string> button_icon(const std::string& button_id) const;
    std::optional<std::string> ability_set_id(const std::string& abil_id) const {
        auto v = scalar("CAbil", abil_id, "AbilSetId");
        if (!v || v->empty()) return std::nullopt;
        return v;
    }
    std::optional<std::string> ability_redirect_target(const std::string& abil_id) const {
        const Entry* e = find("CAbil", abil_id);
        if (!e || e->tag.rfind("CAbilRedirect", 0) != 0) return std::nullopt;
        auto v = scalar("CAbil", abil_id, "Abil");
        if (!v || v->empty()) return std::nullopt;
        return v;
    }
    std::optional<std::string> behavior_info_icon(const std::string& behavior_id) const;
    struct CursorLook {
        std::string texture;
        int hotspot_x = 0, hotspot_y = 0;
    };
    std::optional<CursorLook> cursor_look(const std::string& cursor_id) const;

    struct AbilityCardButton {
        std::string ability;
        std::string command;
        std::string face;
        int row = -1;
        int col = -1;
        int card = 0;
    };
    std::vector<AbilityCardButton> unit_ability_card_buttons(const std::string& unit_id) const;

    struct AbilityAutocast {
        bool offered = false;
        bool on_by_default = false;
        double range = 0.0;
        bool targets_allies = false;
    };
    AbilityAutocast ability_autocast(const std::string& abil_id) const;
    std::vector<std::string> morph_finish_autocast_off(const std::string& abil_id) const;

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

    std::pair<double, double> weapon_random_delay(const std::string& weapon_id) const {
        const Entry* e = find("CWeapon", weapon_id);
        auto read = [&](const char* f) {
            if (auto v = scalar_num("CWeapon", weapon_id, f)) return *v;
            if (e)
                if (auto d = tag_default_scalar(e->tag, f); d && !d->empty()) return std::strtod(d->c_str(), nullptr);
            return 0.0;
        };
        return {read("RandomDelayMin"), read("RandomDelayMax")};
    }

    bool unit_has_worker_rally(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilRally") continue;
            for (const core::XmlNode& n : resolved("CAbil", ab))
                if (n.name == "InfoArray" && n.attr_or("UseFilters", "").compare(0, 6, "Worker") == 0) return true;
        }
        return false;
    }

    bool mover_placement_bit(const std::string& mover_id, const char* slot, const char* bit) const {
        bool on = false;
        for (const core::XmlNode& n : resolved("CMover", mover_id)) {
            if (n.name != "PlacementArray" || n.attr_or("index", "") != slot) continue;
            for (const core::XmlNode& b : n.children)
                if (b.name == "Bits" && b.attr_or("index", "") == bit) on = b.attr_or("value", "1") != "0";
        }
        return on;
    }

    bool unit_type_validator_rejects(const std::string& validator_id, const std::string& unit_id) const {
        const Entry* e = find("CValidator", validator_id);
        if (!e || e->tag != "CValidatorUnitType") return false;
        const auto value = scalar("CValidator", validator_id, "Value");
        const auto want = scalar("CValidator", validator_id, "Find");
        const bool find_mode = !want || *want != "0";
        const bool is_type = value && *value == unit_id;
        return is_type != find_mode;
    }

    bool unit_rally_exempt(const std::string& unit_id) const {
        auto fam = families_.find("CAbil");
        if (fam == families_.end()) return false;
        for (const auto& [abil_id, entry] : fam->second) {
            if (entry.tag != "CAbilRally") continue;
            int validated = 0, rejected = 0;
            for (const core::XmlNode& n : resolved("CAbil", abil_id)) {
                if (n.name != "InfoArray") continue;
                bool has = false, rejects = false;
                for (const core::XmlNode& c : n.children) {
                    if (c.name != "UseValidators") continue;
                    has = true;
                    if (const std::string* v = c.attr("value"); v && unit_type_validator_rejects(*v, unit_id)) rejects = true;
                }
                if (!has) continue;
                ++validated;
                if (rejects) ++rejected;
            }
            if (validated > 0 && rejected == validated) return true;
        }
        return false;
    }

    double weapon_range_slop(const std::string& weapon_id) const {
        return scalar_num("CWeapon", weapon_id, "RangeSlop").value_or(1.0);
    }

    double weapon_fire_arc_deg(const std::string& weapon_id) const {
        const double arc = scalar_num("CWeapon", weapon_id, "Arc").value_or(0.0);
        const double slop = scalar_num("CWeapon", weapon_id, "ArcSlop").value_or(11.25);
        return arc * 0.5 + slop;
    }

    bool effect_damage_hidden(const std::string& effect_id) const {
        auto v = scalar("CEffect", effect_id, "Visibility");
        return v && *v == "Hidden";
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
        if (auto id = damage_effect_id(effect_id, 0)) return effect_amount(*id);
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

    std::optional<double> effect_amount(const std::string& effect_id) const {
        if (auto a = scalar_num("CEffect", effect_id, "Amount")) return a;
        if (const Entry* e = find("CEffect", effect_id))
            if (auto d = tag_default_scalar(e->tag, "Amount"); d && !d->empty()) return std::strtod(d->c_str(), nullptr);
        return std::nullopt;
    }
    struct EnumAreaSpec {
        std::string child;
        double radius = 0;
        int max_count = 0;
        bool exclude_target = false;
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
            if (const std::string* mc = n.attr("MaxCount"); mc && !mc->empty()) s.max_count = std::atoi(mc->c_str());
        }
        if (auto it = slots.find(0); it != slots.end() && !it->second.child.empty()) {
            for (const core::XmlNode& n : resolved("CEffect", effect_id))
                if (n.name == "ExcludeArray")
                    if (const std::string* v = n.attr("Value"); v && *v == "Target") it->second.exclude_target = true;
            return it->second;
        }
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

    std::uint8_t effect_search_planes(const std::string& effect_id) const {
        auto sf = scalar("CEffect", effect_id, "SearchFilters");
        if (!sf || sf->empty()) return 1 | 2;
        const std::size_t semi = sf->find(';');
        const std::string req = semi == std::string::npos ? *sf : sf->substr(0, semi);
        const std::string exc = semi == std::string::npos ? std::string{} : sf->substr(semi + 1);
        auto planes_of = [](const std::string& part) {
            std::uint8_t bits = 0;
            std::size_t i = 0;
            while (i < part.size()) {
                std::size_t j = part.find(',', i);
                if (j == std::string::npos) j = part.size();
                const std::string tok = part.substr(i, j - i);
                if (tok == "Ground") bits |= 1;
                else if (tok == "Air") bits |= 2;
                i = j + 1;
            }
            return bits;
        };
        const std::uint8_t req_bits = planes_of(req), exc_bits = planes_of(exc);
        std::uint8_t allowed = std::uint8_t((1 | 2) & ~exc_bits);
        if (req_bits) allowed = std::uint8_t(allowed & req_bits);
        return allowed;
    }

    struct SplashFilters {
        std::uint8_t affiliation = 8 | 16;
        std::uint8_t planes = 1 | 2;
        bool excludes_structure = false;
        bool offset_by_radius = false;
    };
    SplashFilters weapon_splash_filters(const std::string& weapon_id) const {
        SplashFilters f;
        auto did = damage_effect_id(weapon_effect_id(weapon_id), 0);
        if (!did) return f;
        f.affiliation = effect_search_affiliation(*did);
        f.planes = effect_search_planes(*did);
        f.excludes_structure = effect_search_excludes_structure(*did);
        f.offset_by_radius = effect_search_offset_by_radius(*did);
        return f;
    }

    bool effect_search_offset_by_radius(const std::string& effect_id) const {
        bool on = false;
        for (const core::XmlNode& n : resolved("CEffect", effect_id))
            if (n.name == "SearchFlags" && n.attr_or("index", "") == "OffsetByUnitRadius")
                on = n.attr_or("value", "1") != "0";
        return on;
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
    struct VitalChange {
        double life = 0, life_frac = 0, shields = 0, shields_frac = 0, energy = 0, energy_frac = 0;
        bool any = false;
    };
    VitalChange effect_vital_change(const std::string& effect_id) const {
        VitalChange v;
        const Entry* e = find("CEffect", effect_id);
        if (!e || e->tag != "CEffectModifyUnit") return v;
        auto set = [&](const std::string& vital, const std::string& field, const std::string& val) {
            if (val.empty()) return;
            const double d = std::strtod(val.c_str(), nullptr);
            double* flat = vital == "Life" ? &v.life : vital == "Shields" ? &v.shields : vital == "Energy" ? &v.energy : nullptr;
            double* frac = vital == "Life" ? &v.life_frac : vital == "Shields" ? &v.shields_frac
                         : vital == "Energy" ? &v.energy_frac : nullptr;
            if (!flat) return;
            if (field == "Change") *flat = d;
            else if (field == "ChangeFraction") *frac = d;
            else return;
            v.any = v.any || d != 0;
        };
        for (const core::XmlNode& n : resolved("CEffect", effect_id)) {
            if (n.name != "VitalArray") continue;
            const std::string vital = n.attr_or("index", "");
            for (const char* f : {"Change", "ChangeFraction"})
                if (const std::string* a = n.attr(f)) set(vital, f, *a);
            for (const core::XmlNode& c : n.children) set(vital, c.name, c.attr_or("value", ""));
        }
        return v;
    }
    std::optional<VitalChange> effect_energy_vital_change(const std::string& effect_id) const;

    struct BehaviorBuff {
        double duration = 0;
        bool duration_authored = false;
        double move_speed_mult = 1.0;
        double rate_mult = 1.0;
        double attack_speed_mult = 1.0;
        bool root = false;
        bool stun = false;
        double dmg_per_tick = 0;
        double dmg_period = 0;
        bool control_transfer = false;
        bool suppress_attack = false;
        bool invulnerable = false;
        bool uncommandable = false;
        bool untargetable = false;
        bool suppress_cloak = false, benign = false, passive = false;
        bool cloak = false;
        bool disable_abils = false;
        bool disable_production = false, disable_spawner = false;
        bool disable_morph = false, disable_transport = false;
        double weapon_range_bonus = 0.0;
        double ranged_weapon_range_bonus = 0.0;
        bool no_detect = false;
        double move_speed_max = -1.0;
        double decel_mult = 1.0;
        double accel_mult = 1.0;
        std::vector<std::pair<std::string, double>> damage_attr_bonus;
        bool suppress_collision = false;
        bool suppress_passenger = false;
        std::vector<std::string> disabled_weapons;
        std::vector<std::string> disabled_abils;
        double life_armor_bonus = 0.0, shield_armor_bonus = 0.0;
        int max_stack = 1;
        int max_stack_caster = 0;
        bool refresh_stack = false;
        std::string initial_effect, periodic_effect, expire_effect, final_effect;
        double food = 0.0, life_max_bonus = 0.0;
        bool no_life_regen = false, no_shield_regen = false, no_energy_regen = false;
        double weapon_scan_bonus = 0.0;
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
        std::uint8_t kinds = 15;
        bool present = false;
        std::string handled;
        bool fatal = false;
    };
    DamageResponse behavior_damage_response(const std::string& behavior_id) const;

    std::vector<std::string> effect_apply_behaviors(const std::string& effect_id) const {
        std::vector<std::string> out;
        auto add = [&](const std::string& eff) {
            if (auto b = effect_apply_behavior(eff); b && !b->empty() &&
                std::find(out.begin(), out.end(), *b) == out.end())
                out.push_back(*b);
        };
        const Entry* e = find("CEffect", effect_id);
        if (e && e->tag == "CEffectSet")
            for (const std::string& member : array_links("CEffect", effect_id, "EffectArray")) add(member);
        else
            add(effect_id);
        return out;
    }

    bool behavior_no_damage_dealt(const std::string& behavior_id) const {
        bool saw = false;
        for (const core::XmlNode& n : resolved("CBehavior", behavior_id)) {
            if (n.name != "Modification") continue;
            for (const core::XmlNode& m : n.children) {
                if (m.name != "DamageDealtMaximum") continue;
                saw = true;
                if (m.attr_or("value", "0") != "0") return false;
            }
        }
        return saw;
    }

    std::optional<double> effect_apply_duration(const std::string& effect_id) const {
        auto beh = effect_apply_behavior(effect_id);
        if (!beh) return std::nullopt;
        if (effect_subclass(effect_id) == "ApplyBehavior") {
            bool use = false;
            for (const core::XmlNode& n : resolved("CEffect", effect_id))
                if (n.name == "Flags" && n.attr_or("index", "") == "UseDuration")
                    use = n.attr_or("value", "1") != "0";
            if (use)
                if (auto d = scalar_num("CEffect", effect_id, "Duration")) return *d;
        }
        auto bb = behavior_buff(*beh);
        return bb ? std::optional<double>(bb->duration) : std::nullopt;
    }

    std::optional<std::string> effect_apply_behavior(const std::string& effect_id) const {
        return effect_apply_behavior(effect_id, 0);
    }

    std::optional<std::string> effect_movement_slow_behavior(const std::string& effect_id) const;

    struct PlacedObstacleSpec {
        std::string unit;
        double radius = 0;
        double duration = 0;
        double energy = 0;
        bool ok() const { return !unit.empty() && radius > 0 && duration > 0; }
    };
    std::optional<PlacedObstacleSpec> placed_obstacle_spec(const std::string& abil_id) const {
        std::string placed;
        for (const core::XmlNode& n : resolved("CAbil", abil_id))
            if (n.name == "PlaceUnit") {
                const std::string v = n.attr_or("value", "");
                if (!v.empty()) placed = v;
            }
        if (placed.empty()) return std::nullopt;
        auto flag_set = [&](const char* name) {
            bool on = false;
            for (const core::XmlNode& n : resolved("CUnit", placed))
                if (n.name == "FlagArray" && n.attr_or("index", "") == name)
                    on = n.attr_or("value", "") == "1";
            return on;
        };
        if (!flag_set("Unselectable") || !flag_set("Untargetable") || !flag_set("Uncommandable"))
            return std::nullopt;
        bool ground = false;
        for (const core::XmlNode& n : resolved("CUnit", placed))
            if (n.name == "PlaneArray" && n.attr_or("index", "") == "Ground")
                ground = n.attr_or("value", "") == "1";
        if (!ground) return std::nullopt;

        PlacedObstacleSpec sp;
        sp.unit = placed;
        sp.radius = scalar_num("CUnit", placed, "Radius").value_or(0.0);
        if (auto cu = effect_create_unit(abil_id)) {
            if (!cu->spawn_effect.empty())
                if (auto beh = scalar("CEffect", cu->spawn_effect, "Behavior"))
                    sp.duration = scalar_num("CBehavior", *beh, "Duration").value_or(0.0);
        }
        for (const core::XmlNode& c : resolved("CAbil", abil_id)) {
            if (c.name != "Cost") continue;
            for (const core::XmlNode& g : c.children)
                if (g.name == "Vital" && g.attr_or("index", "") == "Energy") {
                    const std::string v = g.attr_or("value", "");
                    if (!v.empty()) sp.energy = std::atof(v.c_str());
                }
        }
        return sp;
    }

    enum class SpawnOwner { Caster, Neutral, Target, Source };
    struct CreateUnitSpec {
        std::string unit;
        int count = 1;
        std::string spawn_effect;
        double spawn_range = 0;
        SpawnOwner spawn_owner = SpawnOwner::Caster;
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
        std::string ability;
        std::string command;
        std::string queue_ability;
        int queue_size = 0;
        int queue_count = 0;
        int minerals = 0;
        int gas = 0;
        bool auto_build = false;
        bool auto_build_on = false;
        bool show_status = false;
        std::string activity;
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
        std::uint8_t affiliation = 0;
        bool excludes_structure = false;
    };
    std::optional<PersistentSpec> effect_persistent(const std::string& effect_id) const {
        return effect_persistent(effect_id, 0);
    }

    struct MorphSpec {
        std::string target;
        double morph_time = 0;
        std::vector<std::string> chain;
        std::string finish_effect;
        double order_time = 0;
        double stats_delay = 0;
        double collide_delay = 0;
        bool transient = false;
        struct Stage {
            std::string unit;
            double type_at = 0;
            double vitals_at = 0;
        };
        std::vector<Stage> stages;
        double random_delay_min = 0;
        double random_delay_max = 0;
        std::vector<std::string> behavior_on;
        std::vector<std::string> behavior_off;
    };
    std::optional<MorphSpec> morph_spec(const std::string& abil_id) const;
    using MorphFieldFold = std::function<double(const std::string& field, double base)>;
    std::optional<MorphSpec> morph_spec(const std::string& abil_id, const MorphFieldFold& fold) const;

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
        std::vector<std::pair<double, double>> offsets;
        std::pair<double, double> center{0, 0};
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

    std::optional<std::string> unit_wireframe_image(const std::string& unit_id) const;

    struct ActorRule {
        std::string head, subject, sub;
        std::vector<std::pair<std::string, std::string>> filters;
        std::string verb;
        std::vector<std::string> args;
        std::string target;
    };

    std::vector<ActorRule> actor_rules(const std::string& actor_id,
                                       const std::string& family = "CActorUnit", bool with_macros = true) const;

    struct ActorSubscription {
        std::string actor;
        std::string family;
        std::vector<std::pair<std::string, std::string>> filters;
    };
    const std::vector<ActorSubscription>& actors_on(const std::string& head, const std::string& subject,
                                                    const std::string& sub) const;

    struct MorphTransition {
        std::string actor;
        std::string model;
        std::string slot;
        std::vector<std::string> props;
        bool play_forever = false;
    };
    std::optional<MorphTransition> morph_transition(const std::string& to_unit) const;
    std::pair<double, double> morph_actor_section(const std::string& abil_id) const;
    std::string ability_point_marker(const std::string& abil_id) const;

    std::optional<std::array<int, 3>> ability_rally_color(const std::string& ability_id) const {
        if (ability_id.empty()) return std::nullopt;
        for (const core::XmlNode& n : resolved("CAbil", ability_id)) {
            if (n.name != "OrderArray") continue;
            std::string v = n.attr_or("Color", "");
            if (v.empty())
                for (const core::XmlNode& ch : n.children)
                    if (ch.name == "Color") { v = ch.attr_or("value", ""); break; }
            if (v.empty()) continue;
            int comp[4] = {255, 200, 200, 200};
            std::size_t i = 0;
            for (int k = 0; k < 4 && i <= v.size(); ++k) {
                const std::size_t c = v.find(',', i);
                try {
                    comp[k] = std::stoi(v.substr(i, c == std::string::npos ? c : c - i));
                } catch (...) {   }
                if (c == std::string::npos) break;
                i = c + 1;
            }
            return std::array<int, 3>{comp[1], comp[2], comp[3]};
        }
        return std::nullopt;
    }

    std::string unit_rally_ability(const std::string& unit_id) const {
        if (unit_id.empty()) return {};
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            const Entry* e = find("CAbil", ab);
            if (e && e->tag == "CAbilRally") return ab;
        }
        return {};
    }

    std::array<float, 3> unit_render_scale(const std::string& unit_id) const;

    std::optional<int> cargo_size(const std::string& unit_id) const {
        if (auto v = scalar_num("CUnit", unit_id, "CargoSize")) return int(*v);
        return std::nullopt;
    }

    std::string unit_subgroup_alias(const std::string& unit_id) const {
        auto a = scalar("CUnit", unit_id, "SubgroupAlias");
        return a && !a->empty() ? *a : unit_id;
    }
    std::string unit_select_alias(const std::string& unit_id) const {
        auto a = scalar("CUnit", unit_id, "SelectAlias");
        return a && !a->empty() ? *a : unit_id;
    }
    std::optional<int> unit_subgroup_priority(const std::string& unit_id) const {
        if (auto v = scalar_num("CUnit", unit_id, "SubgroupPriority")) return int(*v);
        return std::nullopt;
    }

    double watchtower_capture_range() const {
        return scalar_num("CAbil", "TowerCapture", "Range").value_or(0.0);
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
        double best = 0;
        for (const auto& [dx, dy] : footprint_place_offsets(fp_id)) best = std::max(best, std::sqrt(double(dx * dx + dy * dy)));
        return best;
    }

    double creep_source_radius(const std::string& unit_id) const {
        double best = 0;
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
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

    std::string creep_source_footprint(const std::string& unit_id, const char* field) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            const Entry* e = beh.empty() ? nullptr : find("CBehavior", beh);
            if (e && e->tag == "CBehaviorCreepSource") return scalar("CBehavior", beh, field).value_or("");
        }
        return {};
    }

    std::string unit_toggled_creep_source(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray"))
            for (const std::string& beh : array_links("CAbil", ab, "BehaviorArray")) {
                if (beh.empty()) continue;
                const Entry* e = find("CBehavior", beh);
                if (e && e->tag == "CBehaviorCreepSource") return beh;
            }
        return {};
    }

    double toggled_creep_radius(const std::string& unit_id) const {
        const std::string beh = unit_toggled_creep_source(unit_id);
        if (beh.empty()) return 0;
        auto fp = scalar("CBehavior", beh, "Grown");
        return fp ? footprint_place_radius(*fp) : 0;
    }
    double toggled_creep_period(const std::string& unit_id) const {
        const std::string beh = unit_toggled_creep_source(unit_id);
        if (beh.empty()) return 0;
        return scalar_num("CBehavior", beh, "Period").value_or(0.0);
    }
    std::vector<std::pair<int, int>> toggled_creep_footprint_offsets(const std::string& unit_id) const {
        const std::string beh = unit_toggled_creep_source(unit_id);
        if (beh.empty()) return {};
        auto fp = scalar("CBehavior", beh, "Grown");
        return fp ? footprint_place_offsets(*fp) : std::vector<std::pair<int, int>>{};
    }

    static int footprint_row_y(int miny, int maxy, int r) {
        const int h = maxy - miny;
        return h > 0 ? miny + (h - 1 - r) : miny + r;
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
                    cells.emplace_back(minx + int(col), footprint_row_y(miny, maxy, r));
                }
                ++seq;
            }
            result = std::move(cells);
        }
        return result;
    }

    std::vector<std::pair<int, int>> creep_source_footprint_offsets(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
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
        return pathing_footprint_offsets(*fp);
    }

    std::vector<std::pair<int, int>> doodad_pathing_footprint_offsets(const std::string& doodad_type) const {
        auto fp = scalar("CActorDoodad", doodad_type, "Footprint");
        if (!fp) return {};
        if (footprint_has_layer(*fp, "Pathing")) return pathing_footprint_offsets(*fp, "Pathing");
        return pathing_footprint_offsets(*fp);
    }

    bool footprint_has_layer(const std::string& fp_id, const char* layer_name) const {
        for (const core::XmlNode& L : resolved("CFootprint", fp_id))
            if (L.name == "Layers" && L.attr_or("index", "") == layer_name) return true;
        return false;
    }

    std::vector<std::vector<std::pair<double, double>>> footprint_shape_rings(
        const std::string& fp_id, std::vector<int>* border_weights_out = nullptr,
        double* shape_radius_out = nullptr) const {
        if (border_weights_out) border_weights_out->clear();
        if (shape_radius_out) *shape_radius_out = 0;
        std::vector<std::pair<double, double>> verts;
        std::string borders;
        for (const core::XmlNode& n : resolved("CFootprint", fp_id)) {
            if (n.name != "Shape") continue;
            for (const core::XmlNode& c : n.children) {
                const std::string* v = c.attr("value");
                const std::string& s = v ? *v : c.text;
                if (s.empty()) continue;
                if (c.name == "Radius") {
                    if (shape_radius_out) *shape_radius_out = std::strtod(s.c_str(), nullptr);
                } else if (c.name == "Offsets") {
                    std::vector<std::pair<double, double>> vs;
                    const char* p = s.c_str();
                    while (*p) {
                        char* end = nullptr;
                        const double x = std::strtod(p, &end);
                        if (end == p) break;
                        p = end;
                        while (*p == ',' || *p == ' ') ++p;
                        const double y = std::strtod(p, &end);
                        if (end == p) break;
                        p = end;
                        vs.emplace_back(x, y);
                        while (*p == ';' || *p == ' ') ++p;
                    }
                    if (!vs.empty()) verts = std::move(vs);
                } else if (c.name == "Borders") {
                    borders = s;
                }
            }
        }
        std::vector<int> next(verts.size(), -1);
        if (!borders.empty()) {
            const char* p = borders.c_str();
            while (*p) {
                char* end = nullptr;
                const long a = std::strtol(p, &end, 10);
                if (end == p) break;
                p = end;
                while (*p == ',' || *p == ' ') ++p;
                const long b = std::strtol(p, &end, 10);
                if (end == p) break;
                p = end;
                while (*p == ',' || *p == ' ') ++p;
                const long w = std::strtol(p, &end, 10);
                if (end != p) {
                    p = end;
                    if (border_weights_out) border_weights_out->push_back(int(w));
                }
                while (*p && *p != ';') ++p;
                while (*p == ';' || *p == ' ') ++p;
                if (a >= 0 && a < long(verts.size()) && b >= 0 && b < long(verts.size()))
                    next[std::size_t(a)] = int(b);
            }
        }
        if (verts.size() < 3) return {};
        std::vector<std::vector<std::pair<double, double>>> rings;
        const bool have_edges =
            std::find_if(next.begin(), next.end(), [](int n) { return n >= 0; }) != next.end();
        if (!have_edges) {
            rings.push_back(verts);
            return rings;
        }
        std::vector<char> seen(verts.size(), 0);
        for (std::size_t start = 0; start < verts.size(); ++start) {
            if (seen[start] || next[start] < 0) continue;
            std::vector<std::pair<double, double>> ring;
            std::size_t cur = start;
            for (std::size_t guard = 0; guard <= verts.size(); ++guard) {
                if (seen[cur]) break;
                seen[cur] = 1;
                ring.push_back(verts[cur]);
                const int nx = next[cur];
                if (nx < 0) break;
                cur = std::size_t(nx);
            }
            if (ring.size() >= 3) rings.push_back(std::move(ring));
        }
        return rings;
    }

    bool footprint_has_shape(const std::string& fp_id) const {
        return !footprint_shape_rings(fp_id).empty();
    }

    double footprint_shape_radius(const std::string& fp_id) const {
        double r = 0;
        footprint_shape_rings(fp_id, nullptr, &r);
        return r;
    }

    double unit_body_radius(const std::string& unit_id, double authored_radius) const {
        const std::optional<std::string> fp = scalar("CUnit", unit_id, "Footprint");
        if (!fp) return authored_radius;
        const double contour = footprint_shape_radius(*fp);
        if (contour <= 0) return authored_radius;
        const double quantised = std::ceil(contour * 16.0 - 1e-9) / 16.0;
        return quantised > authored_radius ? quantised : authored_radius;
    }

    int footprint_min_border_weight(const std::string& fp_id) const {
        std::vector<int> w;
        footprint_shape_rings(fp_id, &w);
        if (w.empty()) return -1;
        return *std::min_element(w.begin(), w.end());
    }

    bool unit_placement_grid_phase(const std::string& unit_id, double& fx, double& fy) const {
        const auto fp = scalar("CUnit", unit_id, "PlacementFootprint");
        if (!fp || fp->empty()) return false;
        double rx = 0.5, ry = 0.5;
        footprint_grid_phase(*fp, 0.0, 0.0, 0.0, 0.0, rx, ry);
        auto to_world = [](double f) {
            const double w = 0.5 - f;
            return w - std::floor(w);
        };
        fx = to_world(rx);
        fy = to_world(ry);
        return true;
    }

    void footprint_grid_phase(const std::string& fp_id, double minx, double miny, double maxx,
                              double maxy, double& fx, double& fy) const {
        auto wrap = [](double f) {
            f -= std::floor(f);
            return f;
        };
        for (const char* layer_name : {"Place", "Pathing", "Check"}) {
            bool have = false, have_area = false;
            int bx = 0, by = 0, maxcols = 0, nrows = 0;
            for (const core::XmlNode& layer : resolved("CFootprint", fp_id)) {
                if (layer.name != "Layers" || layer.attr_or("index", "") != layer_name) continue;
                int seq = 0, maxrow = -1;
                for (const core::XmlNode& row : layer.children) {
                    if (row.name != "Rows") continue;
                    const std::string* v = row.attr("value");
                    const std::string& bits = v ? *v : row.text;
                    if (int(bits.size()) > maxcols) maxcols = int(bits.size());
                    const std::string* ri = row.attr("index");
                    const int r = ri ? std::atoi(ri->c_str()) : seq;
                    if (r > maxrow) maxrow = r;
                    ++seq;
                }
                if (seq == 0) continue;
                have = true;
                if (maxrow + 1 > nrows) nrows = maxrow + 1;
                int ax, ay, aw, ah;
                const std::string* area = layer.attr("Area");
                if (!have_area && area &&
                    std::sscanf(area->c_str(), "%d,%d,%d,%d", &ax, &ay, &aw, &ah) == 4) {
                    have_area = true;
                    bx = ax;
                    by = ay;
                }
            }
            if (!have || maxcols == 0) continue;
            if (!have_area) {
                bx = -(maxcols / 2);
                by = -(nrows / 2);
            }
            fx = wrap(0.5 - double(bx) - double(maxcols) / 2.0);
            fy = wrap(0.5 - double(by) - double(nrows) / 2.0);
            return;
        }
        auto bbox_phase = [&](double m) {
            const double fr = m - std::floor(m);
            return std::fabs(fr - 0.5) < 1e-6 ? 0.0 : 0.5;
        };
        (void)maxx;
        (void)maxy;
        fx = bbox_phase(minx);
        fy = bbox_phase(miny);
    }

    std::vector<std::pair<int, int>> footprint_shape_offsets(const std::string& fp_id) const {
        const auto rings = footprint_shape_rings(fp_id);
        if (rings.empty()) return {};
        double minx = 1e30, miny = 1e30, maxx = -1e30, maxy = -1e30;
        for (const auto& ring : rings)
            for (const auto& v : ring) {
                minx = std::min(minx, v.first);
                maxx = std::max(maxx, v.first);
                miny = std::min(miny, v.second);
                maxy = std::max(maxy, v.second);
            }
        double fx = 0.5, fy = 0.5;
        footprint_grid_phase(fp_id, minx, miny, maxx, maxy, fx, fy);
        auto inside = [&](double px, double py) {
            bool in = false;
            for (const auto& ring : rings)
                for (std::size_t i = 0, j = ring.size() - 1; i < ring.size(); j = i++) {
                    const double xi = ring[i].first, yi = ring[i].second;
                    const double xj = ring[j].first, yj = ring[j].second;
                    const double cross = (xj - xi) * (py - yi) - (yj - yi) * (px - xi);
                    if (std::fabs(cross) < 1e-9 && px >= std::min(xi, xj) - 1e-9 &&
                        px <= std::max(xi, xj) + 1e-9 && py >= std::min(yi, yj) - 1e-9 &&
                        py <= std::max(yi, yj) + 1e-9)
                        return true;
                    if ((yi > py) != (yj > py) && px < (xj - xi) * (py - yi) / (yj - yi) + xi) in = !in;
                }
            return in;
        };
        std::vector<std::pair<int, int>> cells;
        const int y0 = int(std::floor(miny - fy)), y1 = int(std::ceil(maxy - fy));
        const int x0 = int(std::floor(minx - fx)), x1 = int(std::ceil(maxx - fx));
        for (int dy = y0; dy <= y1; ++dy)
            for (int dx = x0; dx <= x1; ++dx)
                if (inside(double(dx) + fx, double(dy) + fy)) cells.emplace_back(dx, dy);

        return cells;
    }

    bool footprint_shape_bitmap_disjoint(const std::string& fp_id) const {
        const std::vector<std::pair<int, int>> poly = footprint_shape_offsets(fp_id);
        const std::vector<std::pair<int, int>> bmp = pathing_footprint_offsets(fp_id);
        if (poly.empty() || bmp.empty()) return false;
        for (const std::pair<int, int>& c : poly)
            if (std::find(bmp.begin(), bmp.end(), c) != bmp.end()) return false;
        return true;
    }

    std::vector<std::pair<int, int>> footprint_shape_occupancy(const std::string& fp_id) const {
        const std::vector<std::pair<int, int>> poly = footprint_shape_offsets(fp_id);
        const std::vector<std::pair<int, int>> bmp = pathing_footprint_offsets(fp_id);
        if (bmp.empty() || poly.empty()) return poly;
        std::vector<std::pair<int, int>> clamped;
        clamped.reserve(poly.size());
        for (const std::pair<int, int>& c : poly)
            if (std::find(bmp.begin(), bmp.end(), c) != bmp.end()) clamped.push_back(c);
        if (clamped.empty()) return bmp;
        return clamped;
    }

    std::vector<std::pair<int, int>> doodad_shape_footprint_offsets(
        const std::string& doodad_type) const {
        auto fp = scalar("CActorDoodad", doodad_type, "Footprint");
        if (!fp) return {};
        return footprint_body_offsets(*fp,  false);
    }

    std::vector<std::pair<int, int>> doodad_nav_footprint_offsets(const std::string& doodad_type) const {
        auto fp = scalar("CActorDoodad", doodad_type, "Footprint");
        if (!fp) return {};
        return footprint_body_offsets(*fp,  true);
    }

    std::vector<std::pair<int, int>> footprint_body_offsets(const std::string& fp_id, bool nav) const {
        if (footprint_has_layer(fp_id, "Pathing")) return pathing_footprint_offsets(fp_id, "Pathing");
        if (footprint_has_shape(fp_id))
            return nav ? footprint_shape_offsets(fp_id) : footprint_shape_occupancy(fp_id);
        return pathing_footprint_offsets(fp_id);
    }

    std::vector<std::pair<int, int>> unit_shape_footprint_offsets(const std::string& unit_id) const {
        auto fp = scalar("CUnit", unit_id, "Footprint");
        if (!fp) return {};
        if (footprint_min_border_weight(*fp) == 1) return {};
        return footprint_body_offsets(*fp,  false);
    }

    std::vector<std::pair<int, int>> unit_nav_footprint_offsets(const std::string& unit_id) const {
        auto fp = scalar("CUnit", unit_id, "Footprint");
        if (!fp) return {};
        if (footprint_min_border_weight(*fp) == 1) return {};
        return footprint_body_offsets(*fp,  true);
    }

    std::vector<std::pair<int, int>> pathing_footprint_offsets(const std::string& footprint_id,
                                                              const char* layer_name = "Place",
                                                              bool include_aux = false,
                                                              const std::string* only_chars = nullptr) const {
        const std::string* fp = &footprint_id;
        std::vector<std::pair<int, int>> result;
        auto has = [&](int x, int y) {
            for (const auto& c : result)
                if (c.first == x && c.second == y) return true;
            return false;
        };
        std::string aux;
        for (const core::XmlNode& layer : resolved("CFootprint", *fp)) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != layer_name) continue;
            for (const core::XmlNode& s : layer.children)
                if (s.name == "Sets")
                    if (const std::string* c = s.attr("Character"); c && !c->empty())
                        if (aux.find((*c)[0]) == std::string::npos) aux.push_back((*c)[0]);
        }
        for (const core::XmlNode& layer : resolved("CFootprint", *fp)) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != layer_name) continue;
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
            int minx, miny, maxy, mx, my, mw, mh;
            const std::string* area = layer.attr("Area");
            if (area && std::sscanf(area->c_str(), "%d,%d,%d,%d", &mx, &my, &mw, &mh) == 4) {
                minx = mx;
                miny = my;
                maxy = mh;
            } else {
                minx = -(maxcols / 2);
                miny = -(seq / 2);
                maxy = miny + seq;
            }
            for (const auto& [bits, r] : rows)
                for (std::size_t col = 0; col < bits.size(); ++col) {
                    const char ch = bits[col];
                    if (ch == '.' || ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') continue;
                    if (only_chars ? only_chars->find(ch) == std::string::npos
                                   : (!include_aux && aux.find(ch) != std::string::npos))
                        continue;
                    const int x = minx + int(col), y = footprint_row_y(miny, maxy, r);
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

    std::string unburrow_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilMorph") continue;
            if (auto k = scalar("CAbil", ab, "ActorKey"); k && *k == "BurrowUp") return ab;
        }
        return {};
    }

    std::string burrowed_variant(const std::string& unit_id) const {
        const std::string ab = burrow_ability(unit_id);
        if (ab.empty()) return {};
        if (auto ms = morph_spec(ab)) return ms->target;
        return {};
    }

    struct LiftSpec {
        double lock = 0, rise_delay = 0, rise = 0, actor = 0;
    };
    std::optional<LiftSpec> liftoff_spec(const std::string& abil_id) const {
        const Entry* e = find("CAbil", abil_id);
        if (!e || (e->tag != "CAbilMorph" && e->tag != "CAbilMorphPlacement")) return std::nullopt;
        LiftSpec s;
        bool any = false;
        for (const core::XmlNode& info : resolved("CAbil", abil_id)) {
            if (info.name != "InfoArray") continue;
            for (const core::XmlNode& sec : info.children) {
                if (sec.name != "SectionArray") continue;
                const std::string sname = sec.attr_or("index", "");
                double delay = 0, dur = 0;
                for (const core::XmlNode& d : sec.children) {
                    if (d.name != "DurationArray") continue;
                    const std::string* v = d.attr("value");
                    const double val = v ? std::atof(v->c_str()) : 0.0;
                    const std::string idx = d.attr_or("index", "");
                    if (idx == "Delay") delay = val;
                    else if (idx == "Duration") dur = val;
                }
                any = true;
                if (sname == "Actor") {
                    s.actor = delay + dur;
                    continue;
                }
                if (sname == "Mover") {
                    s.rise_delay = delay;
                    s.rise = dur;
                }
                if (delay + dur > s.lock) s.lock = delay + dur;
            }
        }
        if (!any) return std::nullopt;
        return s;
    }
    std::optional<std::pair<float, float>> actor_host_forward(const std::string& actor_id,
                                                              const std::string& family = "CActorModel") const {
        for (const core::XmlNode& n : resolved(family, actor_id)) {
            if (n.name != "HostSiteOps") continue;
            const std::string ops = n.attr_or("Ops", "");
            std::size_t i = 0;
            while (i < ops.size()) {
                while (i < ops.size() && ops[i] == ' ') ++i;
                std::size_t j = i;
                while (j < ops.size() && ops[j] != ' ') ++j;
                const std::string op = ops.substr(i, j - i);
                i = j;
                if (op.empty() || !find("CActorSiteOpRotationExplicit", op)) continue;
                const std::string fwd = scalar("CActorSiteOpRotationExplicit", op, "Forward").value_or("");
                float fx = 0.0f, fy = 0.0f, fz = 0.0f;
                if (fwd.empty() || std::sscanf(fwd.c_str(), "%f,%f,%f", &fx, &fy, &fz) < 2) continue;
                if (fx == 0.0f && fy == 0.0f) continue;
                return std::make_pair(fx, fy);
            }
        }
        return std::nullopt;
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

    std::string land_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilMorphPlacement") continue;
            if (auto k = scalar("CAbil", ab, "ActorKey"); k && *k == "LiftOffLand") return ab;
        }
        return {};
    }

    std::string liftoff_target(const std::string& unit_id) const {
        const std::string ab = liftoff_ability(unit_id);
        if (ab.empty()) return {};
        for (const Entry* e = find("CAbil", ab); e;
             e = e->parent.empty() ? nullptr : find("CAbil", e->parent))
            if (const std::string* u = e->attr("unit"); u && !u->empty()) return *u;
        return {};
    }

    int race_food_ceiling(const std::string& race_id) const {
        if (auto v = scalar("CRace", race_id, "FoodCeiling")) return std::atoi(v->c_str());
        if (auto v = tag_default_scalar("CRace", "FoodCeiling")) return std::atoi(v->c_str());
        return 200;
    }
    int race_starting_resource(const std::string& race_id, const char* resource) const {
        int value = 0;
        for (const core::XmlNode& n : resolved("CRace", race_id))
            if (n.name == "StartingResource" && n.attr_or("index", "") == resource)
                value = std::atoi(n.attr_or("value", "0").c_str());
        return value;
    }
    struct StartSlot {
        std::string unit;
        int count = 1;
        bool resource_rally = false;
        std::vector<std::pair<int, int>> offsets;
    };
    std::vector<StartSlot> race_start_roster(const std::string& race_id) const {
        std::vector<StartSlot> slots;
        for (const core::XmlNode& n : resolved("CRace", race_id)) {
            if (n.name != "StartingUnitArray") continue;
            const std::string* ix = n.attr("index");
            std::size_t at = ix ? std::size_t(std::atoi(ix->c_str())) : slots.size();
            if (slots.size() <= at) slots.resize(at + 1);
            StartSlot& sl = slots[at];
            if (const std::string* u = n.attr("Unit")) sl.unit = *u;
            if (const std::string* c = n.attr("Count")) sl.count = std::atoi(c->c_str());
            std::size_t next = 0;
            for (const core::XmlNode& ch : n.children) {
                if (ch.name == "Flags" && ch.attr_or("index", "") == "ResourceRally")
                    sl.resource_rally = ch.attr_or("value", "0") == "1";
                if (ch.name != "Offset") continue;
                const std::string* oi = ch.attr("index");
                const std::size_t k = oi ? std::size_t(std::atoi(oi->c_str())) : next;
                next = k + 1;
                const std::string v = ch.attr_or("value", "");
                const std::size_t comma = v.find(',');
                if (comma == std::string::npos) continue;
                if (sl.offsets.size() <= k) sl.offsets.resize(k + 1);
                sl.offsets[k] = {std::atoi(v.substr(0, comma).c_str()), std::atoi(v.substr(comma + 1).c_str())};
            }
        }
        return slots;
    }
    int race_starting_unit_count(const std::string& race_id, const std::string& unit_id) const {
        struct Slot { std::string unit; int count = 1; };
        std::vector<Slot> slots;
        for (const core::XmlNode& n : resolved("CRace", race_id)) {
            if (n.name != "StartingUnitArray") continue;
            const std::string* ix = n.attr("index");
            std::size_t at = slots.size();
            if (ix) at = std::size_t(std::atoi(ix->c_str()));
            if (slots.size() <= at) slots.resize(at + 1);
            if (const std::string* u = n.attr("Unit")) slots[at].unit = *u;
            if (const std::string* c = n.attr("Count")) slots[at].count = std::atoi(c->c_str());
        }
        int total = 0;
        for (const Slot& sl : slots)
            if (sl.unit == unit_id) total += sl.count;
        return total;
    }
    bool unit_flag(const std::string& unit_id, const char* name) const {
        bool on = false;
        for (const core::XmlNode& n : resolved("CUnit", unit_id))
            if (n.name == "FlagArray" && n.attr_or("index", "") == name) on = n.attr_or("value", "") == "1";
        return on;
    }
    std::string depot_lower_ability(const std::string& unit_id) const {
        return morph_button_ability(unit_id, "Lower");
    }
    std::string morph_button_ability(const std::string& unit_id, const std::string& face) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || (e->tag != "CAbilMorph" && e->tag != "CAbilMorphPlacement")) continue;
            for (const core::XmlNode& n : resolved("CAbil", ab))
                if (n.name == "CmdButtonArray" && n.attr_or("index", "") == "Execute" &&
                    n.attr_or("DefaultButtonFace", "") == face)
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
        std::string caster_behavior;
        double caster_duration = 0;
        std::string cancel_behavior;
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
        PhaseShiftSpec spec;
        spec.shade = shade;
        for (const std::string& eid : walk_effect_set(root)) {
            const Entry* ee = find("CEffect", eid);
            if (!ee || ee->tag != "CEffectApplyBehavior") continue;
            std::string who;
            for (const core::XmlNode& k : resolved("CEffect", eid))
                if (k.name == "WhichUnit")
                    if (const std::string* v = k.attr("Value") ? k.attr("Value") : k.attr("value")) who = *v;
            if (who != "Caster") continue;
            spec.caster_behavior = scalar("CEffect", eid, "Behavior").value_or("");
            spec.caster_duration = scalar_num("CBehavior", spec.caster_behavior, "Duration").value_or(0);
            break;
        }
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
                if (!te || te->tag != "CEffectTeleport") continue;
                spec.found = true;
                spec.duration = *dur;
                for (const std::string& vid : array_links("CBehavior", beh, "RemoveValidatorArray")) {
                    const Entry* ve = find("CValidator", vid);
                    if (ve && ve->tag == "CValidatorUnitCompareBehaviorCount")
                        spec.cancel_behavior = scalar("CValidator", vid, "Behavior").value_or("");
                    if (!spec.cancel_behavior.empty()) break;
                }
                return spec;
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
    bool weapon_hidden(const std::string& id) const {
        bool set = false;
        for (const core::XmlNode& c : resolved("CWeapon", id))
            if (c.name == "Options" && c.attr_or("index", "") == "Hidden")
                set = c.attr_or("value", "0") != "0";
        return set;
    }

    std::vector<std::string> equipped_weapons(const std::string& unit_id,
                                              bool include_disabled = false) const {
        std::vector<std::string> out;
        for (const std::string& w : array_links("CUnit", unit_id, "WeaponArray"))
            if (!w.empty() && !weapon_hidden(w) && (include_disabled || !weapon_disabled(w)))
                out.push_back(w);
        if (out.empty() && !OMC_ENV_ONCE("OMC_HIDDEN_ONLY_WEAPON_OFF"))
            for (const std::string& w : array_links("CUnit", unit_id, "WeaponArray"))
                if (!w.empty() && weapon_hidden(w) && (include_disabled || !weapon_disabled(w)))
                    out.push_back(w);
        return out;
    }

    bool hidden_sibling_covers_min_range(const std::string& unit_id, const std::string& weapon) const {
        auto num = [&](const std::string& w, const char* f, double dflt) {
            const auto v = scalar("CWeapon", w, f);
            return v && !v->empty() ? std::atof(v->c_str()) : dflt;
        };
        auto planes = [&](const std::string& w) {
            const std::string tf = scalar("CWeapon", w, "TargetFilters").value_or("");
            const std::string req = tf.substr(0, tf.find(';'));
            if (req.empty()) return 3;
            return (req.find("Ground") != std::string::npos ? 1 : 0) | (req.find("Air") != std::string::npos ? 2 : 0);
        };
        const double min_r = num(weapon, "MinimumRange", 0.0);
        if (min_r <= 0.0) return false;
        for (const std::string& h : array_links("CUnit", unit_id, "WeaponArray"))
            if (!h.empty() && h != weapon && weapon_hidden(h) && !weapon_disabled(h) &&
                num(h, "MinimumRange", 0.0) <= 0.0 && num(h, "Range", 5.0) >= min_r && (planes(h) & planes(weapon)) != 0)
                return true;
        return false;
    }

    std::string unit_toggled_weapon(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray"))
            for (const std::string& beh : array_links("CAbil", ab, "BehaviorArray")) {
                const std::string w = behavior_weapon_enable(beh);
                if (!w.empty() && weapon_disabled(w)) return w;
            }
        return {};
    }

    bool weapon_disabled(const std::string& id) const {
        bool set = false;
        for (const core::XmlNode& c : resolved("CWeapon", id))
            if (c.name == "Options" && c.attr_or("index", "") == "Disabled")
                set = c.attr_or("value", "0") != "0";
        return set;
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
        const std::string root = cabil_effect_link(abil_id);
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
                    for (const std::string& c : array_links("CEffect", cur, "EffectArray"))
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

    struct AbilInfoRow {
        std::string index;
        std::string unit;
        bool unit_written = false;
        std::string upgrade;
        bool upgrade_written = false;
        bool upgrade_named = false;
        double time = 0;
        int minerals = 0;
        int gas = 0;
        std::string requirement;
        bool requirement_written = false;
        int unit_count = 1;
        double warp_charge = 0;
        bool is_gas = false;
        bool self_macro = false;
    };
    std::vector<AbilInfoRow> ability_info_rows(const std::string& abil_id) const;
    std::optional<AbilInfoRow> ability_info_row(const std::string& abil_id,
                                                const std::string& index) const;

    struct TrainProduct {
        std::string unit;
        std::string ability;
        std::string command;
        std::string face;
        int row = -1, col = -1;
        int card = 0;
        double time = 0;
        std::string requirement;
        double warp_charge = 0;
    };
    bool unit_carries_ability(const std::string& unit_id, const std::string& abil_id) const {
        const std::vector<std::string> own = array_links("CUnit", unit_id, "AbilArray");
        if (own.empty()) return true;
        return std::find(own.begin(), own.end(), abil_id) != own.end();
    }

    bool trained_by_spawner(const std::string& unit_id) const {
        if (!spawner_trained_built_) {
            for (const std::string& id : ids("CUnit"))
                if (unit_spawn_behavior(id))
                    for (const TrainProduct& tp : unit_train_products(id)) spawner_trained_.insert(tp.unit);
            spawner_trained_built_ = true;
        }
        return spawner_trained_.count(unit_id) != 0;
    }
    bool calldown_magazine(const std::string& ability_id) const {
        const Entry* e = find("CAbil", ability_id);
        return e && e->tag == "CAbilArmMagazine" && !scalar("CAbil", ability_id, "CalldownEffect").value_or("").empty();
    }
    struct RangeRing {
        double radius = 0;
        std::string source;
        bool on_placement = false;
        bool on_selection = false;
    };
    const std::vector<RangeRing>& unit_range_rings(const std::string& unit_id) const {
        build_range_rings();
        static const std::vector<RangeRing> kNone;
        auto it = range_rings_.find(unit_id);
        return it == range_rings_.end() ? kNone : it->second;
    }
    std::optional<double> ability_range_ring(const std::string& abil_id) const {
        build_range_rings();
        auto it = abil_rings_.find(abil_id);
        if (it == abil_rings_.end()) return std::nullopt;
        return it->second;
    }
    bool calldown_ammo_unit(const std::string& unit_id) const {
        if (!calldown_ammo_built_) {
            for (const std::string& id : ids("CAbil"))
                if (calldown_magazine(id))
                    for (const AbilInfoRow& r : ability_info_rows(id))
                        if (!r.self_macro && !r.unit.empty()) calldown_ammo_.insert(r.unit);
            calldown_ammo_built_ = true;
        }
        return calldown_ammo_.count(unit_id) != 0;
    }
    std::vector<TrainProduct> unit_train_products(const std::string& producer_id) const {
        std::vector<TrainProduct> out;
        for (const AbilityCardButton& b : unit_ability_card_buttons(producer_id)) {
            const Entry* e = find("CAbil", b.ability);
            if (!e || (e->tag != "CAbilTrain" && !calldown_magazine(b.ability))) continue;
            if (b.command.empty()) continue;
            if (!unit_carries_ability(producer_id, b.ability)) continue;
            const auto row = ability_info_row(b.ability, b.command);
            if (!row || row->unit.empty()) continue;
            TrainProduct p;
            p.ability = b.ability;
            p.command = b.command;
            p.face = b.face;
            p.row = b.row;
            p.col = b.col;
            p.card = b.card;
            p.unit = row->unit;
            p.time = row->time;
            p.requirement = button_requirement(b.ability, b.command);
            out.push_back(std::move(p));
        }
        return out;
    }

    std::vector<TrainProduct> unit_warp_products(const std::string& producer_id) const {
        std::vector<TrainProduct> out;
        for (const AbilityCardButton& b : unit_ability_card_buttons(producer_id)) {
            const Entry* e = find("CAbil", b.ability);
            if (!e || e->tag != "CAbilWarpTrain") continue;
            if (b.command.empty()) continue;
            if (!unit_carries_ability(producer_id, b.ability)) continue;
            const auto row = ability_info_row(b.ability, b.command);
            if (!row || row->unit.empty()) continue;
            TrainProduct p;
            p.ability = b.ability;
            p.command = b.command;
            p.face = b.face;
            p.row = b.row;
            p.col = b.col;
            p.card = b.card;
            p.unit = row->unit;
            p.time = row->time;
            p.warp_charge = row->warp_charge;
            p.requirement = button_requirement(b.ability, b.command);
            out.push_back(std::move(p));
        }
        return out;
    }

    std::string morph_finish_behavior(const std::string& abil_id) const {
        const Entry* e = find("CAbil", abil_id);
        if (!e || e->tag != "CAbilMorph") return {};
        std::string effect;
        for (const core::XmlNode& info : resolved("CAbil", abil_id)) {
            if (info.name != "InfoArray") continue;
            for (const core::XmlNode& sec : info.children) {
                if (sec.name != "SectionArray" || sec.attr_or("index", "") != "Abils") continue;
                for (const core::XmlNode& ea : sec.children)
                    if (ea.name == "EffectArray" && ea.attr_or("index", "") == "Finish")
                        if (const std::string* v = ea.attr("value"))
                            if (!v->empty()) effect = *v;
            }
        }
        if (effect.empty()) return {};
        if (effect_subclass(effect) != "ApplyBehavior") return {};
        std::string beh = scalar("CEffect", effect, "Behavior").value_or("");
        if (beh.empty()) beh = effect;
        return find("CBehavior", beh) ? beh : std::string{};
    }

    struct MorphAbilityCost {
        bool found = false;
        int minerals = 0;
        int gas = 0;
    };
    MorphAbilityCost morph_ability_cost(const std::string& abil_id) const {
        MorphAbilityCost out;
        for (const core::XmlNode& c : resolved("CAbil", abil_id)) {
            if (c.name != "Cost") continue;
            for (const core::XmlNode& r : c.children) {
                if (r.name != "Resource") continue;
                const std::string idx = r.attr_or("index", "");
                const std::string* v = r.attr("value");
                if (!v) continue;
                const int n = std::atoi(v->c_str());
                if (n < 0) continue;
                if (idx == "Minerals") { out.minerals = n; out.found = true; }
                else if (idx == "Vespene") { out.gas = n; out.found = true; }
            }
        }
        return out;
    }

    bool behavior_is_permanent(const std::string& beh_id) const {
        for (const core::XmlNode& n : resolved("CBehavior", beh_id))
            if (n.name == "BehaviorFlags" && n.attr_or("index", "") == "Permanent")
                return n.attr_or("value", "") == "1";
        return false;
    }

    std::string warpable_power_behavior(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilWarpable") continue;
            if (auto pu = scalar("CAbil", ab, "PowerUserBehavior")) return *pu;
        }
        return {};
    }

    struct BuildProduct {
        std::string unit;
        std::string ability;
        std::string command;
        std::string face;
        int row = -1, col = -1;
        int card = 0;
        double time = 0;
        bool is_gas = false;
        bool instant_placement = false;
        std::string requirement;
    };
    std::vector<BuildProduct> unit_build_products(const std::string& worker_id) const {
        std::vector<BuildProduct> out;
        for (const AbilityCardButton& b : unit_ability_card_buttons(worker_id)) {
            const Entry* e = find("CAbil", b.ability);
            if (!e || e->tag != "CAbilBuild") continue;
            if (b.command.empty()) continue;
            const auto row = ability_info_row(b.ability, b.command);
            if (!row || !row->unit_written || row->unit.empty()) continue;
            BuildProduct p;
            p.ability = b.ability;
            p.command = b.command;
            p.face = b.face;
            p.row = b.row;
            p.col = b.col;
            p.card = b.card;
            p.unit = row->unit;
            p.time = row->time;
            p.is_gas = row->is_gas;
            p.instant_placement = ability_flag(b.ability, "InstantPlacement");
            p.requirement = button_requirement(b.ability, b.command);
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
    std::vector<std::string> research_slots(const std::string& abil_id) const {
        const Entry* e = find("CAbil", abil_id);
        if (!e || e->tag != "CAbilResearch") return {};
        std::vector<std::string> out;
        for (const AbilInfoRow& r : ability_info_rows(abil_id))
            if (!r.upgrade.empty()) out.push_back(r.index);
        return out;
    }
    std::string research_slot_upgrade(const std::string& abil_id, const std::string& index) const {
        if (auto r = ability_info_row(abil_id, index)) return r->upgrade;
        return {};
    }
    double research_slot_time(const std::string& abil_id, const std::string& index) const {
        if (auto r = ability_info_row(abil_id, index)) return r->time;
        return 0;
    }
    float unit_facing_radians(const std::string& unit_id) const {
        constexpr double kSchemaDefaultDeg = 360.0;
        double deg = kSchemaDefaultDeg;
        if (auto s = scalar("CUnit", unit_id, "Facing"); s && !s->empty()) {
            try {
                deg = std::stod(*s);
            } catch (const std::exception&) {
                deg = kSchemaDefaultDeg;
            }
        }
        const double rad = (deg - 90.0) * 3.14159265358979323846 / 180.0;
        const double two_pi = 2.0 * 3.14159265358979323846;
        double w = std::fmod(rad, two_pi);
        if (w < 0) w += two_pi;
        return float(w);
    }

    ResearchInfo research_slot_info(const std::string& abil_id, const std::string& index) const {
        ResearchInfo out;
        const Entry* e = find("CAbil", abil_id);
        if (!e || e->tag != "CAbilResearch") return out;
        auto r = ability_info_row(abil_id, index);
        if (!r || r->upgrade.empty()) return out;
        out.found = true;
        out.minerals = r->minerals;
        out.gas = r->gas;
        out.time = r->time;
        return out;
    }

    ResearchInfo research_info(const std::string& upgrade_id) const {
        struct Candidate { std::string ability; std::string index; };
        std::vector<Candidate> candidates;
        std::vector<Candidate> carded;
        for (const std::string& ab : ids("CAbil")) {
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilResearch") continue;
            for (const AbilInfoRow& r : ability_info_rows(ab)) {
                if (upgrade_id.empty() || r.upgrade != upgrade_id) continue;
                candidates.push_back(Candidate{ab, r.index});
                if (carded_ability_commands().count(ab + "," + r.index))
                    carded.push_back(Candidate{ab, r.index});
            }
        }
        const Candidate* pick = nullptr;
        if (carded.size() == 1) pick = &carded[0];
        else if (!candidates.empty()) pick = &candidates[0];
        if (pick) {
            const auto row = ability_info_row(pick->ability, pick->index);
            ResearchInfo out;
            out.found = true;
            out.minerals = row->minerals;
            out.gas = row->gas;
            out.time = row->time;
            return out;
        }
        for (const std::string& ab : ids("CAbil")) {
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilResearch") continue;
            for (const core::XmlNode& info : resolved("CAbil", ab)) {
                if (info.name != "InfoArray") continue;
                if (info.attr_or("Upgrade", "") == upgrade_id) {
                    ResearchInfo r;
                    r.found = true;
                    return r;
                }
                for (const core::XmlNode& ch : info.children)
                    if (ch.name == "Upgrade" && ch.attr_or("value", "") == upgrade_id) {
                        ResearchInfo r;
                        r.found = true;
                        return r;
                    }
            }
        }
        return {};
    }

    const std::set<std::string>& carded_ability_commands() const {
        if (!carded_cmds_built_) {
            carded_cmds_built_ = true;
            for (const std::string& unit : ids("CUnit"))
                for (const AbilityCardButton& b : unit_ability_card_buttons(unit))
                    carded_cmds_.insert(b.ability + "," + b.command);
        }
        return carded_cmds_;
    }

    std::optional<int> resource_ideal_harvesters(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            if (auto a2 = scalar_num("CBehavior", beh, "IdealHarvesterCount")) return int(*a2);
            return std::nullopt;
        }
        return std::nullopt;
    }

    std::optional<double> resource_return_delay(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            if (auto d = scalar_num("CBehavior", beh, "ReturnDelay")) return *d;
            return std::nullopt;
        }
        return std::nullopt;
    }

    int resource_harvest_amount(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            if (auto a = scalar_num("CBehavior", beh, "HarvestAmount")) return int(*a);
            return 5;
        }
        return 0;
    }

    std::optional<double> resource_harvest_time(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            return scalar_num("CBehavior", beh, "HarvestTime");
        }
        return std::nullopt;
    }

    int resource_content(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            if (auto a = scalar_num("CBehavior", beh, "Contents")) return int(*a);
            return 0;
        }
        return 0;
    }

    std::string resource_exhausted_alert(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorResource") continue;
            if (auto a = scalar("CBehavior", beh, "ExhaustedAlert"); a && !a->empty()) return *a;
            return {};
        }
        return {};
    }

    std::string transport_ability(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (e && e->tag == "CAbilTransport") return ab;
        }
        return {};
    }

    struct LoadRule {
        int require_race = -1;
        std::vector<std::string> require_types;
        std::vector<std::string> exclude_types;
        std::vector<std::string> block_behaviors;
        int unknown = 0;
        bool empty() const {
            return require_race < 0 && require_types.empty() && exclude_types.empty() &&
                   block_behaviors.empty();
        }
    };
    LoadRule transport_load_rule(const std::string& unit_id) const {
        LoadRule r;
        const std::string ab = transport_ability(unit_id);
        if (ab.empty()) return r;
        std::vector<std::string> todo = array_links("CAbil", ab, "LoadValidatorArray");
        for (std::size_t i = 0; i < todo.size(); ++i) {
            const std::string& vid = todo[i];
            if (vid.empty()) continue;
            const Entry* v = find("CValidator", vid);
            if (!v) { ++r.unknown; continue; }
            if (v->tag == "CValidatorCombine") {
                for (const std::string& m : array_links("CValidator", vid, "CombineArray"))
                    if (!m.empty()) todo.push_back(m);
                continue;
            }
            if (v->tag == "CValidatorUnitType") {
                const auto val = scalar("CValidator", vid, "Value");
                if (!val || val->empty()) { ++r.unknown; continue; }
                const auto find_s = scalar("CValidator", vid, "Find");
                const bool want = !find_s || find_s->empty() || *find_s != "0";
                (want ? r.require_types : r.exclude_types).push_back(*val);
                continue;
            }
            if (v->tag == "CValidatorUnitCompareField") {
                const auto fld = scalar("CValidator", vid, "Field");
                const auto val = scalar("CValidator", vid, "Value");
                if (fld && val && *fld == "Race") {
                    r.require_race = *val == "Terr" ? 0 : *val == "Prot" ? 1 : *val == "Zerg" ? 2 : -1;
                    if (r.require_race < 0) ++r.unknown;
                } else {
                    ++r.unknown;
                }
                continue;
            }
            if (v->tag == "CValidatorUnitCompareBehaviorCount") {
                const auto beh = scalar("CValidator", vid, "Behavior");
                const auto cmp = scalar("CValidator", vid, "Compare");
                if (beh && !beh->empty() && (!cmp || cmp->empty())) r.block_behaviors.push_back(*beh);
                else ++r.unknown;
                continue;
            }
            ++r.unknown;
        }
        return r;
    }

    bool transport_player_hold(const std::string& unit_id) const {
        const std::string ab = transport_ability(unit_id);
        if (ab.empty()) return false;
        bool on = false;
        for (const core::XmlNode& n : resolved("CAbil", ab))
            if (n.name == "Flags" && n.attr_or("index", "") == "PlayerHold")
                on = n.attr_or("value", "1") != "0";
        return on;
    }
    bool transport_unload_targeted(const std::string& unit_id) const {
        const std::string ab = transport_ability(unit_id);
        if (ab.empty()) return false;
        bool at_hidden = false, all_hidden = false;
        for (const core::XmlNode& n : resolved("CAbil", ab)) {
            if (n.name != "CmdButtonArray") continue;
            const std::string idx = n.attr_or("index", "");
            for (const core::XmlNode& c : n.children) {
                if (c.name != "Flags" || c.attr_or("index", "") != "Hidden") continue;
                const bool h = c.attr_or("value", "1") != "0";
                if (idx == "UnloadAt") at_hidden = h;
                else if (idx == "UnloadAll") all_hidden = h;
            }
        }
        return !at_hidden && all_hidden;
    }
    double transport_field(const std::string& unit_id, const char* field) const {
        const std::string ab = transport_ability(unit_id);
        if (ab.empty()) return 0.0;
        return scalar_num("CAbil", ab, field).value_or(0.0);
    }
    double transport_load_range(const std::string& unit_id) const {
        const std::string ab = transport_ability(unit_id);
        if (ab.empty()) return 0.0;
        if (auto v = scalar_num("CAbil", ab, "Range")) return *v;
        if (auto d = tag_default_scalar("CAbilTransport", "Range"); d && !d->empty())
            return std::strtod(d->c_str(), nullptr);
        return 0.0;
    }
    struct TurretSpec {
        double yaw_arc = 0, yaw_rate = 0, yaw_idle_rate = 0, yaw_start = 0;
        std::string idle;
        double fidget_delay_min = 0, fidget_delay_max = 0, fidget_turn_angle = 0, fidget_turning_rate = 0, fidget_turn_chance = 0;
    };
    std::optional<TurretSpec> turret(const std::string& turret_id) const {
        if (!find("CTurret", turret_id)) return std::nullopt;
        auto str = [&](const char* f) -> std::string {
            if (auto v = scalar("CTurret", turret_id, f)) return *v;
            if (auto d = tag_default_scalar("CTurret", f)) return *d;
            return {};
        };
        auto num = [&](const char* f) {
            const std::string v = str(f);
            return v.empty() ? 0.0 : std::strtod(v.c_str(), nullptr);
        };
        TurretSpec ts{num("YawArc"), num("YawRate"), num("YawIdleRate"), num("YawStart"), str("Idle")};
        std::map<std::string, double> chance;
        auto scan = [&](const auto& nodes) {
            for (const core::XmlNode& fn : nodes) {
                if (fn.name != "Fidget") continue;
                auto take = [&](const char* a, double& out) {
                    if (const std::string* s = fn.attr(a)) out = std::strtod(s->c_str(), nullptr);
                };
                take("DelayMin", ts.fidget_delay_min);
                take("DelayMax", ts.fidget_delay_max);
                take("TurnAngle", ts.fidget_turn_angle);
                take("TurningRate", ts.fidget_turning_rate);
                for (const core::XmlNode& c : fn.children)
                    if (c.name == "ChanceArray")
                        if (const std::string* s = c.attr("value")) chance[c.attr_or("index", "")] = std::strtod(s->c_str(), nullptr);
            }
        };
        if (const auto* d = tag_default_nodes("CTurret")) scan(*d);
        scan(resolved("CTurret", turret_id));
        double total = 0;
        for (const auto& kv : chance) total += kv.second;
        if (total > 0) ts.fidget_turn_chance = chance["Turn"] / total;
        return ts;
    }
    std::string weapon_turret(const std::string& unit_id, const std::string& weapon_id) const {
        std::string t;
        for (const core::XmlNode& n : resolved("CUnit", unit_id))
            if (n.name == "WeaponArray" && n.attr_or("Link", "") == weapon_id)
                if (const std::string* v = n.attr("Turret")) t = *v;
        return t;
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

    int unit_fog_visibility(const std::string& unit_id) const {
        auto v = scalar("CUnit", unit_id, "FogVisibility");
        if (!v || v->empty()) return 0;
        if (*v == "Snapshot") return 1;
        if (*v == "Dimmed") return 2;
        return 0;
    }

    bool is_harvest_ability(const std::string& ability_id) const {
        const Entry* e = find("CAbil", ability_id);
        return e && e->tag == "CAbilHarvest";
    }

    struct PeonBuildMode {
        bool consumed = false;
        bool maintained = true;
        bool found = false;
        bool range_includes_building = true;
    };
    PeonBuildMode worker_build_mode(const std::string& unit_id) const {
        PeonBuildMode m;
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilBuild") continue;
            bool any_product = false;
            for (const core::XmlNode& n : resolved("CAbil", ab))
                if (n.name == "InfoArray") { any_product = true; break; }
            if (!any_product) continue;
            m.found = true;
            for (const core::XmlNode& n : resolved("CAbil", ab)) {
                if (n.name != "FlagArray") continue;
                const std::string idx = n.attr_or("index", "");
                const std::string val = n.attr_or("value", "1");
                const bool on = !(val.empty() || val == "0" || val == "false");
                if (idx == "PeonKillFinish") m.consumed = on;
                else if (idx == "PeonMaintained") m.maintained = on;
                else if (idx == "RangeIncludesBuilding") m.range_includes_building = on;
            }
            break;
        }
        return m;
    }

    std::optional<std::string> train_morph_unit(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e || e->tag != "CAbilTrain") continue;
            if (auto mu = scalar("CAbil", ab, "MorphUnit"); mu && !mu->empty()) return *mu;
        }
        return std::nullopt;
    }

    bool unit_commandable(const std::string& unit_id) const {
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            if (ab.empty()) continue;
            const Entry* e = find("CAbil", ab);
            if (!e) continue;
            if (e->tag == "CAbilMove" || e->tag == "CAbilAttack" || e->tag == "CAbilStop")
                return true;
        }
        return false;
    }

    bool footprint_layer_flag(const std::string& footprint_id, const char* layer_name, const char* sign,
                              const char* flag) const {
        bool on = false;
        for (const core::XmlNode& layer : resolved("CFootprint", footprint_id)) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != layer_name) continue;
            for (const core::XmlNode& set : layer.children) {
                if (set.name != "Sets") continue;
                for (const core::XmlNode& n : set.children)
                    if (n.name == sign && n.attr_or("index", "") == flag) on = n.attr_or("value", "1") != "0";
            }
        }
        return on;
    }
    std::vector<std::pair<int, int>> near_resources_offsets(const std::string& unit_id) const {
        auto fp = scalar("CUnit", unit_id, "Footprint");
        if (!fp || fp->empty() || !footprint_layer_flag(*fp, "Place", "Positive", "NearResources")) return {};
        return pathing_footprint_offsets(*fp, "Place",  true);
    }
    std::vector<std::pair<int, int>> near_resources_check_offsets(const std::string& unit_id) const {
        auto fp = scalar("CUnit", unit_id, "PlacementFootprint");
        if (!fp || fp->empty() || !footprint_layer_flag(*fp, "Check", "Negative", "NearResources")) return {};
        return pathing_footprint_offsets(*fp, "Check",  true);
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

    std::vector<std::pair<int, int>> creep_check_offsets(const std::string& unit_id) const {
        if (!requires_creep(unit_id)) return {};
        auto fp = scalar("CUnit", unit_id, "PlacementFootprint");
        if (!fp || fp->empty()) return {};
        return pathing_footprint_offsets(*fp, "Check");
    }

    std::string footprint_set_chars(const std::string& footprint_id, const char* layer_name, const char* sign,
                                    const char* flag) const {
        std::vector<std::pair<std::string, bool>> sets;
        auto apply = [&](const core::XmlNode& layer) {
            if (layer.name != "Layers" || layer.attr_or("index", "") != layer_name) return;
            for (const core::XmlNode& s : layer.children) {
                if (s.name != "Sets") continue;
                const std::string* si = s.attr("index");
                const std::size_t k = si ? std::size_t(std::max(0, std::atoi(si->c_str()))) : sets.size();
                if (k >= sets.size()) sets.resize(k + 1);
                if (const std::string* c = s.attr("Character")) sets[k].first = *c;
                for (const core::XmlNode& n : s.children)
                    if (n.name == sign && n.attr_or("index", "") == flag) sets[k].second = n.attr_or("value", "1") != "0";
            }
        };
        if (const std::vector<core::XmlNode>* d = tag_default_indexed_nodes("CFootprint"))
            for (const core::XmlNode& layer : *d) apply(layer);
        for (const core::XmlNode& layer : resolved("CFootprint", footprint_id)) apply(layer);
        std::string chars;
        for (const auto& [c, on] : sets)
            if (on && !c.empty() && chars.find(c[0]) == std::string::npos) chars.push_back(c[0]);
        return chars;
    }

    std::vector<std::pair<int, int>> creep_forbidden_offsets(const std::string& unit_id) const {
        auto fp = scalar("CUnit", unit_id, "PlacementFootprint");
        if (!fp || fp->empty() || !find("CFootprint", *fp)) return {};
        const std::string chars = footprint_set_chars(*fp, "Check", "Negative", "Creep");
        if (chars.empty()) return {};
        return pathing_footprint_offsets(*fp, "Check", true, &chars);
    }

    std::vector<std::pair<int, int>> placement_nospawn_offsets(const std::string& unit_id) const {
        auto fp = scalar("CUnit", unit_id, "PlacementFootprint");
        if (!fp || fp->empty() || !find("CFootprint", *fp)) return {};
        const std::string chars = footprint_set_chars(*fp, "Place", "Positive", "GroundNoSpawn");
        if (chars.empty()) return {};
        return pathing_footprint_offsets(*fp, "Place", true, &chars);
    }

    std::vector<std::pair<int, int>> placement_nobuild_offsets(const std::string& unit_id) const {
        if (auto own = scalar("CUnit", unit_id, "Footprint"); own && !own->empty()) return {};
        auto fp = scalar("CUnit", unit_id, "PlacementFootprint");
        if (!fp || fp->empty() || !find("CFootprint", *fp)) return {};
        const std::string chars = footprint_set_chars(*fp, "Place", "Positive", "NoBuild");
        if (chars.empty()) return {};
        return pathing_footprint_offsets(*fp, "Place", true, &chars);
    }

    struct UnitPower {
        double radius = 0;
        int level = 0;
        int boosted_level = 0;
        int boost_min_level = 0;
        double enabler_radius = 0;
        int enabler_level = 0;
    };

    std::string power_link(const std::string& beh) const;
    int power_source_level(const std::string& beh) const;
    bool power_field_link(const std::string& link) const;
    UnitPower unit_power(const std::string& unit_id) const;
    double warp_progress_rate(const std::string& unit_id, int power_level) const;

    bool power_source_reaches_higher(const std::string& unit_id) const {
        bool reaches = true;
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorPowerSource") continue;
            if (!power_field_link(power_link(beh))) continue;
            for (const core::XmlNode& n : resolved("CBehavior", beh))
                if (n.name == "CliffLevelFlags" && n.attr_or("index", "") == "Higher")
                    reaches = n.attr_or("value", "1") != "0";
        }
        return reaches;
    }

    double power_source_radius(const std::string& unit_id) const { return unit_power(unit_id).radius; }

    bool requires_power(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (e && e->tag == "CBehaviorPowerUser" && power_field_link(power_link(beh))) return true;
        }
        return false;
    }

    std::vector<std::string> unpowered_enabled_abilities(const std::string& unit_id) const {
        std::vector<std::string> out;
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorPowerUser") continue;
            for (const core::XmlNode& n : resolved("CBehavior", beh)) {
                if (n.name != "PowerStageArray") continue;
                if (n.attr("MinPowerLevel")) continue;
                for (const core::XmlNode& m : n.children) {
                    if (m.name != "Modification") continue;
                    for (const core::XmlNode& f : m.children)
                        if (f.name == "AbilLinkEnableArray")
                            if (const std::string* v = f.attr("value"))
                                if (!v->empty() &&
                                    std::find(out.begin(), out.end(), *v) == out.end())
                                    out.push_back(*v);
                }
            }
        }
        return out;
    }

    bool disables_abils_unpowered(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorPowerUser") continue;
            for (const core::XmlNode& n : resolved("CBehavior", beh)) {
                if (n.name != "PowerStageArray" || n.attr("MinPowerLevel")) continue;
                for (const core::XmlNode& m : n.children) {
                    if (m.name != "Modification") continue;
                    for (const core::XmlNode& f : m.children)
                        if (f.name == "ModifyFlags" && f.attr_or("index", "") == "DisableAbils" &&
                            f.attr_or("value", "") == "1")
                            return true;
                }
            }
        }
        return false;
    }

    double creep_source_period(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            const Entry* e = find("CBehavior", beh);
            if (!e || e->tag != "CBehaviorCreepSource") continue;
            if (auto p = scalar_num("CBehavior", beh, "Period")) return *p;
        }
        return 0;
    }

    bool radar_disabled_while_constructing(const std::string& unit_id) const {
        for (const std::string& beh : array_links("CUnit", unit_id, "BehaviorArray")) {
            if (beh.empty()) continue;
            bool grants_radar = false, disabled = false;
            for (const core::XmlNode& n : resolved("CBehavior", beh)) {
                if (n.name == "Modification" && n.attr("Radar")) grants_radar = true;
                if (n.name == "BuffFlags" &&
                    n.attr_or("index", "") == "DisableWhileUnderConstruction" &&
                    n.attr_or("value", "0") != "0")
                    disabled = true;
            }
            if (grants_radar && disabled) return true;
        }
        return false;
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

    struct SalvageSpec {
        bool found = false;
        double delay = 0;
        double mineral_frac = 0;
        double gas_frac = 0;
    };
    SalvageSpec salvage_spec(const std::string& abil_id) const {
        SalvageSpec s;
        for (const std::string& beh : array_links("CAbil", abil_id, "BehaviorArray")) {
            if (auto bb = behavior_buff(beh)) s.delay = bb->duration;
            s.found = true;
            break;
        }
        if (!s.found) return s;
        for (const core::XmlNode& c : resolved("CEffect", "SalvageShared")) {
            if (c.name != "SalvageFactor") continue;
            for (const core::XmlNode& r : c.children) {
                if (r.name != "Resource") continue;
                const std::string idx = r.attr_or("index", "");
                const double v = std::atof(r.attr_or("value", "0").c_str());
                if (idx == "Minerals") s.mineral_frac = v < 0 ? -v : v;
                else if (idx == "Vespene") s.gas_frac = v < 0 ? -v : v;
            }
        }
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

    std::string behavior_weapon_enable(const std::string& behavior_id) const {
        for (const core::XmlNode& n : resolved("CBehavior", behavior_id)) {
            if (n.name != "Modification") continue;
            for (const core::XmlNode& v : n.children)
                if (v.name == "WeaponEnableArray")
                    if (const std::string* val = v.attr("value"); val && !val->empty()) return *val;
        }
        return {};
    }

    std::vector<std::pair<double, double>> weapon_splash(const std::string& weapon_id) const;

    struct ArcSplash {
        double arc_deg;
        double radius;
        double frac;
    };
    std::vector<ArcSplash> weapon_arc_splash(const std::string& weapon_id) const;

    std::optional<double> missile_speed(const std::string& weapon_id) const;
    std::optional<double> effect_missile_speed(const std::string& effect_id) const;
    struct MissileMover {
        double range_lte = 0.0;
        double speed = 0.0;
        double accel = 0.0;
        double max_speed = 0.0;
    };
    std::vector<MissileMover> effect_missile_movers(const std::string& effect_id, const char* field = "Movers") const;
    double weapon_turret_yaw_rate(const std::string& unit_id) const;

    std::optional<std::string> weapon_missile_model_id(const std::string& weapon_id) const;
    std::optional<std::string> weapon_impact_model_id(const std::string& weapon_id) const;
    std::optional<std::string> weapon_launch_model_id(const std::string& weapon_id) const;

    const std::vector<core::XmlNode>& singleton_fields(const std::string& family) const;

    const std::vector<core::XmlNode>* tag_default_nodes(const std::string& tag) const {
        auto it = tag_defaults_.find(tag);
        return it == tag_defaults_.end() ? nullptr : &it->second;
    }
    bool ability_disables_class(const std::string& abil, const std::string& cls) const {
        const Entry* e = find("CAbil", abil);
        if (!e) return false;
        int own = -1;
        for (const core::XmlNode& n : resolved("CAbil", abil))
            if (n.name == "AbilClassDisableArray" && n.attr_or("index", "") == cls) own = n.attr_or("value", "0") != "0";
        if (own >= 0) return own == 1;
        if (const std::vector<core::XmlNode>* d = tag_default_indexed_nodes(e->tag))
            for (const core::XmlNode& n : *d)
                if (n.name == "AbilClassDisableArray" && n.attr_or("index", "") == cls) own = n.attr_or("value", "0") != "0";
        return own == 1;
    }
    const std::vector<core::XmlNode>* tag_default_indexed_nodes(const std::string& tag) const {
        auto it = tag_default_indexed_.find(tag);
        return it == tag_default_indexed_.end() ? nullptr : &it->second;
    }
    std::optional<double> refund_fraction(const std::string& abil_id, const std::string& resource) const {
        std::optional<double> v;
        auto scan = [&](const auto& nodes) {
            for (const core::XmlNode& rf : nodes) {
                if (rf.name != "RefundFraction") continue;
                for (const core::XmlNode& r : rf.children)
                    if (r.name == "Resource" && r.attr_or("index", "") == resource)
                        if (const std::string* s = r.attr("value")) v = std::fabs(std::strtod(s->c_str(), nullptr));
            }
        };
        scan(resolved("CAbil", abil_id));
        if (!v)
            if (const Entry* e = find("CAbil", abil_id))
                if (const auto* d = tag_default_nodes(e->tag)) scan(*d);
        return v;
    }
    struct OffCreepDecay {
        double period = 0, amount = 0;
    };
    OffCreepDecay off_creep_decay(const std::string& unit_id) const {
        for (const std::string& b : array_links("CUnit", unit_id, "BehaviorArray")) {
            const std::string req = b.empty() ? std::string() : scalar("CBehavior", b, "Requirements").value_or("");
            if (req.empty() || !requires_off_creep(req)) continue;
            const double period = scalar_num("CBehavior", b, "Period").value_or(0);
            const std::string eff = scalar("CBehavior", b, "PeriodicEffect").value_or("");
            const double amount = eff.empty() ? 0 : effect_amount(eff).value_or(0);
            if (period > 0 && amount > 0) return {period, amount};
        }
        return {};
    }
    double death_off_creep_radius(const std::string& unit_id) const {
        for (const std::string& b : array_links("CUnit", unit_id, "BehaviorArray")) {
            const std::string eff = b.empty() ? std::string() : scalar("CBehavior", b, "PeriodicEffect").value_or("");
            if (eff.empty()) continue;
            bool kills = false;
            for (const core::XmlNode& n : resolved("CEffect", eff))
                if (n.name == "Flags" && n.attr_or("index", "") == "Kill") kills = n.attr_or("value", "") == "1";
            if (!kills) continue;
            for (const std::string& v : array_links("CBehavior", b, "DisableValidatorArray")) {
                const Entry* ve = find("CValidator", v);
                if (!ve || ve->tag != "CValidatorLocationCreep" || scalar("CValidator", v, "Find").value_or("1") != "0")
                    continue;
                return scalar_num("CValidator", v, "RadiusBonus").value_or(0);
            }
        }
        return -1;
    }
    bool requires_off_creep(const std::string& req) const {
        std::string node;
        for (const core::XmlNode& n : resolved("CRequirement", req))
            if (n.name == "NodeArray") node = n.attr_or("Link", "");
        const Entry* ne = node.empty() ? nullptr : find("CRequirement", node);
        if (!ne || ne->tag != "CRequirementNot") return false;
        const std::vector<std::string> ops = array_links("CRequirement", node, "OperandArray");
        const Entry* ce = ops.empty() ? nullptr : find("CRequirement", ops[0]);
        if (!ce || ce->tag != "CRequirementCountBehavior") return false;
        for (const core::XmlNode& c : resolved("CRequirement", ops[0]))
            if (c.name == "Count" && c.attr_or("Link", "") == "OnCreep") return true;
        return false;
    }
    int unit_race_index(const std::string& unit_id) const {
        const auto r = scalar("CUnit", unit_id, "Race");
        if (!r) return -1;
        if (*r == "Prot") return 1;
        if (*r == "Zerg") return 2;
        if (*r == "Terr" || *r == "Neut") return 0;
        return -1;
    }
    struct HarvestSpec {
        bool found = false;
        int amount_request = 0;
        double time_mult = 0;
    };
    HarvestSpec harvest_spec(const std::string& unit_id, const std::string& resource = "Minerals") const {
        HarvestSpec out;
        for (const std::string& ab : array_links("CUnit", unit_id, "AbilArray")) {
            const Entry* e = ab.empty() ? nullptr : find("CAbil", ab);
            if (!e || e->tag != "CAbilHarvest") continue;
            out.found = true;
            for (const core::XmlNode& n : resolved("CAbil", ab)) {
                if (n.attr_or("index", "") != resource) continue;
                const std::string* v = n.attr("value");
                if (!v) continue;
                if (n.name == "ResourceAmountRequest") out.amount_request = std::atoi(v->c_str());
                else if (n.name == "ResourceTimeMultiplier") out.time_mult = std::strtod(v->c_str(), nullptr);
            }
            break;
        }
        return out;
    }
    std::vector<std::vector<std::string>> actor_progress_stages(const std::string& id) const {
        std::vector<std::vector<std::string>> out;
        for (const core::XmlNode& n : resolved("CActorProgress", id)) {
            if (n.name != "StageArray") continue;
            const std::string* ap = n.attr("AnimProps");
            if (!ap || ap->empty()) continue;
            std::vector<std::string> props;
            std::size_t s = 0;
            while (s <= ap->size()) {
                std::size_t c = ap->find(',', s);
                if (c == std::string::npos) c = ap->size();
                if (c > s) props.push_back(ap->substr(s, c - s));
                s = c + 1;
            }
            if (!props.empty()) out.push_back(std::move(props));
        }
        return out;
    }
    std::string progress_actor_for_build(const std::string& build_abil) const {
        if (build_abil.empty()) return {};
        const std::string want = "Abil." + build_abil + ".BuiltStart";
        std::vector<std::string> ids_ = ids("CActorProgress");
        std::sort(ids_.begin(), ids_.end());
        for (const std::string& id : ids_)
            for (const core::XmlNode& n : resolved("CActorProgress", id))
                if (n.name == "On" && n.attr_or("Terms", "") == want) return id;
        return {};
    }
    struct BeamActor {
        std::string actor;
        std::string model;
    };
    std::optional<BeamActor> beam_for_effect(const std::string& effect_id) const {
        if (effect_id.empty()) return std::nullopt;
        const std::string prefix = "Effect." + effect_id + ".";
        std::vector<std::string> ids_ = ids("CActorBeamSimple");
        for (const std::string& s : ids("CActorBeamStandard")) ids_.push_back(s);
        std::sort(ids_.begin(), ids_.end());
        for (const std::string& id : ids_) {
            const char* fam = find("CActorBeamSimple", id) ? "CActorBeamSimple" : "CActorBeamStandard";
            bool creates = false;
            std::string model;
            for (const core::XmlNode& n : resolved(fam, id)) {
                if (n.name == "Model") {
                    if (const std::string* v = n.attr("value")) model = *v;
                    else if (const std::string* l = n.attr("Link")) model = *l;
                } else if (n.name == "On" && n.attr_or("Send", "") == "Create") {
                    const std::string terms = n.attr_or("Terms", "");
                    if (terms.rfind(prefix, 0) == 0) creates = true;
                }
            }
            if (creates) return BeamActor{id, model};
        }
        return std::nullopt;
    }
    struct WarpBirth {
        double slow_s = 0;
        double fast_s = 0;
    };
    std::optional<WarpBirth> warp_birth_of(const std::string& warp_abil) const {
        if (warp_abil.empty()) return std::nullopt;
        const std::string want = "Abil." + warp_abil + ".TargetStart";
        std::vector<std::string> ids_ = ids("CActorProgress");
        std::sort(ids_.begin(), ids_.end());
        for (const std::string& id : ids_) {
            const auto& kids = resolved("CActorProgress", id);
            bool creates = false;
            for (const core::XmlNode& n : kids)
                if (n.name == "On" && n.attr_or("Terms", "") == want && n.attr_or("Send", "") == "Create") creates = true;
            if (!creates) continue;
            WarpBirth wb;
            bool any = false;
            for (const core::XmlNode& n : kids) {
                if (n.name != "On" || n.attr_or("Target", "") != "_Unit") continue;
                const std::string terms = n.attr_or("Terms", "");
                const std::string send = n.attr_or("Send", "");
                if (terms.rfind("ActorCreation", 0) != 0 || send.rfind("$Birth ", 0) != 0) continue;
                double v[4] = {0, 0, 0, 0};
                const char* p = send.c_str() + 7;
                int k = 0;
                for (; k < 4; ++k) {
                    char* end = nullptr;
                    v[k] = std::strtod(p, &end);
                    if (end == p) break;
                    p = end;
                }
                if (k < 4 || send.find("AsDuration") == std::string::npos) continue;
                if (terms.find("ValidateUnit SuperPower") != std::string::npos && terms.find("!ValidateUnit") == std::string::npos)
                    wb.fast_s = v[3];
                else
                    wb.slow_s = v[3];
                any = true;
            }
            if (any) return wb;
        }
        return std::nullopt;
    }
    std::optional<std::array<float, 3>> actor_tint(const std::string& unit_id, const std::string& terms_prefix,
                                                   const std::string& terms_contains) const;
    std::optional<float> unit_anim_blend_time(const std::string& unit_id) const {
        if (unit_id.empty()) return std::nullopt;
        ensure_actor_by_unit();
        auto it = actor_by_unit_.find(unit_id);
        if (it == actor_by_unit_.end()) return std::nullopt;
        const auto v = scalar("CActorUnit", it->second, "AnimBlendTime");
        if (!v || v->empty()) return std::nullopt;
        const double s = std::strtod(v->c_str(), nullptr);
        if (s < 0.0) return std::nullopt;
        return float(s);
    }
    std::vector<std::string> minimap_priority_order() const {
        std::vector<std::string> out;
        for (const core::XmlNode& n : resolved("CActorGlobalConfig", "SYSTEM_ActorConfig")) {
            if (n.name != "MinimapRenderPriorityList") continue;
            const std::string v = n.attr_or("value", "");
            if (!v.empty()) out.push_back(v);
        }
        return out;
    }
    struct CloakTransition {
        std::string enter;
        std::string loop;
    };
    std::optional<CloakTransition> cloak_transition(const std::string& from, const std::string& to) const {
        const core::XmlNodeList& all = resolved("CActorGlobalConfig", "SYSTEM_ActorConfig");
        for (auto it = all.rbegin(); it != all.rend(); ++it) {
            const core::XmlNode& n = *it;
            if (n.name != "CloakTransitionArray" || n.attr_or("index", "") != from) continue;
            for (const core::XmlNode& s : n.children) {
                if (s.name != "StateArray" || s.attr_or("index", "") != to) continue;
                CloakTransition t;
                t.enter = s.attr_or("Enter", "");
                t.loop = s.attr_or("Loop", "");
                for (const core::XmlNode& c : s.children) {
                    if (c.name == "Enter") t.enter = c.attr_or("value", t.enter);
                    if (c.name == "Loop") t.loop = c.attr_or("value", t.loop);
                }
                return t;
            }
        }
        return std::nullopt;
    }
    std::string actor_cloak_model() const {
        const core::XmlNodeList& all = resolved("CActorGlobalConfig", "SYSTEM_ActorConfig");
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (it->name == "CloakModel") return it->attr_or("value", "");
        return {};
    }
    std::string minimap_render_priority(const std::string& unit_id) const {
        if (unit_id.empty()) return {};
        ensure_actor_by_unit();
        auto it = actor_by_unit_.find(unit_id);
        if (it == actor_by_unit_.end()) return {};
        if (const auto v = scalar("CActorUnit", it->second, "MinimapRenderPriority"))
            if (!v->empty()) return *v;
        return {};
    }
    double unit_damage_flash_ms() const {
        for (const char* entry : {"Dflt", "CoreDflt"}) {
            if (const auto v = scalar("CGameUI", entry, "UnitDamageFlashDuration"))
                if (!v->empty()) {
                    const double d = std::strtod(v->c_str(), nullptr);
                    if (d > 0.0) return d;
                }
        }
        return 3000.0;
    }
    struct SelectionLook {
        std::array<std::uint8_t, 3> selected = {0, 168, 90};
        std::array<std::uint8_t, 3> subgroup = {0, 250, 25};
        std::array<std::uint8_t, 3> ally = {255, 255, 0};
        std::array<std::uint8_t, 3> enemy = {230, 0, 0};
        float width = 0.03f;
        float inner_boundary = 0.86f;
        float inner_falloff = 0.40f;
        float pre_alpha = 0.75f;
        float pre_rotation = 0.50f;
        int pre_segments = 8;
        float pre_solid = 0.80f;
    };
    SelectionLook selection_look() const {
        SelectionLook s;
        auto rgb_of = [](const std::string& v, std::array<std::uint8_t, 3>& out) {
            int a = 255, r = 0, g = 0, b = 0;
            if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &a, &r, &g, &b) != 4) return;
            out = {std::uint8_t(r), std::uint8_t(g), std::uint8_t(b)};
        };
        for (const char* entry : {"Dflt", "CoreDflt"}) {
            for (const core::XmlNode& n : resolved("CGameUI", entry)) {
                if (n.name == "SelectionColors") {
                    const std::string idx = n.attr_or("index", ""), v = n.attr_or("value", "");
                    if (v.empty()) continue;
                    if (idx == "Selected") rgb_of(v, s.selected);
                    else if (idx == "Subgroup") rgb_of(v, s.subgroup);
                    else if (idx == "Ally") rgb_of(v, s.ally);
                    else if (idx == "Enemy") rgb_of(v, s.enemy);
                    continue;
                }
                if (n.name != "SelectionData") continue;
                for (const core::XmlNode& f : n.children) {
                    const std::string v = f.attr_or("value", "");
                    if (v.empty()) continue;
                    const double d = std::strtod(v.c_str(), nullptr);
                    if (f.name == "SelectionWidth") s.width = float(d);
                    else if (f.name == "SelectionInnerBoundaryRatio") s.inner_boundary = float(d);
                    else if (f.name == "SelectionInnerBoundaryFallOffRatio") s.inner_falloff = float(d);
                    else if (f.name == "PreselectionAlpha") s.pre_alpha = float(d);
                    else if (f.name == "PreselectionRotationSpeed") s.pre_rotation = float(d);
                    else if (f.name == "PreselectionSegmentCount") s.pre_segments = int(d);
                    else if (f.name == "PreselectionSegmentPercentSolid") s.pre_solid = float(d);
                }
            }
        }
        if (s.pre_segments < 1) s.pre_segments = 1;
        return s;
    }
    struct MinimapColors {
        std::array<std::uint8_t, 3> resource = {126, 191, 241};
        std::array<std::uint8_t, 3> frustum = {255, 255, 255};
        std::array<std::uint8_t, 3> unit_border = {0, 0, 0};
        float border_size = 1.0f;
        float min_dot_size = 3.0f;
        std::array<std::uint8_t, 3> blip = {255, 0, 0};
        float radar_alpha = 60.0f / 255.0f;
    };
    MinimapColors minimap_colors() const {
        MinimapColors c;
        auto rgb_of = [](const std::string& v, std::array<std::uint8_t, 3>& out) {
            int a = 255, r = 0, g = 0, b = 0;
            if (std::sscanf(v.c_str(), "%d,%d,%d,%d", &a, &r, &g, &b) != 4) return;
            out = {std::uint8_t(r), std::uint8_t(g), std::uint8_t(b)};
        };
        for (const char* entry : {"Dflt", "CoreDflt"}) {
            for (const core::XmlNode& n : resolved("CGameUI", entry)) {
                if (n.name != "MinimapData") continue;
                for (const core::XmlNode& f : n.children) {
                    const std::string v = f.attr_or("value", "");
                    if (v.empty()) continue;
                    if (f.name == "ResourceUnitColor") rgb_of(v, c.resource);
                    else if (f.name == "FrustumColor") rgb_of(v, c.frustum);
                    else if (f.name == "UnitBorderColor") rgb_of(v, c.unit_border);
                    else if (f.name == "BlipUnitColor") rgb_of(v, c.blip);
                    else if (f.name == "BorderSize") {
                        const double d = std::strtod(v.c_str(), nullptr);
                        if (d >= 0.0 && d < 16.0) c.border_size = float(d);
                    }
                    else if (f.name == "MinUnitDotSize") {
                        const double d = std::strtod(v.c_str(), nullptr);
                        if (d > 0.0 && d < 64.0) c.min_dot_size = float(d);
                    }
                    else if (f.name == "RadarAlpha") {
                        const double d = std::strtod(v.c_str(), nullptr);
                        if (d > 0.0 && d <= 255.0) c.radar_alpha = float(d / 255.0);
                    }
                }
            }
        }
        return c;
    }
    struct MinimapLook {
        std::string icon;
        float scale = 1.0f;
    };
    std::optional<MinimapLook> unit_minimap_icon(const std::string& unit_id) const {
        if (unit_id.empty()) return std::nullopt;
        ensure_actor_by_unit();
        auto it = actor_by_unit_.find(unit_id);
        if (it == actor_by_unit_.end()) return std::nullopt;
        const auto v = scalar("CActorUnit", it->second, "MinimapIcon");
        if (!v || v->empty()) return std::nullopt;
        MinimapLook look;
        look.icon = *v;
        if (const auto s = scalar("CActorUnit", it->second, "MinimapIconScale")) {
            const double d = std::strtod(s->c_str(), nullptr);
            if (d > 0.0) look.scale = float(d);
        }
        return look;
    }
    struct HallucinationLook {
        std::string behavior;
        std::array<float, 3> tint = {1.0f, 1.0f, 1.0f};
        float opacity = 1.0f;
    };
    std::optional<HallucinationLook> hallucination_look() const {
        HallucinationLook look;
        bool found = false;
        for (const std::string& id : ids("CActor")) {
            const Entry* e = find("CActor", id);
            if (!e) continue;
            std::string beh;
            bool tinter = false;
            for (const core::XmlNode& n : e->own) {
                if (n.name != "On") continue;
                const std::string terms = n.attr_or("Terms", "");
                const std::string send = n.attr_or("Send", "");
                if (send.rfind("Signal ", 0) == 0 && send.find("TintOn") != std::string::npos &&
                    terms.rfind("Behavior.", 0) == 0) {
                    const std::size_t dot = terms.find('.', 9);
                    if (dot != std::string::npos && terms.compare(dot, 4, ".On;") == 0) beh = terms.substr(9, dot - 9);
                }
                if (terms.rfind("Signal.", 0) == 0 && terms.find("TintOn") != std::string::npos &&
                    n.attr_or("Target", "") == "_Unit") {
                    int r = 255, g = 255, b = 255;
                    float op = 1.0f;
                    if (send.rfind("SetTintColor ", 0) == 0 && std::sscanf(send.c_str() + 13, "%d,%d,%d", &r, &g, &b) == 3) {
                        look.tint = {float(r) / 255.0f, float(g) / 255.0f, float(b) / 255.0f};
                        tinter = true;
                    } else if (send.rfind("SetOpacity ", 0) == 0 && std::sscanf(send.c_str() + 11, "%f", &op) == 1) {
                        look.opacity = op;
                        tinter = true;
                    }
                }
            }
            if (tinter && !beh.empty()) {
                look.behavior = beh;
                found = true;
                break;
            }
        }
        if (!found) return std::nullopt;
        return look;
    }
    std::string build_ability_of(const std::string& structure) const {
        std::vector<std::string> ab = ids("CAbil");
        std::sort(ab.begin(), ab.end());
        for (const std::string& a : ab) {
            const Entry* e = find("CAbil", a);
            if (!e || e->tag != "CAbilBuild") continue;
            for (const core::XmlNode& n : resolved("CAbil", a))
                if (n.name == "InfoArray" && n.attr_or("Unit", "") == structure) return a;
        }
        return {};
    }
    std::optional<std::string> tag_default_scalar(const std::string& tag,
                                                  const std::string& field) const;

private:
    struct TwoSlot { std::string link; bool present = false; bool dead = false; };
    std::vector<TwoSlot> resolve_array_slots(const std::string& family,
                                             const std::string& id,
                                             const std::string& field,
                                             const std::string& expand_as = std::string()) const;

    struct SlotWrite {
        const core::XmlNode* node;
        std::string self;
    };
    struct NodeSlot { std::vector<SlotWrite> writes; bool dead = false; };
    std::vector<NodeSlot> resolve_node_slots(const std::string& family,
                                             const std::string& id,
                                             const std::string& field,
                                             const std::string& expand_as = std::string()) const;

    mutable FieldReadLog* field_read_log_ = nullptr;
    mutable FieldReadLog* field_touch_log_ = nullptr;
    const core::XmlNodeList& resolve_chain(const std::string& family, const std::string& id) const;
    std::string sibling_actor_family(const std::string& family, const std::string& id) const;
    std::vector<std::string> load_skips_;
    bool add_only_ = false;
    std::set<std::string> add_only_new_;
    std::vector<std::string> dependency_roots_loaded_;
    static std::vector<std::string> unlisted_families_;
    using IdMap = std::unordered_map<std::string, Entry>;
    std::unordered_map<std::string, IdMap> families_;
    std::unordered_map<std::string, std::vector<std::string>> family_order_;
    std::unordered_map<std::string, std::string> consts_;
    std::unordered_map<std::string, std::vector<core::XmlNode>> singletons_;
    std::unordered_map<std::string, std::vector<core::XmlNode>> tag_defaults_;
    std::unordered_map<std::string, std::vector<core::XmlNode>> tag_default_indexed_;
    StringTable strings_;
    StringTable hotkeys_;
    std::map<std::string, std::string> ai_scripts_;
    StringTable assets_;

    mutable std::unordered_map<std::string, core::XmlNodeList> resolved_cache_;
    mutable std::unordered_map<std::string, std::vector<core::XmlNode>> resolved_copies_;
    mutable std::unordered_set<std::string> spawner_trained_;
    mutable bool spawner_trained_built_ = false;
    mutable std::unordered_set<std::string> calldown_ammo_;
    mutable bool calldown_ammo_built_ = false;
    mutable std::unordered_set<std::string> power_field_links_;
    mutable bool power_field_links_built_ = false;
    mutable std::unordered_map<std::string, std::vector<RangeRing>> range_rings_;
    mutable std::unordered_map<std::string, double> abil_rings_;
    mutable bool range_rings_built_ = false;
    void build_range_rings() const;
    mutable std::unordered_map<std::string, std::vector<ActorSubscription>> actors_on_;
    mutable bool actors_on_built_ = false;

    mutable std::unordered_map<std::string, std::string> actor_by_unit_;
    mutable bool actor_by_unit_built_ = false;
    void ensure_actor_by_unit() const;
    mutable std::set<std::string> carded_cmds_;
    mutable bool carded_cmds_built_ = false;

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
        const std::function<T(T, const std::string&)>& periodic) const {
        if (depth > 32) return std::nullopt;
        const Entry* e = find("CEffect", effect_id);
        if (!e) return std::nullopt;
        const std::string& tag = e->tag;
        if (tag == "CEffectDamage") return at_damage(effect_id);
        auto follow = [&](const char* f) -> std::optional<T> {
            if (auto link = scalar("CEffect", effect_id, f); link && !link->empty())
                return walk_damage_tree<T>(*link, depth + 1, at_damage, periodic);
            return std::nullopt;
        };
        auto follow_array = [&](const char* f) -> std::optional<T> {
            for (const std::string& link : array_links("CEffect", effect_id, f))
                if (!link.empty())
                    if (auto d = walk_damage_tree<T>(link, depth + 1, at_damage, periodic))
                        return d;
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
                if (auto d = walk_damage_tree<T>(ce, depth + 1, at_damage, periodic))
                    return d;
        }
        for (const char* f : {"ImpactEffect", "InitialEffect", "FinalEffect", "ExpireEffect"})
            if (auto d = follow(f)) return d;
        for (const char* f : {"EffectArray", "PeriodicEffectArray", "CaseArray"})
            if (auto d = follow_array(f)) return d;
        return std::nullopt;
    }

    std::optional<int> effect_damage(const std::string& effect_id, int depth) const;
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
