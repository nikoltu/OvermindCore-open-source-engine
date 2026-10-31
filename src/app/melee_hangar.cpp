#include "omc/app/melee_game.hpp"

namespace omc::app {

namespace {

bool hangar_ability(const data::Catalog* cat, const std::string& abil_id) {
    if (!cat || abil_id.empty()) return false;
    const data::Catalog::Entry* e = cat->find("CAbil", abil_id);
    return e && e->tag == "CAbilArmMagazine" && !cat->calldown_magazine(abil_id);
}

}

bool MeleeGame::human_arm_hangar(const std::string& abil_id) {
    if (!hangar_ability(cat_, abil_id)) return false;
    std::uint32_t best = sim::kNoTarget;
    int32_t best_q = 0;
    bool any = false;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || !world_.hosts_interceptors(id)) continue;
        any = true;
        const int32_t q = world_.hangar_queued(id) + world_.hangar_stock(id);
        if (best == sim::kNoTarget || q < best_q) {
            best = id;
            best_q = q;
        }
    }
    if (!any) return false;
    if (best != sim::kNoTarget) world_.hangar_enqueue(best);
    return true;
}

bool MeleeGame::toggle_hangar_auto_build(const std::string& abil_id) {
    if (!hangar_ability(cat_, abil_id)) return false;
    bool any = false, on = false;
    for (std::uint32_t id : selection_)
        if (id < world_.units().size() && world_.units()[id].alive && world_.units()[id].player == cfg_.human_player &&
            world_.hosts_interceptors(id)) {
            if (!any) on = !world_.hangar_auto_build(id);
            any = world_.set_hangar_auto_build(id, on) || any;
        }
    return any;
}

bool MeleeGame::hangar_auto_build_shown(const std::string& abil_id) const {
    if (!hangar_ability(cat_, abil_id)) return false;
    for (std::uint32_t id : selection_)
        if (id < world_.units().size() && world_.hangar_auto_build(id)) return true;
    return false;
}

}
