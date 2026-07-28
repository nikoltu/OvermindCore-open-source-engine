#include "sc2/data/upgrades.hpp"

#include <cstdlib>

#include "sc2/casc/storage.hpp"
#include "sc2/core/xml.hpp"

namespace sc2::data {

using core::XmlNode;

double UpgradeEffect::numeric_value() const {
    if (value.empty()) return 0.0;
    const char* s = value.c_str();
    char* end = nullptr;
    double d = std::strtod(s, &end);
    return end == s ? 0.0 : d;
}

bool UpgradeEffect::is_numeric() const {
    if (value.empty()) return false;
    const char* s = value.c_str();
    char* end = nullptr;
    std::strtod(s, &end);
    if (end == s) return false;
    while (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n') ++end;
    return *end == '\0';
}

double UpgradeEffect::delta() const {
    if (!is_numeric()) return 0.0;
    switch (op) {
        case Op::Add:
            return numeric_value();
        case Op::Subtract:
            return -numeric_value();
        case Op::Set:
            return 0.0;
    }
    return 0.0;
}

static UpgradeEffect::Op parse_op(const std::string* s) {
    if (!s) return UpgradeEffect::Op::Add;
    if (*s == "Set") return UpgradeEffect::Op::Set;
    if (*s == "Subtract") return UpgradeEffect::Op::Subtract;
    return UpgradeEffect::Op::Add;
}

static void parse_reference(const std::string& ref, UpgradeEffect& e) {
    e.reference = ref;
    size_t c1 = ref.find(',');
    if (c1 == std::string::npos) {
        e.field = ref;
        return;
    }
    size_t c2 = ref.find(',', c1 + 1);
    e.catalog = ref.substr(0, c1);
    if (c2 == std::string::npos) {
        e.entry = ref.substr(c1 + 1);
        return;
    }
    e.entry = ref.substr(c1 + 1, c2 - c1 - 1);
    std::string fld = ref.substr(c2 + 1);
    size_t lb = fld.find('[');
    if (lb != std::string::npos && fld.back() == ']') {
        e.field = fld.substr(0, lb);
        e.field_index = fld.substr(lb + 1, fld.size() - lb - 2);
    } else {
        e.field = fld;
    }
}

UpgradeCatalog::Upgrade& UpgradeCatalog::upgrade(const std::string& id) {
    Upgrade& u = upgrades_[id];
    if (u.id.empty()) u.id = id;
    return u;
}

void UpgradeCatalog::ingest(const std::vector<uint8_t>& xml) {
    if (xml.empty()) return;
    XmlNode root = core::xml_parse(xml);
    if (root.name != "Catalog") return;

    for (const XmlNode& node : root.children) {
        if (node.name.rfind("CUpgrade", 0) != 0) continue;
        const std::string* id = node.attr("id");
        if (!id) continue;
        Upgrade& u = upgrade(*id);
        if (const std::string* p = node.attr("parent")) u.parent = *p;

        for (const XmlNode& f : node.children) {
            if (f.name != "EffectArray") continue;

            std::string key;
            if (const std::string* idx = f.attr("index")) {
                key = *idx;
                char* end = nullptr;
                long n = std::strtol(idx->c_str(), &end, 10);
                if (end != idx->c_str() && *end == '\0' && n + 1 > u.next_auto)
                    u.next_auto = static_cast<int>(n + 1);
            } else {
                key = std::to_string(u.next_auto++);
            }

            auto it = u.pos.find(key);

            if (const std::string* rm = f.attr("removed"); rm && *rm == "1") {
                if (it != u.pos.end()) u.slots[it->second].alive = false;
                continue;
            }

            const std::string* ref = f.attr("Reference");
            const std::string* val = f.attr("Value");
            UpgradeEffect::Op op = parse_op(f.attr("Operation"));

            if (it != u.pos.end()) {
                Slot& slot = u.slots[it->second];
                slot.alive = true;
                if (ref) {
                    slot.eff = UpgradeEffect{};
                    parse_reference(*ref, slot.eff);
                    slot.has_ref = true;
                }
                if (val) slot.eff.value = *val;
                if (f.attr("Operation")) slot.eff.op = op;
            } else if (ref) {
                Slot slot;
                slot.key = key;
                slot.has_ref = true;
                parse_reference(*ref, slot.eff);
                if (val) slot.eff.value = *val;
                slot.eff.op = op;
                u.pos[key] = u.slots.size();
                u.slots.push_back(std::move(slot));
            }
        }
    }
    resolved_cache_.clear();
}

bool UpgradeCatalog::contains(const std::string& id) const {
    return upgrades_.find(id) != upgrades_.end();
}

std::vector<std::string> UpgradeCatalog::ids() const {
    std::vector<std::string> out;
    out.reserve(upgrades_.size());
    for (const auto& [id, u] : upgrades_) out.push_back(id);
    return out;
}

const std::vector<UpgradeEffect>& UpgradeCatalog::resolve(const std::string& id) const {
    auto cit = resolved_cache_.find(id);
    if (cit != resolved_cache_.end()) return cit->second;

    auto& out = resolved_cache_[id];

    auto uit = upgrades_.find(id);
    if (uit == upgrades_.end()) return out;
    const Upgrade& u = uit->second;

    std::vector<UpgradeEffect> merged;
    if (!u.parent.empty() && u.parent != id) {
        const auto& base = resolve(u.parent);
        merged.insert(merged.end(), base.begin(), base.end());
    }
    for (const Slot& s : u.slots)
        if (s.alive && s.has_ref) merged.push_back(s.eff);

    return resolved_cache_[id] = std::move(merged);
}

static const char* kMeleeMods[] = {"core",       "liberty",   "libertymulti",
                                   "swarm",      "swarmmulti", "void",
                                   "voidmulti"};

void UpgradeCatalog::load_files(const casc::Storage& store,
                                const std::vector<std::string>& basenames) {
    for (const char* mod : kMeleeMods) {
        for (const std::string& file : basenames) {
            std::string name =
                std::string("mods/") + mod + ".sc2mod/base.sc2data/GameData/" + file;
            try {
                ingest(store.read_by_name(name));
            } catch (const std::exception&) {
            }
        }
    }
}

void UpgradeCatalog::load_melee(const casc::Storage& store) {
    load_files(store, {"UpgradeData.xml"});
}

}
