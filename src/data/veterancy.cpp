#include "omc/data/veterancy.hpp"

#include <cstdlib>

namespace omc::data {

using core::XmlNode;
using sim::Fixed;

static const XmlNode* find_child(const XmlNode& n, const std::string& name) {
    for (const XmlNode& c : n.children)
        if (c.name == name) return &c;
    return nullptr;
}

std::optional<sim::VeterancyDef> load_veterancy(const Catalog& cat,
                                                const std::string& id) {
    const Catalog::Entry* e = cat.find("CBehavior", id);
    if (!e || e->tag.rfind("CBehaviorVeterancy", 0) != 0) return std::nullopt;

    sim::VeterancyDef def;
    def.xp_threshold.push_back(0);
    def.cum_life.push_back(Fixed());
    def.cum_damage.push_back(Fixed());

    int64_t cum_xp = 0;
    Fixed cum_life, cum_dmg;
    bool first = true;
    for (const XmlNode& lvl : cat.resolved("CBehavior", id)) {
        if (lvl.name == "XPFraction" && lvl.attr_or("index", "") == "Kills") {
            def.xp_fraction = Fixed::parse(lvl.attr_or("value", "1"));
            continue;
        }
        if (lvl.name != "VeterancyLevelArray") continue;
        if (first) {
            first = false;
            continue;
        }
        cum_xp += std::strtoll(lvl.attr_or("MinVeterancyXP", "0").c_str(), nullptr, 10);
        if (const XmlNode* mod = find_child(lvl, "Modification")) {
            for (const XmlNode& f : mod->children) {
                if (f.name == "VitalMaxArray" && f.attr_or("index", "") == "Life")
                    cum_life = cum_life + Fixed::parse(f.attr_or("value", "0"));
                else if (f.name == "DamageDealtUnscaled")
                    cum_dmg = cum_dmg + Fixed::parse(f.attr_or("value", "0"));
            }
        }
        def.xp_threshold.push_back(cum_xp);
        def.cum_life.push_back(cum_life);
        def.cum_damage.push_back(cum_dmg);
    }
    if (def.xp_threshold.size() <= 1) return std::nullopt;
    return def;
}

}
