#include "sc2/campaign/triggers.hpp"

#include <exception>
#include <string>
#include <unordered_map>

#include "sc2/campaign/mission_load.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/core/xml.hpp"

namespace sc2::campaign {

using core::XmlNode;

void TriggerStrings::merge_txt(const std::vector<std::uint8_t>& txt) {
    std::size_t i = 0, n = txt.size();
    if (n >= 3 && txt[0] == 0xEF && txt[1] == 0xBB && txt[2] == 0xBF) i = 3;
    while (i < n) {
        std::size_t start = i;
        while (i < n && txt[i] != '\n' && txt[i] != '\r') ++i;
        std::string line(reinterpret_cast<const char*>(txt.data()) + start, i - start);
        while (i < n && (txt[i] == '\n' || txt[i] == '\r')) ++i;
        std::size_t eq = line.find('=');
        if (eq == std::string::npos || eq == 0) continue;
        std::string key = line.substr(0, eq);
        names.emplace(std::move(key), line.substr(eq + 1));
    }
}

std::string TriggerStrings::name_of(const std::string& category, const TrigLibRef& ref) const {
    if (!ref.library.empty()) {
        auto it = names.find(category + "/Name/lib_" + ref.library + "_" + ref.id);
        if (it != names.end()) return it->second;
        return "lib_" + ref.library + "_" + ref.id;
    }
    auto it = names.find(category + "/Name/" + ref.id);
    if (it != names.end()) return it->second;
    return ref.id;
}

namespace {

bool ref_child(const XmlNode& parent, const std::string& tag, TrigLibRef& out) {
    for (const XmlNode& c : parent.children) {
        if (c.name != tag) continue;
        out.library = c.attr_or("Library", "");
        out.id = c.attr_or("Id", "");
        return true;
    }
    return false;
}

struct Resolver {
    const std::unordered_map<std::string, const XmlNode*>& by_id;
    const TriggerStrings* strings;
    TriggerModel& model;

    static constexpr int kMaxDepth = 24;
    static constexpr std::size_t kMaxParams = 64;
    static constexpr std::size_t kMaxSubs = 512;

    const XmlNode* lookup(const std::string& id) const {
        auto it = by_id.find(id);
        return it == by_id.end() ? nullptr : it->second;
    }

    static std::string key_name(const TrigLibRef& r) {
        return r.library.empty() ? r.id : "lib_" + r.library + "_" + r.id;
    }
    std::string fn_name(const TrigLibRef& def) const {
        return strings ? strings->name_of("FunctionDef", def) : key_name(def);
    }

    TrigFunction function(const XmlNode& fc, const std::string& role, int depth) {
        TrigFunction out;
        out.role = role;
        ref_child(fc, "FunctionDef", out.def);
        out.name = fn_name(out.def);
        TrigLibRef slot;
        if (ref_child(fc, "SubFunctionType", slot))
            out.sub_slot = strings ? strings->name_of("SubFuncType", slot) : key_name(slot);
        ++model.function_calls;
        if (depth >= kMaxDepth) return out;
        std::size_t np = 0;
        for (const XmlNode& c : fc.children) {
            if (c.name != "Parameter") continue;
            if (++np > kMaxParams) break;
            const std::string* pid = c.attr("Id");
            if (!pid) continue;
            const XmlNode* pe = lookup(*pid);
            if (pe) out.params.push_back(param(*pe, depth + 1));
        }
        std::size_t nb = 0;
        for (const XmlNode& c : fc.children) {
            if (c.name != "FunctionCall") continue;
            if (++nb > kMaxSubs) break;
            const std::string* fid = c.attr("Id");
            if (!fid) continue;
            const XmlNode* fe = lookup(*fid);
            if (fe) out.subs.push_back(function(*fe, "", depth + 1));
        }
        return out;
    }

    TrigParam param(const XmlNode& p, int depth) {
        TrigParam out;
        ref_child(p, "ParameterDef", out.def);

        for (const XmlNode& c : p.children)
            if (c.name == "ValueType") { out.value_type = c.attr_or("Type", ""); break; }

        for (const XmlNode& c : p.children) {
            if (c.name == "FunctionCall") {
                out.kind = TrigParamKind::Function;
                if (depth < kMaxDepth) {
                    if (const std::string* fid = c.attr("Id"))
                        if (const XmlNode* fe = lookup(*fid))
                            out.sub.push_back(function(*fe, "", depth + 1));
                }
                return out;
            }
            if (c.name == "Variable") {
                out.kind = TrigParamKind::Variable;
                out.ref.library = c.attr_or("Library", "");
                out.ref.id = c.attr_or("Id", "");
                for (const XmlNode& d : p.children)
                    if (d.name == "Array") { out.has_array_index = true; break; }
                return out;
            }
            if (c.name == "Preset" || c.name == "ValuePreset") {
                out.kind = TrigParamKind::Preset;
                out.ref.library = c.attr_or("Library", "");
                out.ref.id = c.attr_or("Id", "");
                out.preset_name = strings ? strings->name_of("PresetValue", out.ref) : key_name(out.ref);
                return out;
            }
            if (c.name == "ValueId") {
                out.kind = TrigParamKind::GameId;
                out.literal = c.attr_or("Id", "");
                return out;
            }
            if (c.name == "ValueElement") {
                out.kind = TrigParamKind::ElementRef;
                out.element_type = c.attr_or("Type", "");
                out.ref.library = c.attr_or("Library", "");
                out.ref.id = c.attr_or("Id", "");
                return out;
            }
        }

        for (const XmlNode& c : p.children) {
            if (c.name == "Value") {
                out.literal = c.text;
                out.kind = TrigParamKind::Literal;
                for (const XmlNode& d : p.children)
                    if (d.name == "ValueGameType") {
                        out.kind = TrigParamKind::GameLink;
                        out.game_type = d.attr_or("Type", "");
                        break;
                    }
                return out;
            }
        }
        return out;
    }

    TrigFunction comment(const XmlNode& ce) {
        TrigFunction out;
        out.is_comment = true;
        for (const XmlNode& c : ce.children)
            if (c.name == "Comment") { out.comment = c.text; break; }
        ++model.comments;
        return out;
    }

    void item(const XmlNode& child, std::vector<TrigFunction>& into, const std::string& role) {
        const std::string* id = child.attr("Id");
        if (!id) return;
        const XmlNode* e = lookup(*id);
        if (!e) return;
        const std::string* ty = e->attr("Type");
        if (ty && *ty == "Comment") into.push_back(comment(*e));
        else into.push_back(function(*e, role, 0));
    }
};

}

TriggerModel parse_triggers(const std::vector<std::uint8_t>& triggers_xml,
                            const TriggerStrings* strings) {
    TriggerModel model;
    XmlNode root;
    try {
        root = core::xml_parse(triggers_xml);
    } catch (const std::exception&) {
        return model;
    }
    if (root.name != "TriggerData") return model;
    model.parsed = true;

    std::unordered_map<std::string, const XmlNode*> by_id;
    by_id.reserve(root.children.size());
    for (const XmlNode& e : root.children) {
        if (e.name != "Element") continue;
        ++model.element_count;
        if (const std::string* id = e.attr("Id")) by_id.emplace(*id, &e);
    }

    Resolver rs{by_id, strings, model};

    for (const XmlNode& e : root.children) {
        if (e.name != "Element") continue;
        const std::string* ty = e.attr("Type");
        if (!ty || *ty != "Trigger") continue;
        Trigger t;
        t.id = e.attr_or("Id", "");
        t.name = strings ? strings->name_of("Trigger", TrigLibRef{"", t.id}) : t.id;
        for (const XmlNode& c : e.children) {
            if (c.name == "InitOff") t.init_off = true;
            else if (c.name == "Event") rs.item(c, t.events, "Event");
            else if (c.name == "Condition") rs.item(c, t.conditions, "Condition");
            else if (c.name == "Action") rs.item(c, t.actions, "Action");
        }
        model.triggers.push_back(std::move(t));
    }
    return model;
}

TriggerStrings load_mission_trigger_strings(const casc::Storage& store,
                                            const std::string& map_prefix) {
    TriggerStrings out;
    auto merge = [&](const std::string& name) {
        try {
            out.merge_txt(store.read_by_name(name));
        } catch (const std::exception&) {   }
    };
    std::string prefix = map_prefix;
    if (!prefix.empty() && prefix.back() == '/') prefix.pop_back();

    const std::string loc = "/enus.sc2data/LocalizedData/TriggerStrings.txt";
    merge(prefix + loc);
    merge("mods/core.sc2mod/enus.sc2data/LocalizedData/TriggerStrings.txt");
    merge("mods/liberty.sc2mod/enus.sc2data/LocalizedData/TriggerStrings.txt");
    merge("campaigns/libertystory.sc2campaign/enus.sc2data/LocalizedData/TriggerStrings.txt");
    return out;
}

TriggerModel load_mission_triggers(const casc::Storage& store, const std::string& map_prefix) {
    std::vector<std::uint8_t> chunk = load_mission_trigger_chunk(store, map_prefix);
    TriggerStrings strings = load_mission_trigger_strings(store, map_prefix);
    return parse_triggers(chunk, &strings);
}

}
