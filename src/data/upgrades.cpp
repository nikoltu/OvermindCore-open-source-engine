#include "omc/data/upgrades.hpp"

#include <cstdlib>

#include "omc/casc/storage.hpp"
#include "omc/core/xml.hpp"
#include "omc/data/melee_mods.hpp"

namespace omc::data {

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
        case Op::Multiply:
            return 0.0;
        case Op::Set:
            return 0.0;
    }
    return 0.0;
}

double UpgradeEffect::apply(double cur) const {
    if (op == Op::Set)  return is_numeric() ? numeric_value() : cur;
    if (!is_numeric())  return cur;
    switch (op) {
        case Op::Add:      return cur + numeric_value();
        case Op::Subtract: return cur - numeric_value();
        case Op::Multiply: return cur * numeric_value();
        case Op::Set:      return numeric_value();
    }
    return cur;
}

static UpgradeEffect::Op parse_op(const std::string* s) {
    if (!s) return UpgradeEffect::Op::Add;
    if (*s == "Set") return UpgradeEffect::Op::Set;
    if (*s == "Subtract") return UpgradeEffect::Op::Subtract;
    if (*s == "Multiply") return UpgradeEffect::Op::Multiply;
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
    if (u.id.empty()) {
        u.id = id;
        order_.push_back(id);
    }
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
        const std::uint16_t decl = u.decl_seen++;
        for (const XmlNode& f : node.children)
            if (f.name == "EffectArray") u.writes.push_back(Write{f, decl});
    }
    resolved_cache_.clear();
}

std::vector<UpgradeCatalog::RSlot> UpgradeCatalog::resolve_slots(const std::string& id, int depth) const {
    auto uit = upgrades_.find(id);
    if (uit == upgrades_.end() || depth > 64) return {};
    const Upgrade& u = uit->second;
    std::vector<RSlot> slots;
    if (!u.parent.empty() && u.parent != id) slots = resolve_slots(u.parent, depth + 1);
    for (const Write& w : u.writes) {
        const XmlNode& c = w.node;
        RSlot* target = nullptr;
        if (const std::string* ix = c.attr("index")) {
            char* end = nullptr;
            long n = std::strtol(ix->c_str(), &end, 10);
            if (end == ix->c_str() || *end != '\0' || n < 0 || n > 65535) continue;
            if (std::size_t(n) >= slots.size()) slots.resize(std::size_t(n) + 1);
            target = &slots[std::size_t(n)];
        } else {
            slots.emplace_back();
            target = &slots.back();
        }
        if (const std::string* rm = c.attr("removed")) {
            target->dead = (*rm != "0" && !rm->empty());
            if (target->dead) continue;
        }
        if (const std::string* ref = c.attr("Reference")) {
            UpgradeEffect e;
            parse_reference(*ref, e);
            e.value = target->eff.value;
            e.op = target->eff.op;
            target->eff = e;
            target->has_ref = true;
        }
        if (const std::string* val = c.attr("Value")) target->eff.value = *val;
        if (c.attr("Operation")) target->eff.op = parse_op(c.attr("Operation"));
    }
    return slots;
}

bool UpgradeCatalog::contains(const std::string& id) const {
    return upgrades_.find(id) != upgrades_.end();
}

std::vector<std::string> UpgradeCatalog::ids() const {
    return order_;
}

const std::vector<UpgradeEffect>& UpgradeCatalog::resolve(const std::string& id) const {
    auto cit = resolved_cache_.find(id);
    if (cit != resolved_cache_.end()) return cit->second;

    auto& out = resolved_cache_[id];

    auto uit = upgrades_.find(id);
    if (uit == upgrades_.end()) return out;
    const Upgrade& u = uit->second;

    (void)u;
    std::vector<UpgradeEffect> merged;
    for (const RSlot& s : resolve_slots(id, 0))
        if (!s.dead && s.has_ref) merged.push_back(s.eff);

    return resolved_cache_[id] = std::move(merged);
}

void UpgradeCatalog::load_files(const casc::Storage& store,
                                const std::vector<std::string>& basenames) {
    load_skips_.clear();
    for (const char* mod : kMeleeMods) {
        for (const std::string& file : basenames) {
            std::string name =
                std::string("mods/") + mod + ".sc2mod/base.sc2data/GameData/" + file;
            try {
                ingest(store.read_by_name(name));
            } catch (const std::exception&) {
                load_skips_.push_back(name);
            }
        }
    }
}

void UpgradeCatalog::load_melee(const casc::Storage& store) {
    load_files(store, {"UpgradeData.xml"});
}

}
