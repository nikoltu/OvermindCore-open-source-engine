#include "sc2/app/melee_game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <set>
#include <string>
#include <unordered_map>

#include "sc2/app/caster_composition.hpp"
#include "sc2/app/command_bind.hpp"
#include "sc2/app/control.hpp"
#include "sc2/app/hud.hpp"
#include "sc2/app/hud_render.hpp"
#include "sc2/app/melee_commands.hpp"
#include "sc2/app/ability.hpp"
#include "sc2/app/spawn.hpp"
#include "sc2/data/economy.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"
#include "sc2/sim/transport.hpp"
#include "sc2/render/prim.hpp"
#include "sc2/ui/font.hpp"

namespace sc2::app {

using sim::Unit;

namespace {

const char kInfantryWeaponsL1[] = "TerranInfantryWeaponsLevel1";

constexpr std::int32_t kAbilCdSlotInfestedTerran = 0;

void alpha_over(std::vector<uint8_t>& base, const std::vector<uint8_t>& top) {
    size_t n = std::min(base.size(), top.size());
    for (size_t i = 0; i + 3 < n; i += 4) {
        float a = top[i + 3] / 255.0f;
        for (int c = 0; c < 3; ++c)
            base[i + c] = uint8_t(float(top[i + c]) * a + float(base[i + c]) * (1.0f - a) + 0.5f);
        base[i + 3] = uint8_t(std::min(255.0f, float(top[i + 3]) + float(base[i + 3]) * (1.0f - a) + 0.5f));
    }
}

std::array<float, 4> team_colour(int player, bool selected) {
    std::array<float, 4> c;
    if (player == 0)
        c = {0.85f, 0.15f, 0.12f, 1.0f};
    else if (player == 1)
        c = {0.15f, 0.35f, 0.90f, 1.0f};
    else
        return {0, 0, 0, -1.0f};
    if (selected)
        for (int i = 0; i < 3; ++i) c[i] = std::min(1.0f, c[i] + 0.35f);
    return c;
}

[[maybe_unused]] int race_of(const std::string& t) {
    static const char* kProtoss[] = {"Probe", "Zealot", "Stalker", "Sentry", "Adept",
        "Immortal", "Colossus", "Nexus", "Pylon", "Gateway", "WarpGate", "Assimilator",
        "Cybernetics", "Forge", "Stargate", "Robotics", "Twilight", "Dark", "Templar",
        "Archon", "Phoenix", "Oracle", "Void", "Carrier", "Tempest", "Mothership",
        "Observer", "WarpPrism", "Disruptor"};
    static const char* kZerg[] = {"Drone", "Zergling", "Baneling", "Roach", "Ravager",
        "Hydralisk", "Lurker", "Infestor", "SwarmHost", "Ultralisk", "Overlord", "Overseer",
        "Mutalisk", "Corruptor", "Brood", "Viper", "Queen", "Hatchery", "Lair", "Hive",
        "Spawning", "Extractor", "Spire", "Nydus", "Creep", "Roach", "Baneling"};
    for (const char* s : kProtoss) if (t.find(s) != std::string::npos) return 1;
    for (const char* s : kZerg) if (t.find(s) != std::string::npos) return 2;
    return 0;
}

struct WireMask { int w = 0, h = 0; std::vector<std::uint8_t> cov; };

const WireMask& wire_mask(const formats::Model* m, int W, int H) {
    static std::unordered_map<const formats::Model*, WireMask> cache;
    auto it = cache.find(m);
    if (it != cache.end()) return it->second;
    WireMask wm; wm.w = W; wm.h = H; wm.cov.assign(std::size_t(W) * H, 0);
    if (m && m->vertex_count > 2 && !m->positions.empty()) {
        float mnx = 1e30f, mxx = -1e30f, mnz = 1e30f, mxz = -1e30f;
        for (std::uint32_t i = 0; i < m->vertex_count; ++i) {
            const float x = m->positions[3 * i], z = m->positions[3 * i + 2];
            mnx = x < mnx ? x : mnx; mxx = x > mxx ? x : mxx;
            mnz = z < mnz ? z : mnz; mxz = z > mxz ? z : mxz;
        }
        const float spanx = std::max(mxx - mnx, 1e-3f), spanz = std::max(mxz - mnz, 1e-3f);
        const float availW = W * 0.84f, availH = H * 0.84f;
        const float s = std::min(availW / spanx, availH / spanz);
        const float ox = (W - spanx * s) * 0.5f, oy = (H - spanz * s) * 0.5f;
        auto proj = [&](std::uint32_t i, float& sx, float& sy) {
            sx = ox + (m->positions[3 * i] - mnx) * s;
            sy = oy + (mxz - m->positions[3 * i + 2]) * s;
        };
        auto raster = [&](std::uint32_t a, std::uint32_t b, std::uint32_t c) {
            if (a >= m->vertex_count || b >= m->vertex_count || c >= m->vertex_count) return;
            float ax, ay, bx, by, cx, cy;
            proj(a, ax, ay); proj(b, bx, by); proj(c, cx, cy);
            int x0 = std::max(0, int(std::floor(std::min(ax, std::min(bx, cx)))));
            int x1 = std::min(W - 1, int(std::ceil(std::max(ax, std::max(bx, cx)))));
            int y0 = std::max(0, int(std::floor(std::min(ay, std::min(by, cy)))));
            int y1 = std::min(H - 1, int(std::ceil(std::max(ay, std::max(by, cy)))));
            const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
            if (std::fabs(area) < 1e-6f) return;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float w0 = ((bx - px) * (cy - py) - (by - py) * (cx - px)) / area;
                    const float w1 = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) / area;
                    const float w2 = 1.0f - w0 - w1;
                    if (w0 >= -0.001f && w1 >= -0.001f && w2 >= -0.001f)
                        wm.cov[std::size_t(y) * W + x] = 255;
                }
        };
        for (const formats::M3Region& r : m->regions) {
            const std::uint32_t end = r.first_index + r.num_indices;
            for (std::uint32_t k = r.first_index; k + 2 < end && k + 2 < m->indices.size(); k += 3)
                raster(m->indices[k] + r.first_vertex, m->indices[k + 1] + r.first_vertex,
                       m->indices[k + 2] + r.first_vertex);
        }
    }
    return cache.emplace(m, std::move(wm)).first->second;
}

float model_diameter(const formats::Model& m) {
    if (!m.vertex_count || m.positions.size() < size_t(m.vertex_count) * 3) return 0.0f;
    float mn = 1e30f, mx = -1e30f;
    for (uint32_t i = 0; i < m.vertex_count; ++i)
        for (int a = 0; a < 2; ++a) {
            float v = m.positions[i * 3 + a];
            mn = v < mn ? v : mn;
            mx = v > mx ? v : mx;
        }
    return mx - mn;
}

std::unordered_map<const formats::Model*, float>& diam_cache() {
    static std::unordered_map<const formats::Model*, float> c;
    return c;
}
float cached_diameter(const formats::Model* m) {
    if (!m) return 0.0f;
    auto& c = diam_cache();
    auto it = c.find(m);
    if (it != c.end()) return it->second;
    float d = model_diameter(*m);
    c[m] = d;
    return d;
}

void draw_text_rgba(std::vector<std::uint8_t>& buf, std::uint32_t w, std::uint32_t h, int x,
                    int y, const std::string& text, int s, std::uint8_t r, std::uint8_t g,
                    std::uint8_t b) {
    auto put = [&](int px, int py, std::uint8_t cr, std::uint8_t cg, std::uint8_t cb) {
        if (px < 0 || py < 0 || px >= int(w) || py >= int(h)) return;
        std::size_t i = (std::size_t(py) * w + px) * 4;
        buf[i] = cr; buf[i + 1] = cg; buf[i + 2] = cb; buf[i + 3] = 255;
    };
    int pen = x;
    for (char ch : text) {
        for (int row = 0; row < ui::kFontH; ++row)
            for (int col = 0; col < ui::kFontW; ++col)
                if (ui::font_pixel(ch, col, row))
                    for (int dy = 0; dy < s; ++dy)
                        for (int dx = 0; dx < s; ++dx) {
                            int px = pen + col * s + dx, py = y + row * s + dy;
                            put(px + s, py + s, 0, 0, 0);
                            put(px, py, r, g, b);
                        }
        pen += (ui::kFontW + 1) * s;
    }
}

void draw_box_rgba(std::vector<std::uint8_t>& buf, std::uint32_t w, std::uint32_t h, int x0,
                   int y0, int x1, int y1, std::uint8_t r, std::uint8_t g, std::uint8_t b) {
    if (x1 < x0) std::swap(x0, x1);
    if (y1 < y0) std::swap(y0, y1);
    auto put = [&](int px, int py) {
        if (px < 0 || py < 0 || px >= int(w) || py >= int(h)) return;
        std::size_t i = (std::size_t(py) * w + px) * 4;
        buf[i] = r; buf[i + 1] = g; buf[i + 2] = b; buf[i + 3] = 255;
    };
    for (int x = x0; x <= x1; ++x) { put(x, y0); put(x, y1); }
    for (int y = y0; y <= y1; ++y) { put(x0, y); put(x1, y); }
}

}

MeleeGame::MeleeGame(MeleeGameConfig cfg) : cfg_(cfg), world_(1) {
    alerts_.set_player(cfg_.human_player >= 0 ? cfg_.human_player : 0);
}

std::string MeleeGame::human_gas_structure() const {
    if (cfg_.human_player < 0 || size_t(cfg_.human_player) >= layout_.players.size())
        return "Refinery";
    const std::uint32_t depot = layout_.players[size_t(cfg_.human_player)].base.depot;
    const int r = depot < world_.units().size() ? race_of(world_.units()[depot].catalog_id) : 0;
    return r == 2 ? "Extractor" : r == 1 ? "Assimilator" : "Refinery";
}

std::string MeleeGame::human_town_hall() const {
    if (cfg_.human_player < 0 || size_t(cfg_.human_player) >= layout_.players.size())
        return "CommandCenter";
    const std::uint32_t depot = layout_.players[size_t(cfg_.human_player)].base.depot;
    const int r = depot < world_.units().size() ? race_of(world_.units()[depot].catalog_id) : 0;
    return r == 2 ? "Hatchery" : r == 1 ? "Nexus" : "CommandCenter";
}

bool MeleeGame::human_is_protoss() const {
    if (cfg_.human_player < 0 || size_t(cfg_.human_player) >= layout_.players.size()) return false;
    const std::uint32_t depot = layout_.players[size_t(cfg_.human_player)].base.depot;
    return depot < world_.units().size() && race_of(world_.units()[depot].catalog_id) == 1;
}

MeleeCommandContext MeleeGame::command_context() {
    MeleeCommandContext ctx;
    if (cfg_.human_player < 0) return ctx;
    for (std::uint32_t id : selection_)
        if (id < world_.units().size() && world_.unit(id).alive &&
            world_.unit(id).player == cfg_.human_player) {
            const sim::Unit& u = world_.unit(id);
            if (u.speed.raw() != 0) {
                ctx.mobile_selected = true;
                if (id < layout_.unit_type.size() && layout_.unit_type[id] == "SCV")
                    ctx.worker_selected = true;
                else
                    ctx.combat_selected = true;
            } else if (u.supply_provides.raw() > 0) {
                ctx.town_hall_selected = true;
            }
        }
    ctx.producer_built = human_producer_ != sim::kNoTarget;
    const int bank = world_.player(cfg_.human_player).minerals;
    ctx.can_afford_producer = bank >= human_build_.producer_cost;
    ctx.can_afford_army = bank >= human_build_.army_cost;
    ctx.can_afford_worker = bank >= human_build_.worker_cost;
    ctx.can_afford_supply = bank >= human_build_.supply_cost;
    if (ctx.worker_selected && !ctx.town_hall_selected && cat_ &&
        size_t(cfg_.human_player) < layout_.players.size()) {
        ctx.worker_gas_selected = true;
        ctx.refinery_name = human_gas_structure();
        ctx.can_afford_refinery = bank >= data::unit_cost(*cat_, ctx.refinery_name).minerals;
        ctx.worker_expand_selected = true;
        ctx.town_hall_name = human_town_hall();
        ctx.can_afford_town_hall = bank >= data::unit_cost(*cat_, ctx.town_hall_name).minerals;
    }
    if (ctx.worker_selected && !ctx.town_hall_selected && ctx.producer_built && cat_) {
        ctx.worker_starport_selected = true;
        const data::UnitCost sc = data::unit_cost(*cat_, "Starport");
        ctx.can_afford_starport = bank >= sc.minerals && world_.player(cfg_.human_player).gas >= sc.gas;
    }
    if (cat_ && selected_starport() != sim::kNoTarget) {
        ctx.starport_selected = true;
        ctx.air_unit_name = "Viking";
        const data::UnitCost ac = data::unit_cost(*cat_, "VikingFighter");
        ctx.can_afford_air = bank >= ac.minerals && world_.player(cfg_.human_player).gas >= ac.gas;
    }
    if (ctx.worker_selected && !ctx.town_hall_selected && cat_ && human_is_protoss()) {
        ctx.worker_pylon_selected = true;
        ctx.can_afford_pylon = bank >= data::unit_cost(*cat_, "Pylon").minerals;
        ctx.worker_gateway_selected = true;
        ctx.can_afford_gateway = bank >= data::unit_cost(*cat_, "Gateway").minerals;
    }
    if (cat_ && human_is_protoss() && selected_gateway() != sim::kNoTarget) {
        ctx.gateway_selected = true;
        ctx.can_afford_zealot = bank >= data::unit_cost(*cat_, "Zealot").minerals;
    }
    ctx.addon_producer_selected = selected_addon_producer() != sim::kNoTarget;
    ctx.can_afford_addon = bank >= 50 && world_.player(cfg_.human_player).gas >= 25;
    if (cat_) ctx.can_afford_factory = bank >= data::unit_cost(*cat_, "Factory").minerals;
    if (const std::uint32_t tp = selected_techlab_producer(); tp != sim::kNoTarget && cat_) {
        const data::Catalog::TechTrain tt = cat_->techlab_train_product(world_.units()[tp].catalog_id);
        if (!tt.unit.empty()) {
            ctx.techlab_producer_selected = true;
            ctx.techlab_unit_name = tt.unit;
            const data::UnitCost mc = data::unit_cost(*cat_, tt.unit);
            ctx.can_afford_marauder = bank >= mc.minerals && world_.player(cfg_.human_player).gas >= mc.gas;
        }
    }
    if (cat_ && selected_command_center() != sim::kNoTarget && ctx.producer_built) {
        ctx.cc_morph_selected = true;
        const auto delta = morph_cost_delta(*cat_, "CommandCenter", "OrbitalCommand");
        ctx.can_afford_orbital =
            bank >= delta.first && world_.player(cfg_.human_player).gas >= delta.second;
    }
    if (const std::uint32_t ob = selected_orbital(); ob != sim::kNoTarget) {
        ctx.orbital_selected = true;
        ctx.orbital_has_energy = world_.units()[ob].energy >= sim::Fixed::from_int(50);
    }
    if (cat_) ctx.can_afford_engbay = bank >= data::unit_cost(*cat_, "EngineeringBay").minerals;
    if (cat_ && selected_completed_engbay() != sim::kNoTarget) {
        ctx.engbay_selected = true;
        const data::Catalog::ResearchInfo info = cat_->research_info(kInfantryWeaponsL1);
        const sim::Player& hp = world_.player(cfg_.human_player);
        const bool done = research_ && research_->has(cfg_.human_player, kInfantryWeaponsL1);
        const bool inflight = research_ && research_->pending(cfg_.human_player, kInfantryWeaponsL1);
        ctx.can_afford_research =
            info.found && !done && !inflight && hp.minerals >= info.minerals && hp.gas >= info.gas;
    }
    if (cat_) {
        if (selected_liberator() != sim::kNoTarget) ctx.liberator_selected = true;
        if (selected_liberator_ag() != sim::kNoTarget) ctx.liberator_ag_selected = true;
    }
    if (cat_ && selected_merge_templar().size() >= 2) ctx.templar_merge_selected = true;
    if (const std::uint32_t qz = cat_ ? selected_queen() : sim::kNoTarget; qz != sim::kNoTarget) {
        ctx.queen_selected = true;
        const auto iv = cat_->inject_ability(world_.unit(qz).catalog_id);
        const sim::Fixed e = app::ability_energy_cost(*cat_, iv.ability);
        ctx.inject_ready = e.raw() <= 0 || world_.unit(qz).energy >= e;
    }
    if (const std::uint32_t iz = cat_ ? selected_infestor() : sim::kNoTarget; iz != sim::kNoTarget) {
        ctx.infestor_selected = true;
        const auto it = cat_->infested_terran_ability(world_.unit(iz).catalog_id);
        const sim::Fixed e = sim::Fixed::from_double(it.energy);
        ctx.infested_terran_ready =
            world_.ability_cooldown_left(iz, kAbilCdSlotInfestedTerran).raw() <= 0 &&
            (e.raw() <= 0 || world_.unit(iz).energy >= e);
    }
    if (cat_) {
        if (!selected_burrowers(false).empty()) ctx.burrow_selected = true;
        if (!selected_burrowers(true).empty()) ctx.unburrow_selected = true;
    }
    if (cat_) {
        if (selected_liftoff_structure(false) != sim::kNoTarget) ctx.liftoff_selected = true;
        if (selected_liftoff_structure(true) != sim::kNoTarget) ctx.land_selected = true;
    }
    if (cat_) {
        if (selected_depot(false) != sim::kNoTarget) ctx.depot_lower_selected = true;
        if (selected_depot(true) != sim::kNoTarget) ctx.depot_raise_selected = true;
    }
    if (selected_loaded_transport() != sim::kNoTarget) ctx.transport_unload_selected = true;
    return ctx;
}

std::uint32_t MeleeGame::selected_command_center() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "CommandCenter") return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_orbital() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "OrbitalCommand") return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_completed_engbay() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "EngineeringBay") return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_starport() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "Starport") return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_gateway() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "Gateway") return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_liberator() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "Liberator") return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_liberator_ag() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.catalog_id == "LiberatorAG") return id;
    }
    return sim::kNoTarget;
}

std::vector<std::uint32_t> MeleeGame::selected_merge_templar() const {
    std::vector<std::uint32_t> out;
    if (cfg_.human_player < 0 || !cat_) return out;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (!cat_->merge_ability(u.catalog_id).empty()) out.push_back(id);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::uint32_t MeleeGame::selected_queen() const {
    if (cfg_.human_player < 0 || !cat_) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (cat_->inject_ability(u.catalog_id).found) return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_infestor() const {
    if (cfg_.human_player < 0 || !cat_) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (cat_->infested_terran_ability(u.catalog_id).found) return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_liftoff_structure(bool flying) const {
    if (cfg_.human_player < 0 || !cat_) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.flying != flying) continue;
        if (!cat_->liftoff_ability(u.catalog_id).empty()) return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_depot(bool lowered) const {
    if (cfg_.human_player < 0 || !cat_) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.lowered != lowered) continue;
        if (!cat_->depot_lower_ability(u.catalog_id).empty()) return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_loaded_transport() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.cargo_space > 0 && world_.cargo_used(id) > 0) return id;
    }
    return sim::kNoTarget;
}

std::vector<std::uint32_t> MeleeGame::selected_burrowers(bool burrowed) const {
    std::vector<std::uint32_t> out;
    if (cfg_.human_player < 0 || !cat_) return out;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.burrowed != burrowed) continue;
        if (!cat_->burrow_ability(u.catalog_id).empty()) out.push_back(id);
    }
    return out;
}

std::uint32_t MeleeGame::selected_techlab_producer() const {
    if (cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player || u.constructing) continue;
        if (u.addon == sim::World::kAddonTechLab && u.addon_building == 0) return id;
    }
    return sim::kNoTarget;
}

std::uint32_t MeleeGame::selected_addon_producer() const {
    if (!cat_ || cfg_.human_player < 0) return sim::kNoTarget;
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (!u.alive || u.player != cfg_.human_player) continue;
        if (u.speed.raw() != 0 || u.constructing) continue;
        if (u.addon != sim::World::kAddonNone || u.addon_building != 0) continue;
        if (cat_->unit_can_build_addon(u.catalog_id)) return id;
    }
    return sim::kNoTarget;
}

void MeleeGame::setup(const MeleeSetup& setup, const MeleeModels* models,
                      const formats::Model* ground, float map_w, float map_h,
                      const sim::NavGrid* nav) {
    ground_ = ground;
    map_w_ = map_w;
    map_h_ = map_h;
    if (map_w > 0.0f && map_h > 0.0f)
        world_.creep().configure(int(map_w) + 1, int(map_h) + 1, sim::Fixed::from_int(1),
                                 sim::Fixed(), sim::Fixed());
    layout_ = build_melee(world_, setup);
    for (const sim::Unit& u : world_.units())
        if (u.alive && u.catalog_id == "XelNagaTower")
            world_.register_watchtower(u.id, sim::Fixed::from_double(2.5));
    if (nav) world_.nav() = *nav;
    if (world_.nav().configured())
        for (const sim::Unit& u : world_.units()) {
            if (!u.alive || u.catalog_id.find("Destructible") == std::string::npos) continue;
            const sim::Fixed r = u.radius;
            int cx0, cy0, cx1, cy1;
            world_.nav().to_cell(u.x - r, u.y - r, cx0, cy0);
            world_.nav().to_cell(u.x + r, u.y + r, cx1, cy1);
            world_.register_destructible(u.id, cx0, cy0, cx1, cy1);
        }
    {
        std::vector<std::uint32_t> towers;
        for (const sim::Unit& u : world_.units()) {
            const std::string& c = u.catalog_id;
            if (u.alive && c.rfind("Collapsible", 0) == 0 && c.find("Tower") != std::string::npos &&
                c.find("Debris") == std::string::npos && c.find("PushUnit") == std::string::npos)
                towers.push_back(u.id);
        }
        const int n = int(towers.size());
        std::vector<int> parent(n);
        for (int i = 0; i < n; ++i) parent[i] = i;
        auto find = [&](int i) {
            while (parent[i] != i) { parent[i] = parent[parent[i]]; i = parent[i]; }
            return i;
        };
        const sim::Fixed r2 = sim::Fixed::from_int(15) * sim::Fixed::from_int(15);
        for (int i = 0; i < n; ++i)
            for (int j = i + 1; j < n; ++j) {
                const sim::Unit& a = world_.unit(towers[i]);
                const sim::Unit& b = world_.unit(towers[j]);
                const sim::Fixed dx = a.x - b.x, dy = a.y - b.y;
                if (dx * dx + dy * dy <= r2) parent[find(i)] = find(j);
            }
        for (int root = 0; root < n; ++root) {
            if (find(root) != root) continue;
            std::vector<std::uint32_t> members;
            for (int i = 0; i < n; ++i)
                if (find(i) == root) members.push_back(towers[i]);
            world_.register_conjoined_group(std::move(members));
        }
    }
    world_.set_separation(true);
    world_.set_acceleration(true);
    world_.set_turn_limit(true);
    world_.enable_building_burndown();
    world_.enable_flee_response();
    world_.enable_multi_hit_armor();
    world_.set_projectile_travel(true);
    world_.set_attack_windup(true);
    world_.set_backswing(true);
    world_.set_fire_while_moving(true);
    world_.enable_point_defense();

    if (cfg_.human_player >= 0 && size_t(cfg_.human_player) < layout_.players.size())
        for (std::uint32_t id : layout_.players[size_t(cfg_.human_player)].workers)
            world_.stop(id);

    models_ = models;
    driver_.default_model = models ? models->model_for("Marine") : nullptr;
    for (uint32_t id = 0; id < layout_.unit_type.size(); ++id) {
        const formats::Model* m = models ? models->model_for(layout_.unit_type[id]) : nullptr;
        driver_.set_model(id, m);
    }
    producer_model_ = models ? models->model_for("Barracks") : nullptr;
    carry_model_ = models ? models->model_for("MineralField") : nullptr;
    bound_count_ = layout_.unit_type.size();
    spawn_worker_model_.assign(layout_.players.size(), models ? models->model_for("SCV") : nullptr);
    spawn_army_model_.assign(layout_.players.size(), driver_.default_model);
    spawn_producer_model_.assign(layout_.players.size(), producer_model_);

    ais_.clear();
    for (size_t p = 0; p < layout_.players.size(); ++p) {
        ai::MeleeConfig cfg = layout_.players[p].config;
        if (int(p) != cfg_.human_player) cfg.smart_combat = true;
        auto ai = std::make_unique<ai::MeleeAI>(int(p), cfg);
        ai->set_base(layout_.players[p].base);
        ais_.push_back(std::move(ai));
    }

    camctl_.yaw = 1.5707963f;
    camctl_.pitch = 1.0f;
    camctl_.mirror_x = true;

    if (cfg_.human_player >= 0 && size_t(cfg_.human_player) < setup.players.size()) {
        const MeleePlayerSetup& hp = setup.players[size_t(cfg_.human_player)];
        human_build_.producer_cost = hp.producer_mineral_cost;
        human_build_.army_cost = hp.army_mineral_cost;
        human_build_.worker_proto = hp.worker;
        human_build_.worker_cost = hp.worker_mineral_cost;
        human_build_.worker_build_time = hp.worker_build_time;
        human_build_.supply_proto = hp.supply_unit;
        human_build_.supply_cost = hp.supply_mineral_cost;
        human_build_.supply_build_time = hp.supply_build_time;
        human_build_.producer_proto = hp.producer;
        human_build_.producer_proto.speed = sim::Fixed();
        human_build_.producer_build_time = hp.producer_build_time;
    }

    if (cfg_.human_player >= 0 && size_t(cfg_.human_player) < setup.players.size()) {
        const MeleePos& st = setup.players[size_t(cfg_.human_player)].start;
        camctl_.focus = {float(st.x.to_double()), float(st.y.to_double()), 0.0f};
        camctl_.distance = 16.0f;
        framed_ = true;
    } else {
        render::Vec3 c{0, 0, 0};
        int n = 0;
        for (const Unit& u : world_.units()) {
            c.x += float(u.x.to_double());
            c.y += float(u.y.to_double());
            ++n;
        }
        if (n) {
            camctl_.focus = {c.x / float(n), c.y / float(n), 0};
            camctl_.distance = 40.0f;
        }
    }
}

void MeleeGame::enable_research(const data::Catalog& cat, const data::UpgradeCatalog& up,
                                std::vector<MeleeResearchDriver::ProtoNames> proto_names) {
    research_ = std::make_unique<MeleeResearchDriver>(cat, up);
    research_->set_world(&world_);
    cat_ = &cat;
    for (std::size_t p = 0; p < ais_.size(); ++p) {
        MeleeResearchDriver::ProtoNames names =
            p < proto_names.size() ? proto_names[p] : MeleeResearchDriver::ProtoNames{};
        research_->bind(*ais_[p], std::move(names));
    }
}

void MeleeGame::register_creep_masks(const data::Catalog& cat) {
    for (const std::string& id : cat.ids("CUnit")) {
        auto mask = cat.creep_source_footprint_offsets(id);
        if (!mask.empty()) world_.register_creep_mask(id, std::move(mask));
    }
}

void MeleeGame::enable_larva(const data::Catalog& cat) {
    std::unordered_map<std::string, int32_t> proto_idx;
    for (const MeleePlayerLayout& pl : layout_.players) {
        const std::uint32_t hall = pl.town_hall;
        if (hall >= world_.units().size() || !world_.unit(hall).alive) continue;
        const std::string hid = world_.unit(hall).catalog_id;
        if (hid.empty()) continue;
        const auto sb = cat.unit_spawn_behavior(hid);
        if (!sb) continue;
        auto it = proto_idx.find(sb->unit);
        int32_t idx;
        if (it == proto_idx.end()) {
            idx = world_.add_spawn_proto(make_unit(cat, sb->unit, 0));
            proto_idx.emplace(sb->unit, idx);
        } else {
            idx = it->second;
        }
        world_.begin_spawner(hall, idx, sb->max_count, sim::Fixed::from_double(sb->period),
                             sb->hard_cap);
    }
}

void MeleeGame::begin_research(int player, const std::string& id, sim::Fixed delay) {
    if (research_) research_->begin(player, id, delay);
}

void MeleeGame::maybe_ai_research(int player) {
    if (!research_ || !cat_ || player < 0 || std::size_t(player) >= ai_research_.size()) return;
    const AiResearchPlan& plan = ai_research_[player];
    if (plan.tech_id.empty() || plan.upgrades.empty()) return;
    std::string next;
    for (const std::string& tier : plan.upgrades) {
        if (research_->has(player, tier)) continue;
        if (research_->pending(player, tier)) return;
        next = tier;
        break;
    }
    if (next.empty()) return;
    bool tech_stands = false;
    for (const sim::Unit& u : world_.units())
        if (u.alive && u.player == player && !u.constructing && u.catalog_id == plan.tech_id) {
            tech_stands = true;
            break;
        }
    if (!tech_stands) return;
    const data::Catalog::ResearchInfo info = cat_->research_info(next);
    if (!info.found || info.time <= 0) return;
    sim::Player& pp = world_.player(player);
    if (pp.minerals < info.minerals || pp.gas < info.gas) return;
    pp.minerals -= info.minerals;
    pp.gas -= info.gas;
    research_->begin(player, next, sim::Fixed::from_double(info.time));
}

const std::vector<std::string>& MeleeGame::researched(int player) const {
    static const std::vector<std::string> kNone{};
    return research_ ? research_->researched(player) : kNone;
}

int MeleeGame::research_in_progress() const {
    return research_ ? research_->in_progress() : 0;
}

void MeleeGame::enable_collapsible_debris(const data::Catalog& cat) {
    if (world_.conjoined_count() == 0) return;
    sim::Unit debris = make_unit(cat, "CollapsibleRockTowerDebris", 0);
    debris.player = -1;
    debris.alive = true;
    world_.set_conjoined_debris_proto(world_.add_spawn_proto(std::move(debris)));
}

void MeleeGame::enable_ai_casting(const data::Catalog& cat,
                                  const std::vector<std::pair<std::string, std::string>>& regs) {
    cast_policy_ = std::make_unique<CastPolicy>(cat);
    cat_ = &cat;
    for (const auto& [unit_id, abil_id] : regs)
        cast_policy_->register_offensive(world_, unit_id, abil_id);
}

int MeleeGame::enable_ai_casting_from_roster(const data::Catalog& cat) {
    cast_policy_ = std::make_unique<CastPolicy>(cat);
    cat_ = &cat;
    int registered = 0;
    for (std::size_t i = 0; i < kNumCasterCompositions; ++i) {
        const CasterComposition& c = kCasterCompositions[i];
        const int before = registered;
        std::set<std::string> seen;
        for (const auto& [abil_id, cmd_index] : cat.unit_ability_commands(c.caster)) {
            (void)cmd_index;
            if (abil_id.empty() || !seen.insert(abil_id).second) continue;
            const auto* ce = cat.find("CAbil", abil_id);
            if (!ce || ce->tag != "CAbilEffectTarget") continue;
            if (cast_policy_->register_offensive(world_, c.caster, abil_id)) ++registered;
        }
        if (registered == before && cast_policy_->register_offensive(world_, c.caster, c.ability))
            ++registered;
    }
    struct SelfBuff {
        const char* unit;
        const char* abil;
    };
    static const SelfBuff kSelfBuffs[] = {{"Marine", "Stimpack"}, {"Marauder", "StimpackMarauder"}};
    for (const SelfBuff& sb : kSelfBuffs)
        if (cast_policy_->register_selfcast(world_, sb.unit, sb.abil, sim::Fixed::from_int(7)))
            ++registered;
    if (cast_policy_->register_support(world_, "Medivac", "MedivacHeal", sim::Fixed::from_int(6)))
        ++registered;
    if (cast_policy_->register_support(world_, "Queen", "Transfusion", sim::Fixed::from_int(7)))
        ++registered;
    return registered;
}

std::uint32_t MeleeGame::human_lead_caster() const {
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size()) continue;
        const sim::Unit& u = world_.units()[id];
        if (u.alive && u.player == cfg_.human_player) return id;
    }
    return sim::kNoTarget;
}

bool MeleeGame::human_cast(const std::string& abil_id, std::uint32_t target_id) {
    if (!cast_policy_) return false;
    const std::uint32_t caster = human_lead_caster();
    if (caster == sim::kNoTarget) return false;
    return cast_policy_->cast_now(world_, caster, abil_id, target_id);
}

bool MeleeGame::human_cast_point(const std::string& abil_id, sim::Fixed px, sim::Fixed py) {
    if (!cast_policy_) return false;
    const std::uint32_t caster = human_lead_caster();
    if (caster == sim::kNoTarget) return false;
    return cast_policy_->cast_now_point(world_, caster, abil_id, px, py);
}

std::vector<AbilityCardEntry> MeleeGame::selection_ability_entries() const {
    if (!cat_) return {};
    const std::uint32_t lead = human_lead_caster();
    if (lead == sim::kNoTarget) return {};
    const std::string& type = world_.units()[lead].catalog_id;
    if (type.empty()) return {};
    static const std::vector<std::string> kNoResearch;
    const std::vector<std::string>& researched =
        research_ ? research_->researched(cfg_.human_player) : kNoResearch;
    return build_ability_card_entries(*cat_, type, researched);
}

std::vector<AbilityCardEntry> MeleeGame::merge_selection_abilities(CommandCard& card) const {
    std::vector<AbilityCardEntry> out;
    if (!cat_) return out;
    std::vector<AbilityCardEntry> entries = selection_ability_entries();
    if (entries.empty()) return out;
    auto occupied = [&](int s) {
        return s < 0 || s >= CommandCard::kSlots || s == CommandCard::kCancelSlot ||
               card.slots[s].state != ButtonState::Empty;
    };
    for (AbilityCardEntry& e : entries) {
        int slot = e.slot;
        if (occupied(slot)) {
            slot = -1;
            for (int s = 0; s < CommandCard::kCancelSlot; ++s)
                if (!occupied(s)) { slot = s; break; }
        }
        if (slot < 0) continue;
        e.slot = slot;
        CommandButton btn;
        btn.icon = e.icon;
        btn.name = e.name;
        btn.ability = 0;
        btn.state = resolve_button_state(e.requirement_met, 0.0f);
        card.slots[slot] = btn;
        out.push_back(e);
    }
    return out;
}

std::vector<AbilityCardEntry> MeleeGame::selection_card_abilities() {
    CommandCard card = build_melee_command_card(command_context());
    return merge_selection_abilities(card);
}

void MeleeGame::update_camera(const render::InputState& in, float dt_s) {
    if (in.wheel != 0.0f) camctl_.zoom(-in.wheel * cfg_.zoom_step);
    float px = 0, py = 0;
    if (in.key_pan_left) px -= 1;
    if (in.key_pan_right) px += 1;
    if (in.key_pan_down) py -= 1;
    if (in.key_pan_up) py += 1;
    if (in.view_w && in.view_h) {
        const float bar_top = float(int(in.view_h) - hud_panel_height(in.view_h));
        const bool over_bar = in.mouse_y >= bar_top;
        if (in.mouse_x <= cfg_.edge_margin) px -= 1;
        else if (in.mouse_x >= float(in.view_w) - cfg_.edge_margin) px += 1;
        if (in.mouse_y <= cfg_.edge_margin) py += 1;
        else if (in.mouse_y >= float(in.view_h) - cfg_.edge_margin && !over_bar) py -= 1;
    }
    if (px != 0.0f || py != 0.0f)
        camctl_.pan(px * cfg_.pan_speed * dt_s, py * cfg_.pan_speed * dt_s);
    float o = 0;
    if (in.key_orbit_left) o += 1;
    if (in.key_orbit_right) o -= 1;
    if (o != 0.0f) camctl_.orbit(o * cfg_.orbit_speed * dt_s);
    if (in.middle_down && (in.mouse_dx != 0.0f || in.mouse_dy != 0.0f))
        camctl_.orbit(-in.mouse_dx * cfg_.orbit_mouse, -in.mouse_dy * cfg_.orbit_mouse);
}

void MeleeGame::handle_commands(const render::InputState& in, float aspect) {
    if (cfg_.human_player < 0 || !in.view_w || !in.view_h) return;
    render::Mat4 mvp = camctl_.view_proj(aspect);
    Control ctrl(mvp, in.view_w, in.view_h, cfg_.human_player);

    const HudRect mm = hud_minimap_rect(in.view_w, in.view_h);
    const bool over_minimap = in.mouse_x >= float(mm.x0) && in.mouse_x < float(mm.x1) &&
                              in.mouse_y >= float(mm.y0) && in.mouse_y < float(mm.y1);
    const bool over_bar = in.mouse_y >= float(int(in.view_h) - hud_panel_height(in.view_h));
    auto minimap_world = [&](float& wx, float& wy) {
        float fx = (in.mouse_x - float(mm.x0)) / float(std::max(mm.w() - 1, 1));
        float fy = (in.mouse_y - float(mm.y0)) / float(std::max(mm.h() - 1, 1));
        wx = std::clamp(fx, 0.0f, 1.0f) * map_w_;
        wy = std::clamp(fy, 0.0f, 1.0f) * map_h_;
    };

    if (!selection_.empty()) {
        if (in.key_hold && !prev_keys_.hold)
            for (std::uint32_t id : selection_)
                world_.issue(id, sim::Order{sim::OrderType::HoldPosition});
        if (in.key_stop && !prev_keys_.stop)
            for (std::uint32_t id : selection_) world_.stop(id);
        if (in.key_patrol && !prev_keys_.patrol && !over_bar) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy))
                for (std::uint32_t id : selection_) {
                    if (id >= world_.units().size()) continue;
                    const Unit& u = world_.unit(id);
                    sim::Order o;
                    o.type = sim::OrderType::Patrol;
                    o.tx = wx; o.ty = wy; o.px = u.x; o.py = u.y;
                    world_.issue(id, o);
                }
        }
    }
    prev_keys_.hold = in.key_hold;
    prev_keys_.stop = in.key_stop;
    prev_keys_.patrol = in.key_patrol;

    if (pending_build_) {
        sim::Fixed wx, wy;
        if (!over_bar && ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
            build_px_ = wx;
            build_py_ = wy;
            bool clear = wx.to_double() >= 0.0 && wy.to_double() >= 0.0 &&
                         wx.to_double() <= double(map_w_) && wy.to_double() <= double(map_h_);
            if (clear)
                for (const Unit& u : world_.units()) {
                    if (!u.alive) continue;
                    const sim::Fixed dx = u.x - wx, dy = u.y - wy;
                    const sim::Fixed rr = build_proto_.radius + u.radius;
                    if (dx * dx + dy * dy < rr * rr) { clear = false; break; }
                }
            if (clear && build_proto_.requires_creep && !world_.creep().is_creep(wx, wy)) clear = false;
            if (clear && build_proto_.needs_power &&
                !world_.powered_at(wx, wy, cfg_.human_player))
                clear = false;
            build_valid_ = clear && world_.player(cfg_.human_player).minerals >= build_cost_;
        } else {
            build_valid_ = false;
        }
    }

    if (over_minimap && map_w_ > 0.0f && map_h_ > 0.0f) {
        if (in.left_pressed) {
            float wx, wy; minimap_world(wx, wy);
            camctl_.focus = {wx, wy, 0.0f};
            dragging_ = false;
        }
        if (in.right_pressed && !selection_.empty()) {
            float wx, wy; minimap_world(wx, wy);
            sim::Order o;
            o.type = in.shift ? sim::OrderType::AttackMove : sim::OrderType::Move;
            o.tx = sim::Fixed::from_ratio(int(std::lround(double(wx) * 64.0)), 64);
            o.ty = sim::Fixed::from_ratio(int(std::lround(double(wy) * 64.0)), 64);
            for (std::uint32_t id : selection_) world_.issue(id, o);
        }
        return;
    }

    if (in.left_pressed) {
        if (over_bar) {
            dragging_ = false;
            CommandCard card = build_melee_command_card(command_context());
            const std::vector<AbilityCardEntry> card_abils = merge_selection_abilities(card);
            for (int i = 0; i < kCommandCols * kCommandRows && i < CommandCard::kSlots; ++i) {
                const HudRect c = hud_command_cell(in.view_w, in.view_h, i);
                if (in.mouse_x >= float(c.x0) && in.mouse_x < float(c.x1) &&
                    in.mouse_y >= float(c.y0) && in.mouse_y < float(c.y1)) {
                    bool routed = false;
                    for (const AbilityCardEntry& ab : card_abils)
                        if (ab.slot == i) {
                            if (ab.requirement_met && cat_) {
                                pending_attack_ = pending_patrol_ = false;
                                cancel_ability_cast();
                                const AbilTargetMode m = ability_target_mode(*cat_, ab.ability);
                                if (m == AbilTargetMode::Instant)
                                    human_cast(ab.ability, human_lead_caster());
                                else {
                                    begin_ability_cast(ab.ability, ab.command);
                                    pending_cast_mode_ = m;
                                }
                            }
                            routed = true;
                            break;
                        }
                    if (routed) break;
                    cancel_ability_cast();
                    switch (card.slots[i].ability) {
                        case kMeleeAbilTrainWorker: ui_trainworker_ = true; break;
                        case kMeleeAbilBuildProducer: ui_build_ = true; break;
                        case kMeleeAbilBuildSupply: ui_buildsupply_ = true; break;
                        case kMeleeAbilBuildTechLab: ui_buildtechlab_ = true; break;
                        case kMeleeAbilBuildReactor: ui_buildreactor_ = true; break;
                        case kMeleeAbilTrainTechUnit: ui_traintech_ = true; break;
                        case kMeleeAbilBuildFactory: ui_buildfactory_ = true; break;
                        case kMeleeAbilBuildStarport: ui_buildstarport_ = true; break;
                        case kMeleeAbilTrainAir: ui_trainair_ = true; break;
                        case kMeleeAbilBuildPylon: ui_buildpylon_ = true; break;
                        case kMeleeAbilBuildGateway: ui_buildgateway_ = true; break;
                        case kMeleeAbilTrainZealot: ui_trainzealot_ = true; break;
                        case kMeleeAbilBuildTownHall: pending_town_hall_ = true; break;
                        case kMeleeAbilMorphOrbital: ui_morphorbital_ = true; break;
                        case kMeleeAbilBuildEngBay: ui_buildengbay_ = true; break;
                        case kMeleeAbilBuildRefinery:
                            pending_refinery_ = true;
                            pending_build_ = false;
                            pending_attack_ = pending_patrol_ = false;
                            break;
                        case kMeleeAbilResearchInfWeapons: ui_research_ = true; break;
                        case kMeleeAbilTrainArmy: ui_train_ = true; break;
                        case kMeleeAbilCalldownMule:
                            pending_orbital_ = selected_orbital();
                            if (pending_orbital_ != sim::kNoTarget) pending_mule_ = true;
                            pending_attack_ = pending_patrol_ = false;
                            break;
                        case kMeleeAbilLiberatorDefender:
                            pending_liberator_ = selected_liberator();
                            if (pending_liberator_ != sim::kNoTarget) pending_defender_ = true;
                            pending_mule_ = false;
                            pending_attack_ = pending_patrol_ = false;
                            break;
                        case kMeleeAbilLiberatorFighter: ui_fightermode_ = true; break;
                        case kMeleeAbilSpawnLarva:
                            pending_queen_ = selected_queen();
                            if (pending_queen_ != sim::kNoTarget) pending_inject_ = true;
                            pending_mule_ = pending_defender_ = false;
                            pending_attack_ = pending_patrol_ = false;                            pending_infested_ = false;
                            break;
                        case kMeleeAbilSpawnInfestedTerran:
                            pending_mule_ = pending_defender_ = false;
                            pending_inject_ = false;
                            pending_attack_ = pending_patrol_ = false;
                            pending_infestor_ = selected_infestor();
                            pending_infested_ = (pending_infestor_ != sim::kNoTarget);
                            break;
                        case kMeleeAbilMergeArchon: ui_mergearchon_ = true; break;
                        case kMeleeAbilBurrow: ui_burrow_ = true; break;
                        case kMeleeAbilUnburrow: ui_unburrow_ = true; break;
                        case kMeleeAbilLiftOff: ui_liftoff_ = true; break;
                        case kMeleeAbilLowerDepot: ui_lowerdepot_ = true; break;
                        case kMeleeAbilRaiseDepot: ui_raisedepot_ = true; break;
                        case kMeleeAbilLand:
                            pending_land_unit_ = selected_liftoff_structure(true);
                            if (pending_land_unit_ != sim::kNoTarget) pending_land_ = true;
                            pending_mule_ = pending_defender_ = false;
                            pending_inject_ = pending_attack_ = pending_patrol_ = false;                            pending_infested_ = false;
                            break;
                        case kMeleeAbilUnloadAll:
                            pending_unload_unit_ = selected_loaded_transport();
                            if (pending_unload_unit_ != sim::kNoTarget) pending_unload_ = true;
                            pending_mule_ = pending_defender_ = false;
                            pending_land_ = pending_inject_ = false;
                            pending_attack_ = pending_patrol_ = false;
                            break;
                        case kMeleeAbilStop:
                            for (std::uint32_t id : selection_) world_.stop(id);
                            pending_attack_ = pending_patrol_ = pending_unload_ = false;
                            break;
                        case kMeleeAbilHold:
                            for (std::uint32_t id : selection_)
                                world_.issue(id, sim::Order{sim::OrderType::HoldPosition});
                            pending_attack_ = pending_patrol_ = pending_unload_ = false;
                            break;
                        case kMeleeAbilAttack:
                            pending_attack_ = true;
                            pending_patrol_ = pending_unload_ = false;
                            break;
                        case kMeleeAbilPatrol:
                            pending_patrol_ = true;
                            pending_attack_ = pending_unload_ = false;
                            break;
                        default: break;
                    }
                    break;
                }
            }
        } else if (pending_build_) {
            dragging_ = false;
            bool placed = false;
            std::uint32_t bw = sim::kNoTarget;
            for (std::uint32_t id : selection_)
                if (id < world_.units().size() && world_.unit(id).alive &&
                    world_.unit(id).is_worker && world_.unit(id).player == cfg_.human_player) {
                    bw = id;
                    break;
                }
            sim::Fixed wx, wy;
            if (bw != sim::kNoTarget &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                bool clear = wx.to_double() >= 0.0 && wy.to_double() >= 0.0 &&
                             wx.to_double() <= double(map_w_) && wy.to_double() <= double(map_h_);
                if (clear)
                    for (const Unit& u : world_.units()) {
                        if (!u.alive) continue;
                        const sim::Fixed dx = u.x - wx, dy = u.y - wy;
                        const sim::Fixed rr = build_proto_.radius + u.radius;
                        if (dx * dx + dy * dy < rr * rr) { clear = false; break; }
                    }
                if (clear && build_proto_.requires_creep && !world_.creep().is_creep(wx, wy))
                    clear = false;
                if (clear && build_proto_.needs_power && !world_.powered_at(wx, wy, cfg_.human_player))
                    clear = false;
                if (clear) {
                    build_queue_.push_back({bw, wx, wy, build_proto_, build_cost_, build_time_,
                                            build_proto_.supply_provides.raw() == 0});
                    placed = true;
                }
            }
            if ((placed && !in.shift) || bw == sim::kNoTarget) pending_build_ = false;
        } else if (pending_refinery_) {
            dragging_ = false;
            std::uint32_t bw = sim::kNoTarget;
            for (std::uint32_t id : selection_)
                if (id < world_.units().size() && world_.unit(id).alive &&
                    world_.unit(id).is_worker && world_.unit(id).player == cfg_.human_player) {
                    bw = id;
                    break;
                }
            sim::Fixed wx, wy;
            if (bw != sim::kNoTarget && cat_ &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                std::uint32_t geyser = sim::kNoTarget;
                sim::Fixed best_d2;
                for (const Unit& u : world_.units()) {
                    if (!u.alive || u.player >= 0 || !u.is_gas) continue;
                    const sim::Fixed r = u.radius + sim::Fixed::from_int(2);
                    const sim::Fixed dx = u.x - wx, dy = u.y - wy, d2 = dx * dx + dy * dy;
                    if (d2 > r * r) continue;
                    if (geyser == sim::kNoTarget || d2 < best_d2) { geyser = u.id; best_d2 = d2; }
                }
                if (geyser != sim::kNoTarget) {
                    const std::string gas = human_gas_structure();
                    const int cost = data::unit_cost(*cat_, gas).minerals;
                    if (world_.player(cfg_.human_player).minerals >= cost) {
                        Unit& gy = world_.unit(geyser);
                        sim::Unit proto = make_unit(*cat_, gas, cfg_.human_player);
                        proto.speed = sim::Fixed();
                        proto.is_gas = true;
                        proto.resource_left = gy.resource_left;
                        const sim::Fixed gpx = gy.x, gpy = gy.y;
                        gy.alive = false;
                        world_.issue_worker_build(bw, gpx, gpy, proto, cost, 0, sim::Fixed::from_int(30),
                                                  sim::Fixed::from_int(2), human_build_verb_);
                    }
                }
            }
            pending_refinery_ = false;
        } else if (pending_mule_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (pending_orbital_ < world_.units().size() && world_.unit(pending_orbital_).alive &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                calldown_mule(world_, mule_system_, pending_orbital_, wx, wy);
            }
            pending_mule_ = false;
            pending_orbital_ = sim::kNoTarget;
        } else if (pending_defender_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (cat_ && pending_liberator_ < world_.units().size() &&
                world_.unit(pending_liberator_).alive &&
                world_.unit(pending_liberator_).catalog_id == "Liberator" &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                if (liberator_ag_proto_ < 0)
                    liberator_ag_proto_ =
                        world_.add_spawn_proto(make_unit(*cat_, "LiberatorAG", cfg_.human_player));
                const auto ms = cat_->morph_spec("LiberatorMorphtoAG");
                const sim::Fixed t =
                    ms ? sim::Fixed::from_double(ms->morph_time) : sim::Fixed::from_double(4.0417);
                world_.begin_morph(pending_liberator_, liberator_ag_proto_, t);
                world_.set_zone(pending_liberator_, wx, wy, sim::Fixed::from_int(5));
            }
            pending_defender_ = false;
            pending_liberator_ = sim::kNoTarget;
        } else if (pending_inject_) {
            dragging_ = false;
            const std::uint32_t hall = ctrl.pick_own(world_, ScreenPoint{in.mouse_x, in.mouse_y});
            if (cat_ && pending_queen_ < world_.units().size() && world_.unit(pending_queen_).alive &&
                hall != sim::kNoTarget && hall < world_.units().size() && world_.unit(hall).alive &&
                world_.unit(hall).spawner_proto >= 0) {
                const auto iv = cat_->inject_ability(world_.unit(pending_queen_).catalog_id);
                const sim::Fixed iv_energy = ability_energy_cost(*cat_, iv.ability);
                if (iv.found && iv.count > 0 && iv.delay > 0) {
                    world_.cast_inject(pending_queen_, hall, iv_energy,
                                       world_.unit(hall).spawner_proto, iv.count,
                                       sim::Fixed::from_double(iv.delay),
                                       sim::Fixed::from_double(iv.stun));
                }
            }
            pending_inject_ = false;
            pending_queen_ = sim::kNoTarget;
        } else if (pending_infested_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (cat_ && pending_infestor_ < world_.units().size() && world_.unit(pending_infestor_).alive &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                const auto it = cat_->infested_terran_ability(world_.unit(pending_infestor_).catalog_id);
                const sim::Fixed cost = sim::Fixed::from_double(it.energy);
                const sim::Fixed cd = ability_cooldown_sec(*cat_, it.ability);
                const bool ready =
                    world_.ability_cooldown_left(pending_infestor_, kAbilCdSlotInfestedTerran).raw() <= 0;
                const bool afford = cost.raw() <= 0 || world_.unit(pending_infestor_).energy >= cost;
                if (it.found && !it.unit.empty() && ready && afford) {
                    const int n = it.count < 1 ? 1 : it.count;
                    for (int k = 0; k < n; ++k) {
                        sim::Unit u = make_unit(*cat_, it.unit, cfg_.human_player);
                        u.x = wx + sim::Fixed::from_double(n == 1 ? 0.0 : 0.75 * std::cos(2.0 * 3.14159265 * k / n));
                        u.y = wy + sim::Fixed::from_double(n == 1 ? 0.0 : 0.75 * std::sin(2.0 * 3.14159265 * k / n));
                        u.time_life = sim::Fixed::from_double(it.life);
                        world_.add_unit(u);
                    }
                    if (cost.raw() > 0) world_.spend_energy(pending_infestor_, cost);
                    world_.arm_ability_cooldown(pending_infestor_, kAbilCdSlotInfestedTerran, cd);
                }
            }
            pending_infested_ = false;
            pending_infestor_ = sim::kNoTarget;
        } else if (pending_land_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (pending_land_unit_ < world_.units().size() && world_.unit(pending_land_unit_).alive &&
                world_.unit(pending_land_unit_).flying &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                const bool in_bounds = wx.to_double() >= 0.0 && wy.to_double() >= 0.0 &&
                                       wx.to_double() <= double(map_w_) && wy.to_double() <= double(map_h_);
                if (in_bounds && world_.land(pending_land_unit_, wx, wy)) {
                    pending_land_ = false;
                    pending_land_unit_ = sim::kNoTarget;
                }
            } else {
                pending_land_ = false;
                pending_land_unit_ = sim::kNoTarget;
            }
        } else if (pending_unload_) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (pending_unload_unit_ < world_.units().size() && world_.unit(pending_unload_unit_).alive &&
                ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy))
                world_.unload_all_at(pending_unload_unit_, wx, wy);
            pending_unload_ = false;
            pending_unload_unit_ = sim::kNoTarget;
        } else if ((pending_attack_ || pending_patrol_) && !selection_.empty()) {
            dragging_ = false;
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy))
                for (std::uint32_t id : selection_) {
                    if (id >= world_.units().size()) continue;
                    sim::Order o;
                    if (pending_attack_) {
                        o.type = sim::OrderType::AttackMove;
                        o.tx = wx; o.ty = wy;
                    } else {
                        const Unit& u = world_.unit(id);
                        o.type = sim::OrderType::Patrol;
                        o.tx = wx; o.ty = wy; o.px = u.x; o.py = u.y;
                    }
                    world_.issue(id, o);
                }
            pending_attack_ = pending_patrol_ = false;
        } else if (has_pending_ability_cast()) {
            dragging_ = false;
            if (pending_cast_mode_ == AbilTargetMode::Point) {
                sim::Fixed wx, wy;
                if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy))
                    cast_pending_at_point(wx, wy);
            } else {
                std::uint32_t tgt = ctrl.pick_target(world_, ScreenPoint{in.mouse_x, in.mouse_y});
                if (tgt == sim::kNoTarget)
                    tgt = ctrl.pick_own(world_, ScreenPoint{in.mouse_x, in.mouse_y});
                if (tgt != sim::kNoTarget) cast_pending_at_target(tgt);
            }
        } else {
            dragging_ = true;
            drag_x0_ = in.mouse_x;
            drag_y0_ = in.mouse_y;
        }
    }
    if (in.left_released && dragging_) {
        dragging_ = false;
        float x0 = drag_x0_, y0 = drag_y0_, x1 = in.mouse_x, y1 = in.mouse_y;
        if (std::fabs(x1 - x0) < 6.0f && std::fabs(y1 - y0) < 6.0f) {
            x0 = in.mouse_x - 6.0f; x1 = in.mouse_x + 6.0f;
            y0 = in.mouse_y - 6.0f; y1 = in.mouse_y + 6.0f;
        }
        selection_ = ctrl.box_select(world_, ScreenRect{x0, y0, x1, y1});
        if (selection_.empty()) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, wx, wy)) {
                for (const Unit& u : world_.units()) {
                    if (!u.alive || u.player >= 0) continue;
                    const sim::Fixed dx = u.x - wx, dy = u.y - wy;
                    const sim::Fixed rr = u.radius + sim::Fixed::from_ratio(1, 2);
                    if (dx * dx + dy * dy <= rr * rr) { selection_ = {u.id}; break; }
                }
            }
        }
    }

    if (in.right_pressed && pending_build_) pending_build_ = false;
    if (in.right_pressed && pending_refinery_) pending_refinery_ = false;

    if (in.right_pressed && !pending_build_ && !pending_refinery_ && !selection_.empty() && !over_bar) {
        std::uint32_t node = sim::kNoTarget;
        sim::Fixed gx, gy;
        if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, gx, gy)) {
            sim::Fixed best_d2;
            for (const Unit& u : world_.units()) {
                const bool neutral_resource = u.alive && u.player < 0;
                const bool own_gas = u.alive && u.is_gas && u.player == cfg_.human_player;
                if (!neutral_resource && !own_gas) continue;
                sim::Fixed r = u.radius + sim::Fixed::from_int(2);
                sim::Fixed dx = u.x - gx, dy = u.y - gy, d2 = dx * dx + dy * dy;
                if (d2 > r * r) continue;
                if (node == sim::kNoTarget || d2 < best_d2) { node = u.id; best_d2 = d2; }
            }
        }
        std::uint32_t resume = sim::kNoTarget;
        if (node == sim::kNoTarget) {
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.player != cfg_.human_player || !world_.is_halted(u.id)) continue;
                const sim::Fixed r = u.radius + sim::Fixed::from_int(1);
                const sim::Fixed dx = u.x - gx, dy = u.y - gy;
                if (dx * dx + dy * dy <= r * r) { resume = u.id; break; }
            }
        }

        std::uint32_t transport = sim::kNoTarget;
        if (node == sim::kNoTarget && resume == sim::kNoTarget && cfg_.human_player >= 0) {
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.cargo_space <= 0) continue;
                if (!world_.are_allied(u.player, cfg_.human_player)) continue;
                const sim::Fixed r = u.radius + sim::Fixed::from_int(1);
                const sim::Fixed dx = u.x - gx, dy = u.y - gy;
                if (dx * dx + dy * dy <= r * r) { transport = u.id; break; }
            }
        }

        if (resume != sim::kNoTarget) {
            for (std::uint32_t id : selection_)
                if (id < world_.units().size() && world_.unit(id).alive &&
                    world_.unit(id).is_worker) { world_.assign_builder(id, resume); break; }
        } else if (transport != sim::kNoTarget) {
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size() || id == transport) continue;
                const Unit& p = world_.unit(id);
                if (!p.alive || p.player != cfg_.human_player || p.cargo_size <= 0 || p.cargo_space > 0)
                    continue;
                world_.issue(id, sim::Order{sim::OrderType::Load, sim::Fixed(), sim::Fixed(), transport},
                             in.shift);
            }
        } else if (node != sim::kNoTarget && cfg_.human_player >= 0 &&
                   size_t(cfg_.human_player) < layout_.players.size()) {
            const std::uint32_t depot = layout_.players[size_t(cfg_.human_player)].base.depot;
            const bool gas_node = node < world_.units().size() && world_.unit(node).is_gas;
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                const bool is_scv = id < layout_.unit_type.size() && layout_.unit_type[id] == "SCV";
                if (is_scv || world_.unit(id).is_worker) {
                    Unit& w = world_.unit(id);
                    w.home_depot = depot;
                    w.gather_target = node;
                    w.gathers_gas = gas_node;
                    world_.issue(id, sim::Order{sim::OrderType::Gather, sim::Fixed(),
                                                sim::Fixed(), sim::kNoTarget});
                }
            }
        } else {
            ctrl.issue_command(world_, selection_, ScreenPoint{in.mouse_x, in.mouse_y}, in.shift);
        }
    }
}

std::uint32_t MeleeGame::spawn(sim::Unit proto, const std::string& token,
                               const formats::Model* model, sim::Fixed x, sim::Fixed y) {
    proto.x = x;
    proto.y = y;
    std::uint32_t id = world_.add_unit(proto);
    if (layout_.unit_type.size() <= id) layout_.unit_type.resize(id + 1);
    layout_.unit_type[id] = token;
    driver_.set_model(id, model);
    if (bound_count_ <= id) bound_count_ = id + 1;
    return id;
}

void MeleeGame::snapshot_positions(std::vector<render::Vec3>& out) const {
    const std::vector<Unit>& units = world_.units();
    if (out.size() < units.size()) out.resize(units.size());
    for (const Unit& u : units)
        out[u.id] = {float(u.x.to_double()), float(u.y.to_double()), 0.0f};
}

void MeleeGame::emit_scene(std::vector<render::SceneInstance>& scene) {
    scene.clear();
    if (ground_) {
        render::SceneInstance g;
        g.model = ground_;
        g.world = render::Mat4::identity();
        g.team = {0, 0, 0, -1.0f};
        g.seq = -1;
        scene.push_back(g);
    }
    const float tick_ms =
        (1000.0f / float(sim::World::kTicksPerSecond)) / float(clock_.multiplier(clock_.speed()));
    const float alpha = tick_ms > 0.0f ? std::min(1.0f, accum_ms_ / tick_ms) : 0.0f;
    for (const Unit& u : world_.units()) {
        if (!u.alive) continue;
        render::SceneInstance inst;
        const formats::Model* m =
            (u.id < driver_.actors.size()) ? driver_.actors[u.id].model : nullptr;
        inst.model = m;
        const float ms = 1.0f;
        const float native_span = cached_diameter(m);
        const float visual_r =
            native_span > 1e-3f ? native_span * 0.5f : float(u.radius.to_double());
        float wx = float(u.x.to_double()), wy = float(u.y.to_double());
        if (u.id < prev_pos_.size() && u.id < curr_pos_.size()) {
            const render::Vec3& p = prev_pos_[u.id];
            const render::Vec3& c = curr_pos_[u.id];
            wx = p.x + (c.x - p.x) * alpha;
            wy = p.y + (c.y - p.y) * alpha;
        }
        inst.world = render::translation(wx, wy, 0.0f) * render::scaling(ms, ms, ms);
        if (u.id < driver_.actors.size() && m) {
            render::ActorInstance& a = driver_.actors[u.id];
            inst.seq = a.sequence_of(a.current());
            inst.time_ms = a.clock;
        }
        bool selected = false;
        for (uint32_t s : selection_)
            if (s == u.id) { selected = true; break; }
        inst.team = team_colour(u.player, selected);
        scene.push_back(inst);

        if (u.carrying > 0 && carry_model_) {
            float fx = 0.0f, fy = 0.0f;
            if (u.id < prev_pos_.size() && u.id < curr_pos_.size()) {
                fx = curr_pos_[u.id].x - prev_pos_[u.id].x;
                fy = curr_pos_[u.id].y - prev_pos_[u.id].y;
            }
            const float fl = std::sqrt(fx * fx + fy * fy);
            const float off = visual_r + 0.15f;
            const float ox = fl > 1e-3f ? fx / fl * off : off;
            const float oy = fl > 1e-3f ? fy / fl * off : 0.0f;
            const float cs = 0.14f;
            render::SceneInstance chunk;
            chunk.model = carry_model_;
            chunk.world = render::translation(wx + ox, wy + oy, 0.55f) *
                          render::scaling(cs, cs, cs);
            const bool gas_trip = false;
            chunk.team = gas_trip ? std::array<float, 4>{0.2f, 0.9f, 0.35f, 2.0f}
                                  : std::array<float, 4>{0, 0, 0, -1.0f};
            chunk.seq = -1;
            scene.push_back(chunk);
        }

        if (selected) {
            const float rr = visual_r + 0.12f;
            const float band = 0.09f;
            float inner = rr > band ? (rr - band) / rr : 0.0f;
            const int key = int(inner * 200.0f + 0.5f);
            static std::vector<std::pair<int, formats::Model>> ring_cache;
            const formats::Model* rm = nullptr;
            for (auto& e : ring_cache)
                if (e.first == key) { rm = &e.second; break; }
            if (!rm) {
                ring_cache.emplace_back(key, render::make_ring_model(inner));
                rm = &ring_cache.back().second;
            }
            std::array<float, 4> rc = team_colour(u.player, false);
            if (rc[3] < 0.0f) rc = {0.9f, 0.85f, 0.30f, 1.0f};
            rc[3] = 2.0f;
            render::SceneInstance ring;
            ring.model = rm;
            ring.world = render::translation(wx, wy, 0.05f) * render::scaling(rr, rr, 1.0f);
            ring.team = rc;
            ring.seq = -1;
            scene.push_back(ring);
        }
    }
}

void MeleeGame::set_spawn_models(int player, const formats::Model* worker,
                                 const formats::Model* army, const formats::Model* producer) {
    if (player < 0) return;
    const size_t p = size_t(player);
    if (spawn_worker_model_.size() <= p) spawn_worker_model_.resize(p + 1, nullptr);
    if (spawn_army_model_.size() <= p) spawn_army_model_.resize(p + 1, nullptr);
    if (spawn_producer_model_.size() <= p) spawn_producer_model_.resize(p + 1, nullptr);
    if (worker) spawn_worker_model_[p] = worker;
    if (army) spawn_army_model_[p] = army;
    if (producer) spawn_producer_model_[p] = producer;
}

bool MeleeGame::step(const render::InputState& in, float dt_ms,
                     std::vector<render::SceneInstance>& scene, render::Camera& cam) {
    const float dt_s = dt_ms / 1000.0f;
    const float aspect = in.view_h ? float(in.view_w) / float(in.view_h) : 1.0f;

    if (dt_ms > 0.0f) {
        const float inst = 1000.0f / dt_ms;
        fps_smooth_ = fps_smooth_ > 0.0f ? fps_smooth_ * 0.9f + inst * 0.1f : inst;
    }
    mouse_x_ = in.mouse_x;
    mouse_y_ = in.mouse_y;

    update_camera(in, dt_s);

    if (!framed_ && in.view_w && in.view_h) {
        std::vector<render::Vec3> pts;
        for (const Unit& u : world_.units())
            if (u.alive) pts.push_back({float(u.x.to_double()), float(u.y.to_double()), 0.0f});
        camctl_.fit(pts, aspect);
        framed_ = true;
    }

    handle_commands(in, aspect);

    if (cfg_.human_player >= 0 && size_t(cfg_.human_player) < layout_.players.size()) {
        const MeleePlayerLayout& pl = layout_.players[size_t(cfg_.human_player)];

        if (human_producer_ != sim::kNoTarget &&
            (human_producer_ >= world_.units().size() || !world_.unit(human_producer_).alive))
            human_producer_ = sim::kNoTarget;

        if (human_producer_ == sim::kNoTarget) {
            for (const Unit& u : world_.units())
                if (u.alive && u.player == cfg_.human_player && u.speed.raw() == 0 &&
                    !u.constructing && u.id != pl.base.depot && u.supply_provides.raw() == 0) {
                    human_producer_ = u.id;
                    human_producer_pending_ = false;
                    break;
                }
        }

        auto selected_worker = [&]() -> std::uint32_t {
            for (std::uint32_t id : selection_)
                if (id < world_.units().size() && world_.unit(id).alive &&
                    world_.unit(id).is_worker && world_.unit(id).player == cfg_.human_player)
                    return id;
            return sim::kNoTarget;
        };

        const bool do_build = (in.key_build && !prev_keys_.build) || ui_build_;
        if (do_build && !human_producer_pending_ && human_producer_ == sim::kNoTarget &&
            selected_worker() != sim::kNoTarget) {
            pending_build_ = true;
            build_proto_ = human_build_.producer_proto;
            build_cost_ = human_build_.producer_cost;
            build_time_ = human_build_.producer_build_time;
            pending_attack_ = pending_patrol_ = false;
        }

        const bool do_train = (in.key_train && !prev_keys_.train) || ui_train_;
        if (do_train && human_producer_ != sim::kNoTarget) {
            const ai::BuildItem& au = pl.config.army_unit;
            MeleeCommands::train(world_, human_producer_, au.proto, au.minerals, au.gas,
                                 au.build_time);
        }

        if (ui_trainworker_ && pl.base.depot < world_.units().size() &&
            world_.unit(pl.base.depot).alive &&
            world_.player(cfg_.human_player).minerals >= human_build_.worker_cost)
            world_.queue_train(pl.base.depot, human_build_.worker_proto, human_build_.worker_cost, 0,
                               human_build_.worker_build_time);

        if (ui_buildtechlab_ || ui_buildreactor_) {
            const std::uint32_t prod = selected_addon_producer();
            auto& hp = world_.player(cfg_.human_player);
            if (prod != sim::kNoTarget && hp.minerals >= 50 && hp.gas >= 25) {
                const int32_t kind =
                    ui_buildreactor_ ? sim::World::kAddonReactor : sim::World::kAddonTechLab;
                const sim::Fixed btime = sim::Fixed::from_int(ui_buildreactor_ ? 50 : 25);
                if (world_.build_addon(prod, kind, btime)) {
                    hp.minerals -= 50;
                    hp.gas -= 25;
                }
            }
        }

        if (ui_traintech_ && cat_) {
            const std::uint32_t prod = selected_techlab_producer();
            if (prod != sim::kNoTarget) {
                const data::Catalog::TechTrain tt = cat_->techlab_train_product(world_.unit(prod).catalog_id);
                auto& hp = world_.player(cfg_.human_player);
                if (!tt.unit.empty()) {
                    const data::UnitCost mc = data::unit_cost(*cat_, tt.unit);
                    if (hp.minerals >= mc.minerals && hp.gas >= mc.gas) {
                        sim::Unit mp = make_unit(*cat_, tt.unit, cfg_.human_player);
                        world_.queue_train(prod, mp, mc.minerals, mc.gas,
                                           sim::Fixed::from_double(tt.time > 0 ? tt.time : 21.0));
                    }
                }
            }
        }

        if (ui_trainair_ && cat_) {
            const std::uint32_t sp = selected_starport();
            if (sp != sim::kNoTarget) {
                const data::UnitCost ac = data::unit_cost(*cat_, "VikingFighter");
                auto& hp = world_.player(cfg_.human_player);
                if (hp.minerals >= ac.minerals && hp.gas >= ac.gas) {
                    sim::Unit vp = make_unit(*cat_, "VikingFighter", cfg_.human_player);
                    world_.queue_train(sp, vp, ac.minerals, ac.gas, sim::Fixed::from_int(42));
                }
            }
        }

        if (ui_trainzealot_ && cat_) {
            const std::uint32_t gw = selected_gateway();
            if (gw != sim::kNoTarget) {
                const data::UnitCost zc = data::unit_cost(*cat_, "Zealot");
                auto& hp = world_.player(cfg_.human_player);
                if (hp.minerals >= zc.minerals && hp.gas >= zc.gas) {
                    sim::Unit zp = make_unit(*cat_, "Zealot", cfg_.human_player);
                    world_.queue_train(gw, zp, zc.minerals, zc.gas, sim::Fixed::from_int(33));
                }
            }
        }

        if (ui_buildsupply_ && selected_worker() != sim::kNoTarget) {
            pending_build_ = true;
            build_proto_ = human_build_.supply_proto;
            build_cost_ = human_build_.supply_cost;
            build_time_ = human_build_.supply_build_time;
            pending_attack_ = pending_patrol_ = false;
        }

        if (ui_buildfactory_ && cat_ && selected_worker() != sim::kNoTarget) {
            pending_build_ = true;
            build_proto_ = make_unit(*cat_, "Factory", cfg_.human_player);
            build_cost_ = data::unit_cost(*cat_, "Factory").minerals;
            build_time_ = sim::Fixed::from_int(60);
            pending_attack_ = pending_patrol_ = false;
        }

        if (ui_buildstarport_ && cat_ && selected_worker() != sim::kNoTarget) {
            pending_build_ = true;
            build_proto_ = make_unit(*cat_, "Starport", cfg_.human_player);
            build_cost_ = data::unit_cost(*cat_, "Starport").minerals;
            build_time_ = sim::Fixed::from_int(50);
            pending_attack_ = pending_patrol_ = false;
        }

        if (ui_buildpylon_ && cat_ && selected_worker() != sim::kNoTarget) {
            pending_build_ = true;
            build_proto_ = make_unit(*cat_, "Pylon", cfg_.human_player);
            build_cost_ = data::unit_cost(*cat_, "Pylon").minerals;
            build_time_ = sim::Fixed::from_int(25);
            pending_attack_ = pending_patrol_ = false;
        }

        if (ui_buildgateway_ && cat_ && selected_worker() != sim::kNoTarget) {
            pending_build_ = true;
            build_proto_ = make_unit(*cat_, "Gateway", cfg_.human_player);
            build_cost_ = data::unit_cost(*cat_, "Gateway").minerals;
            build_time_ = sim::Fixed::from_int(65);
            pending_attack_ = pending_patrol_ = false;
        }

        if (pending_town_hall_ && cat_ && selected_worker() != sim::kNoTarget) {
            const std::string th = human_town_hall();
            pending_build_ = true;
            build_proto_ = make_unit(*cat_, th, cfg_.human_player);
            build_cost_ = data::unit_cost(*cat_, th).minerals;
            build_time_ = sim::Fixed::from_int(71);
            pending_attack_ = pending_patrol_ = false;
        }

        if (ui_buildengbay_ && cat_ && selected_worker() != sim::kNoTarget) {
            pending_build_ = true;
            build_proto_ = make_unit(*cat_, "EngineeringBay", cfg_.human_player);
            build_cost_ = data::unit_cost(*cat_, "EngineeringBay").minerals;
            build_time_ = sim::Fixed::from_int(35);
            pending_attack_ = pending_patrol_ = false;
        }

        if (ui_research_ && cat_ && research_ && cfg_.human_player >= 0) {
            const std::uint32_t eb = selected_completed_engbay();
            const bool done = research_->has(cfg_.human_player, kInfantryWeaponsL1);
            const bool inflight = research_->pending(cfg_.human_player, kInfantryWeaponsL1);
            if (eb != sim::kNoTarget && !done && !inflight) {
                const data::Catalog::ResearchInfo info = cat_->research_info(kInfantryWeaponsL1);
                sim::Player& hp = world_.player(cfg_.human_player);
                if (info.found && info.time > 0 && hp.minerals >= info.minerals &&
                    hp.gas >= info.gas) {
                    hp.minerals -= info.minerals;
                    hp.gas -= info.gas;
                    begin_research(cfg_.human_player, kInfantryWeaponsL1,
                                   sim::Fixed::from_double(info.time));
                }
            }
        }

        if (ui_morphorbital_ && cat_) {
            const std::uint32_t cc = selected_command_center();
            auto& hp = world_.player(cfg_.human_player);
            const auto delta = morph_cost_delta(*cat_, "CommandCenter", "OrbitalCommand");
            const bool barracks = human_producer_ != sim::kNoTarget;
            if (cc != sim::kNoTarget && barracks && hp.minerals >= delta.first && hp.gas >= delta.second) {
                if (orbital_proto_ < 0)
                    orbital_proto_ =
                        world_.add_spawn_proto(make_unit(*cat_, "OrbitalCommand", cfg_.human_player));
                world_.begin_morph(cc, orbital_proto_, sim::Fixed::from_int(35));
                hp.minerals -= delta.first;
                hp.gas -= delta.second;
            }
        }

        if (ui_fightermode_ && cat_) {
            const std::uint32_t ag = selected_liberator_ag();
            if (ag != sim::kNoTarget) {
                if (liberator_proto_ < 0)
                    liberator_proto_ =
                        world_.add_spawn_proto(make_unit(*cat_, "Liberator", cfg_.human_player));
                world_.clear_zone(ag);
                const auto ms = cat_->morph_spec("LiberatorMorphtoAA");
                const sim::Fixed t =
                    ms ? sim::Fixed::from_double(ms->morph_time) : sim::Fixed::from_double(1.5417);
                world_.begin_morph(ag, liberator_proto_, t);
            }
        }

        if (ui_mergearchon_ && cat_) {
            const std::vector<std::uint32_t> tem = selected_merge_templar();
            if (tem.size() >= 2) {
                const std::uint32_t hi = tem[tem.size() - 1];
                const std::uint32_t lo = tem[tem.size() - 2];
                const std::uint32_t keep = lo;
                const std::uint32_t absorb = hi;
                const std::string abil = cat_->merge_ability(world_.unit(keep).catalog_id);
                const auto ms = cat_->merge_spec(abil);
                if (ms && !ms->target.empty()) {
                    if (archon_proto_ < 0)
                        archon_proto_ =
                            world_.add_spawn_proto(make_unit(*cat_, ms->target, cfg_.human_player));
                    const sim::Fixed t = sim::Fixed::from_double(ms->time);
                    world_.merge_units(keep, absorb, archon_proto_, t);
                }
            }
        }

        if (ui_burrow_ && cat_)
            for (std::uint32_t id : selected_burrowers(false)) sim::set_burrowed(world_, id, true);
        if (ui_unburrow_ && cat_)
            for (std::uint32_t id : selected_burrowers(true)) sim::set_burrowed(world_, id, false);

        if (ui_liftoff_ && cat_)
            if (const std::uint32_t id = selected_liftoff_structure(false); id != sim::kNoTarget)
                world_.lift_off(id);
        if (ui_lowerdepot_ && cat_)
            if (const std::uint32_t id = selected_depot(false); id != sim::kNoTarget) world_.lower(id);
        if (ui_raisedepot_ && cat_)
            if (const std::uint32_t id = selected_depot(true); id != sim::kNoTarget) world_.raise(id);

        std::vector<std::uint32_t> issued_this_frame;
        for (std::size_t i = 0; i < build_queue_.size();) {
            const QueuedBuild& q = build_queue_[i];
            const bool worker_free =
                q.worker < world_.units().size() && world_.unit(q.worker).alive &&
                !world_.worker_committed(q.worker) &&
                std::find(issued_this_frame.begin(), issued_this_frame.end(), q.worker) ==
                    issued_this_frame.end();
            if (!worker_free || world_.player(cfg_.human_player).minerals < q.cost) {
                ++i;
                continue;
            }
            if (world_.issue_worker_build(q.worker, q.x, q.y, q.proto, q.cost, 0, q.time,
                                          sim::Fixed::from_int(2), human_build_verb_) &&
                q.is_producer)
                human_producer_pending_ = true;
            issued_this_frame.push_back(q.worker);
            build_queue_.erase(build_queue_.begin() + i);
        }
    }
    prev_keys_.train = in.key_train;
    prev_keys_.build = in.key_build;
    ui_build_ = ui_train_ = ui_trainworker_ = ui_buildsupply_ = false;
    ui_buildtechlab_ = ui_buildreactor_ = ui_traintech_ = ui_buildfactory_ = false;
    ui_buildstarport_ = ui_trainair_ = false;
    ui_buildpylon_ = ui_buildgateway_ = ui_trainzealot_ = false;
    pending_town_hall_ = false;
    ui_morphorbital_ = ui_buildengbay_ = ui_research_ = ui_fightermode_ = false;
    ui_mergearchon_ = false;
    ui_burrow_ = ui_unburrow_ = false;
    ui_liftoff_ = ui_lowerdepot_ = ui_raisedepot_ = false;

    if (cfg_.human_player >= 0) {
        groups_.prune(world_);
        for (int d = 0; d < 10; ++d) {
            if (in.key_num[d] && !prev_num_[d]) {
                if (in.key_ctrl) {
                    groups_.set(d, selection_);
                } else {
                    std::vector<uint32_t> g = groups_.recall(d);
                    if (!g.empty()) selection_ = g;
                }
            }
            prev_num_[d] = in.key_num[d];
        }
    }

    const float tick_ms =
        (1000.0f / float(sim::World::kTicksPerSecond)) / float(clock_.multiplier(clock_.speed()));
    accum_ms_ += clock_.paused() ? 0.0f : dt_ms;
    if (accum_ms_ > tick_ms * 8.0f) accum_ms_ = tick_ms * 8.0f;
    if (curr_pos_.empty()) snapshot_positions(curr_pos_);
    bool ticked = false;
    while (accum_ms_ >= tick_ms) {
        prev_pos_ = curr_pos_;
        world_.step();
        if (research_)
            research_->advance(sim::Fixed::from_ratio(1, sim::World::kTicksPerSecond));
        mule_system_.advance(world_, sim::World::dt());
        snapshot_positions(curr_pos_);
        if (prev_pos_.size() < curr_pos_.size()) {
            std::size_t old = prev_pos_.size();
            prev_pos_.resize(curr_pos_.size());
            for (std::size_t k = old; k < curr_pos_.size(); ++k) prev_pos_[k] = curr_pos_[k];
        }
        accum_ms_ -= tick_ms;
        ticked = true;
        if (++tick_counter_ >= cfg_.ai_period_ticks) {
            tick_counter_ = 0;
            for (size_t p = 0; p < ais_.size(); ++p)
                if (cfg_.human_player < 0 || int(p) != cfg_.human_player) {
                    ais_[p]->update(world_);
                    maybe_ai_research(int(p));
                    if (cast_policy_) cast_policy_->run(world_, int(p));
                }
        }
    }

    for (std::size_t id = bound_count_; id < world_.units().size(); ++id) {
        const Unit& u = world_.unit(std::uint32_t(id));
        if (u.player < 0) continue;
        if (u.spawn_parent != sim::kNoTarget) {
            driver_.set_model(std::uint32_t(id), models_ ? models_->model_for(u.catalog_id) : nullptr);
            continue;
        }
        const std::vector<const formats::Model*>* tbl;
        const formats::Model* fallback;
        if (u.speed.raw() == 0) { tbl = &spawn_producer_model_; fallback = producer_model_; }
        else if (u.is_worker)   { tbl = &spawn_worker_model_;   fallback = nullptr; }
        else                    { tbl = &spawn_army_model_;     fallback = driver_.default_model; }
        const formats::Model* m =
            (size_t(u.player) < tbl->size() && (*tbl)[size_t(u.player)]) ? (*tbl)[size_t(u.player)]
                                                                         : fallback;
        driver_.set_model(std::uint32_t(id), m);
    }
    bound_count_ = world_.units().size();

    driver_.observe(world_, dt_ms, ticked);
    if (ticked) alerts_.observe(world_, messages_, dt_s);
    messages_.advance(dt_s);

    if (ticked) {
        auto is_structure = [](const Unit& u) { return u.player >= 0 && u.speed.raw() == 0; };
        score_.observe(world_, is_structure);
        if (death_seen_.size() < world_.units().size())
            death_seen_.resize(world_.units().size(), 0);
        for (std::uint32_t id = 0; id < world_.units().size(); ++id) {
            const Unit& u = world_.unit(id);
            if (!u.alive && !death_seen_[id]) {
                death_seen_[id] = 1;
                if (u.player >= 0) {
                    const bool structure = u.speed.raw() == 0;
                    decals_.spawn(float(u.x.to_double()), float(u.y.to_double()),
                                  structure ? render::DecalKind::Crater : render::DecalKind::Scorch,
                                  structure ? 30.0f : 12.0f);
                }
            }
        }
    }
    decals_.advance(dt_s);
    if (!cfg_.no_victory)
        outcome_ = MatchResult::evaluate(world_, cfg_.human_player >= 0 ? cfg_.human_player : 0);
    emit_scene(scene);
    cam = camctl_.camera();
    return !in.quit;
}

static std::uint32_t type_hash(const std::string& s) {
    std::uint32_t h = 2166136261u;
    for (char c : s) {
        h ^= std::uint8_t(c);
        h *= 16777619u;
    }
    return h;
}

void MeleeGame::rebuild_subgroups() {
    std::vector<std::uint32_t> units;
    std::vector<std::uint32_t> types;
    std::vector<int> prio;
    units.reserve(selection_.size());
    for (std::uint32_t id : selection_) {
        if (id >= world_.units().size() || !world_.unit(id).alive) continue;
        const Unit& u = world_.unit(id);
        const std::string& tok = id < layout_.unit_type.size() ? layout_.unit_type[id] : std::string();
        units.push_back(id);
        types.push_back(type_hash(tok));
        int p = 1;
        if (u.speed.raw() == 0) p = 8;
        else if (u.wpn_damage.raw() > 0) p = 5;
        prio.push_back(p);
    }
    const std::size_t prev_active = sel_groups_.active;
    sel_groups_ = SelectionGroups::build(units, types, prio);
    if (prev_active < sel_groups_.subgroups.size()) sel_groups_.active = prev_active;
}

std::vector<std::uint32_t> MeleeGame::subgroup_ordered_selection() const {
    if (sel_groups_.subgroups.empty()) return selection_;
    const std::size_t a =
        sel_groups_.active < sel_groups_.subgroups.size() ? sel_groups_.active : 0;
    std::vector<std::uint32_t> ordered = sel_groups_.subgroups[a].units;
    for (std::uint32_t id : selection_) {
        bool in_active = false;
        for (std::uint32_t x : sel_groups_.subgroups[a].units)
            if (x == id) { in_active = true; break; }
        if (!in_active) ordered.push_back(id);
    }
    return ordered;
}

void MeleeGame::build_overlay(std::uint32_t view_w, std::uint32_t view_h,
                              std::vector<std::uint8_t>& out) {
    out.clear();
    if (!view_w || !view_h) return;
    const int viewed = cfg_.human_player >= 0 ? cfg_.human_player : 0;

    const float aspect = float(view_w) / float(view_h);

    rebuild_subgroups();

    HudContent content = HudModel::build(world_, viewed, subgroup_ordered_selection());
    if (cfg_.human_player >= 0) {
        content.command_buttons.assign(9, "");
        if (human_producer_ == sim::kNoTarget) content.command_buttons[0] = "Build";
        else content.command_buttons[1] = "Marine";
        const char* banner = MatchResult::banner(outcome_);
        if (banner[0] != '\0') content.selection_summary = banner;
    }
    if (selection_.size() == 1 && selection_[0] < world_.units().size()) {
        const Unit& s = world_.unit(selection_[0]);
        if (s.player < 0) {
            const bool gas = selection_[0] < layout_.unit_type.size() &&
                             layout_.unit_type[selection_[0]] == "VespeneGeyser";
            char b[40];
            std::snprintf(b, sizeof(b), "%s: %d", gas ? "Vespene" : "Minerals", s.resource_left);
            content.selection_summary = b;
        }
    }
    for (const HudMessage& mm : messages_.all()) {
        std::string t = mm.text;
        if (mm.count > 1) t += " x" + std::to_string(mm.count);
        content.messages.push_back({mm.zone, t});
    }

    CameraFootprint cam;
    if (map_w_ > 0.0f && map_h_ > 0.0f) {
        Control ctrl(camctl_.view_proj(aspect), view_w, view_h, viewed);
        const float cxs[4] = {0.0f, float(view_w), float(view_w), 0.0f};
        const float cys[4] = {0.0f, 0.0f, float(view_h), float(view_h)};
        cam.valid = true;
        for (int k = 0; k < 4 && cam.valid; ++k) {
            sim::Fixed wx, wy;
            if (ctrl.ground_point(world_, ScreenPoint{cxs[k], cys[k]}, wx, wy)) {
                cam.x[k] = float(wx.to_double());
                cam.y[k] = float(wy.to_double());
            } else {
                cam.valid = false;
            }
        }
    }

    std::vector<std::uint8_t> hud =
        HudRender::render(content, world_, map_w_, map_h_, view_w, view_h, &cam);
    std::vector<std::uint8_t> units =
        unit_overlay_.render(world_, camctl_.view_proj(aspect), view_w, view_h, {}, viewed);

    out.assign(std::size_t(view_w) * view_h * 4, 0);

    if (!decals_.all().empty()) {
        const render::Mat4 dmvp = camctl_.view_proj(aspect);
        auto dproj = [&](float wx, float wy, int& sx, int& sy) -> bool {
            const auto& m = dmvp.m;
            const float cx = m[0] * wx + m[4] * wy + m[12];
            const float cy = m[1] * wx + m[5] * wy + m[13];
            const float cw = m[3] * wx + m[7] * wy + m[15];
            if (cw <= 1e-6f) return false;
            sx = int((cx / cw * 0.5f + 0.5f) * float(view_w));
            sy = int((cy / cw * 0.5f + 0.5f) * float(view_h));
            return true;
        };
        for (const render::Decal& d : decals_.all()) {
            int sx, sy;
            if (!dproj(d.x, d.y, sx, sy)) continue;
            std::uint8_t cr = 20, cg = 18, cb = 16;
            if (d.kind == render::DecalKind::Crater) { cr = 60; cg = 55; cb = 48; }
            else if (d.kind == render::DecalKind::Blood) { cr = 90; cg = 20; cb = 20; }
            const int rad = d.kind == render::DecalKind::Crater ? 9 : 5;
            const float a = d.alpha() * 0.6f;
            for (int dy = -rad; dy <= rad; ++dy)
                for (int dx = -rad; dx <= rad; ++dx) {
                    if (dx * dx + dy * dy > rad * rad) continue;
                    const int px = sx + dx, py = sy + dy;
                    if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                    const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                    out[di] = std::uint8_t(cr * a + out[di] * (1 - a));
                    out[di + 1] = std::uint8_t(cg * a + out[di + 1] * (1 - a));
                    out[di + 2] = std::uint8_t(cb * a + out[di + 2] * (1 - a));
                    if (out[di + 3] < std::uint8_t(a * 255)) out[di + 3] = std::uint8_t(a * 255);
                }
        }
    }

    alpha_over(out, units);
    alpha_over(out, hud);

    if (dragging_)
        draw_box_rgba(out, view_w, view_h, int(drag_x0_), int(drag_y0_), int(mouse_x_),
                      int(mouse_y_), 80, 235, 110);

    {
        const render::Mat4 mvp = camctl_.view_proj(aspect);
        std::vector<int> assigned(world_.units().size(), 0);
        for (const Unit& u : world_.units())
            if (u.alive && u.player >= 0 && u.gather_target != sim::kNoTarget &&
                u.gather_target < assigned.size())
                assigned[u.gather_target]++;
        auto proj = [&](const Unit& n, int& sx, int& sy) -> bool {
            const auto& m = mvp.m;
            const float x = float(n.x.to_double()), y = float(n.y.to_double()), z = 1.2f;
            const float cx = m[0] * x + m[4] * y + m[8] * z + m[12];
            const float cy = m[1] * x + m[5] * y + m[9] * z + m[13];
            const float cw = m[3] * x + m[7] * y + m[11] * z + m[15];
            if (cw <= 1e-6f) return false;
            sx = int((cx / cw * 0.5f + 0.5f) * float(view_w));
            sy = int((cy / cw * 0.5f + 0.5f) * float(view_h));
            return true;
        };
        for (const MeleePlayerLayout& pl : layout_.players) {
            int total = 0, best_sx = 0, best_sy = 1 << 30, live_patches = 0;
            for (std::uint32_t nid : pl.minerals) {
                if (nid >= world_.units().size() || !world_.unit(nid).alive) continue;
                ++live_patches;
                total += assigned[nid];
                int sx, sy;
                if (proj(world_.unit(nid), sx, sy) && sy < best_sy) { best_sy = sy; best_sx = sx; }
            }
            if (live_patches > 0 && best_sy < (1 << 30)) {
                char b[16];
                std::snprintf(b, sizeof(b), "%d/%d", total, live_patches * 2);
                draw_text_rgba(out, view_w, view_h, best_sx - 10, best_sy - 30, b, 2, 240, 236, 150);
            }
            for (std::uint32_t gid : pl.geysers) {
                if (gid >= world_.units().size() || !world_.unit(gid).alive) continue;
                const Unit& g = world_.unit(gid);
                bool extractor = false;
                for (const Unit& s : world_.units())
                    if (s.alive && s.player >= 0 && s.speed.raw() == 0) {
                        const sim::Fixed dx = s.x - g.x, dy = s.y - g.y;
                        if ((dx * dx + dy * dy).to_double() < 1.0) { extractor = true; break; }
                    }
                if (!extractor) continue;
                int sx, sy;
                if (proj(g, sx, sy)) {
                    char b[12];
                    std::snprintf(b, sizeof(b), "%d/3", assigned[gid]);
                    draw_text_rgba(out, view_w, view_h, sx - 8, sy - 30, b, 2, 150, 240, 170);
                }
            }
        }
    }

    {
        auto putpx = [&](int x, int y, int r, int g, int b, int a) {
            if (x < 0 || y < 0 || x >= int(view_w) || y >= int(view_h)) return;
            std::size_t i = (std::size_t(y) * view_w + x) * 4;
            out[i] = std::uint8_t(r); out[i + 1] = std::uint8_t(g);
            out[i + 2] = std::uint8_t(b); out[i + 3] = std::uint8_t(a);
        };
        const float aspect2 = view_h > 0 ? float(view_w) / float(view_h) : 1.0f;
        const render::Mat4 mvp = camctl_.view_proj(aspect2);
        const auto& m = mvp.m;
        auto proj0 = [&](float x, float y, int& sx, int& sy) -> bool {
            const float cx = m[0] * x + m[4] * y + m[12];
            const float cy = m[1] * x + m[5] * y + m[13];
            const float cw = m[3] * x + m[7] * y + m[15];
            if (cw <= 1e-6f) return false;
            sx = int((cx / cw * 0.5f + 0.5f) * float(view_w));
            sy = int((cy / cw * 0.5f + 0.5f) * float(view_h));
            return true;
        };
        auto line = [&](int x0, int y0, int x1, int y1, int r, int g, int b, int a) {
            int dx = std::abs(x1 - x0), dy = -std::abs(y1 - y0);
            int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, err = dx + dy;
            for (int guard = 0; guard < 4000; ++guard) {
                putpx(x0, y0, r, g, b, a);
                if (x0 == x1 && y0 == y1) break;
                int e2 = 2 * err;
                if (e2 >= dy) { err += dy; x0 += sx; }
                if (e2 <= dx) { err += dx; y0 += sy; }
            }
        };
        auto marker = [&](int cx, int cy, int r, int g, int b) {
            for (int k = -5; k <= 5; ++k) {
                putpx(cx + k, cy, r, g, b, 235); putpx(cx, cy + k, r, g, b, 235);
                putpx(cx + k, cy + (5 - std::abs(k)), r, g, b, 200);
                putpx(cx + k, cy - (5 - std::abs(k)), r, g, b, 200);
            }
        };

        if (pending_build_) {
            const float wx = float(build_px_.to_double()), wy = float(build_py_.to_double());
            const float rad = float(build_proto_.radius.to_double());
            int cx, cy, ex, ey;
            if (proj0(wx, wy, cx, cy) && proj0(wx + (rad > 0 ? rad : 1.0f), wy, ex, ey)) {
                int pr = std::abs(ex - cx);
                pr = pr < 8 ? 8 : (pr > 90 ? 90 : pr);
                const int r = build_valid_ ? 60 : 225, g = build_valid_ ? 215 : 60;
                for (int y = cy - pr; y <= cy + pr; ++y)
                    for (int x = cx - pr; x <= cx + pr; ++x) putpx(x, y, r, g, 60, 70);
                for (int t = 0; t < 2; ++t) {
                    for (int x = cx - pr; x <= cx + pr; ++x) { putpx(x, cy - pr + t, r, g, 60, 255); putpx(x, cy + pr - t, r, g, 60, 255); }
                    for (int y = cy - pr; y <= cy + pr; ++y) { putpx(cx - pr + t, y, r, g, 60, 255); putpx(cx + pr - t, y, r, g, 60, 255); }
                }
            }
        }

        if (cfg_.human_player >= 0)
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                const Unit& u = world_.unit(id);
                if (!u.alive || u.player != cfg_.human_player || u.orders.empty()) continue;
                const sim::Order& o = u.orders.front();
                if (o.type != sim::OrderType::Move && o.type != sim::OrderType::AttackMove) continue;
                int ux, uy, tx, ty;
                if (proj0(float(u.x.to_double()), float(u.y.to_double()), ux, uy) &&
                    proj0(float(o.tx.to_double()), float(o.ty.to_double()), tx, ty)) {
                    const bool atk = o.type == sim::OrderType::AttackMove;
                    const int r = atk ? 230 : 90, g = atk ? 90 : 210, b = atk ? 70 : 90;
                    line(ux, uy, tx, ty, r, g, b, 170);
                    marker(tx, ty, r, g, b);
                }
            }

        if (cfg_.human_player >= 0)
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.player != cfg_.human_player || !u.constructing) continue;
                int cx, cy;
                if (!proj0(float(u.x.to_double()), float(u.y.to_double()), cx, cy)) continue;
                const int bw = 40, bh = 6, x0 = cx - bw / 2, y0 = cy - 26;
                const double prog = u.build_progress.to_double();
                const int fillw = int(double(bw) * (prog < 0 ? 0 : (prog > 1 ? 1 : prog)));
                for (int y = y0; y < y0 + bh; ++y)
                    for (int x = x0; x < x0 + bw; ++x) putpx(x, y, 20, 25, 30, 210);
                for (int y = y0; y < y0 + bh; ++y)
                    for (int x = x0; x < x0 + fillw; ++x) putpx(x, y, 240, 210, 70, 255);
                for (int x = x0 - 1; x <= x0 + bw; ++x) { putpx(x, y0 - 1, 0, 0, 0, 255); putpx(x, y0 + bh, 0, 0, 0, 255); }
                for (int y = y0 - 1; y <= y0 + bh; ++y) { putpx(x0 - 1, y, 0, 0, 0, 255); putpx(x0 + bw, y, 0, 0, 0, 255); }
            }
    }

    if (cfg_.human_player >= 0 && selection_.size() == 1 &&
        selection_[0] < world_.units().size() && world_.unit(selection_[0]).alive &&
        world_.unit(selection_[0]).player == cfg_.human_player) {
        const Unit& su = world_.unit(selection_[0]);
        const formats::Model* wm_model =
            selection_[0] < driver_.actors.size() ? driver_.actors[selection_[0]].model : nullptr;
        const std::string tok =
            selection_[0] < layout_.unit_type.size() ? layout_.unit_type[selection_[0]] : "";
        const int race = race_of(tok);
        float hpf = su.hp_max.raw() > 0 ? float(su.hp.to_double() / su.hp_max.to_double()) : 1.0f;
        hpf = hpf < 0 ? 0 : (hpf > 1 ? 1 : hpf);
        auto hc = [&](float f, int& r, int& g, int& b) {
            if (f > 0.6f) { r = 70; g = 220; b = 90; }
            else if (f > 0.3f) { r = 230; g = 210; b = 70; }
            else { r = 225; g = 75; b = 60; }
        };
        const HudRect mm = hud_minimap_rect(view_w, view_h);
        const int ph = hud_panel_height(view_h);
        const int PW = 96, PH = ph - 20;
        const int px0 = mm.x1 + 100, py0 = int(view_h) - ph + 12;
        for (int y = py0; y < py0 + PH && y < int(view_h); ++y)
            for (int x = px0; x < px0 + PW && x < int(view_w); ++x) {
                if (x < 0 || y < 0) continue;
                std::size_t i = (std::size_t(y) * view_w + x) * 4;
                out[i] = std::uint8_t(10 * 0.8f + out[i] * 0.2f);
                out[i + 1] = std::uint8_t(14 * 0.8f + out[i + 1] * 0.2f);
                out[i + 2] = std::uint8_t(20 * 0.8f + out[i + 2] * 0.2f);
                out[i + 3] = 255;
            }
        draw_box_rgba(out, view_w, view_h, px0, py0, px0 + PW, py0 + PH, 80, 100, 130);
        if (!tok.empty())
            draw_text_rgba(out, view_w, view_h, px0 + 3, py0 + 2, tok.c_str(), 1, 200, 208, 220);

        if (wm_model) {
            const int MW = PW - 8, MH = PH - 16;
            const WireMask& msk = wire_mask(wm_model, MW, MH);
            const int mx0 = px0 + 4, my0 = py0 + 12;
            int r, g, b; hc(hpf, r, g, b);
            const bool shielded = su.shields.raw() > 0;
            for (int y = 0; y < msk.h; ++y)
                for (int x = 0; x < msk.w; ++x) {
                    if (!msk.cov[std::size_t(y) * msk.w + x]) continue;
                    int cr = r, cg = g, cb = b;
                    if (race == 2) {
                        const float f = hpf * (0.65f + 0.35f * (1.0f - float(y) / std::max(1, msk.h)));
                        hc(f, cr, cg, cb);
                    }
                    const int px = mx0 + x, py = my0 + y;
                    if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                    std::size_t di = (std::size_t(py) * view_w + px) * 4;
                    out[di] = std::uint8_t(cr); out[di + 1] = std::uint8_t(cg);
                    out[di + 2] = std::uint8_t(cb); out[di + 3] = 255;
                }
            if (race == 1 && shielded) {
                for (int y = 0; y < msk.h; ++y)
                    for (int x = 0; x < msk.w; ++x) {
                        if (!msk.cov[std::size_t(y) * msk.w + x]) continue;
                        const bool edge =
                            x == 0 || y == 0 || x == msk.w - 1 || y == msk.h - 1 ||
                            !msk.cov[std::size_t(y) * msk.w + (x - 1)] ||
                            !msk.cov[std::size_t(y) * msk.w + (x + 1)] ||
                            !msk.cov[std::size_t(y - 1) * msk.w + x] ||
                            !msk.cov[std::size_t(y + 1) * msk.w + x];
                        if (!edge) continue;
                        const int px = mx0 + x, py = my0 + y;
                        if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                        std::size_t di = (std::size_t(py) * view_w + px) * 4;
                        out[di] = 90; out[di + 1] = 160; out[di + 2] = 255; out[di + 3] = 255;
                    }
            }
        }
    }

    if (cfg_.human_player >= 0 && ui_store_) {
        auto blit_icon = [&](const IconAtlas::Icon& ic, int dx0, int dy0, int dx1, int dy1) {
            if (!ic.ok() || dx1 <= dx0 || dy1 <= dy0) return;
            const int dw = dx1 - dx0, dh = dy1 - dy0;
            for (int y = 0; y < dh; ++y)
                for (int x = 0; x < dw; ++x) {
                    const int sx = x * int(ic.w) / dw, sy = y * int(ic.h) / dh;
                    const std::size_t si = (std::size_t(sy) * ic.w + sx) * 4;
                    const float a = ic.rgba[si + 3] / 255.0f;
                    const int px = dx0 + x, py = dy0 + y;
                    if (px < 0 || py < 0 || px >= int(view_w) || py >= int(view_h)) continue;
                    const std::size_t di = (std::size_t(py) * view_w + px) * 4;
                    for (int c = 0; c < 3; ++c)
                        out[di + c] = std::uint8_t(ic.rgba[si + c] * a + out[di + c] * (1 - a));
                    out[di + 3] = 255;
                }
        };
        const MeleeCommandContext ctx = command_context();
        CommandCard card = build_melee_command_card(ctx);
        merge_selection_abilities(card);

        for (int i = 0; i < CommandCard::kSlots; ++i) {
            const HudRect c = hud_command_cell(view_w, view_h, i);
            if (c.w() <= 0) continue;
            const CommandButton& btn = card.slots[i];
            std::uint8_t br = 20, bg = 24, bb = 32;
            if (btn.state == ButtonState::Disabled) { br = 40; bg = 22; bb = 22; }
            else if (btn.state != ButtonState::Empty) { br = 26, bg = 34, bb = 48; }
            for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                    if (x < 0 || y < 0) continue;
                    const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                    out[di] = std::uint8_t(br * 0.55f + out[di] * 0.45f);
                    out[di + 1] = std::uint8_t(bg * 0.55f + out[di + 1] * 0.45f);
                    out[di + 2] = std::uint8_t(bb * 0.55f + out[di + 2] * 0.45f);
                    out[di + 3] = 255;
                }
            if (btn.state != ButtonState::Empty && !btn.icon.empty()) {
                blit_icon(icons_.get(*ui_store_, btn.icon.c_str()), c.x0 + 1, c.y0 + 1, c.x1 - 1,
                          c.y1 - 1);
                if (btn.state == ButtonState::Disabled)
                    for (int y = c.y0; y < c.y1 && y < int(view_h); ++y)
                        for (int x = c.x0; x < c.x1 && x < int(view_w); ++x) {
                            if (x < 0 || y < 0) continue;
                            const std::size_t di = (std::size_t(y) * view_w + x) * 4;
                            out[di] = std::uint8_t(out[di] * 0.45f);
                            out[di + 1] = std::uint8_t(out[di + 1] * 0.45f);
                            out[di + 2] = std::uint8_t(out[di + 2] * 0.45f);
                        }
                const std::uint8_t lr = btn.is_cancel ? 200 : 130;
                const std::uint8_t lg = btn.is_cancel ? 90 : 160;
                const std::uint8_t lb = btn.is_cancel ? 90 : 200;
                draw_box_rgba(out, view_w, view_h, c.x0, c.y0, c.x1, c.y1, lr, lg, lb);
            } else {
                draw_box_rgba(out, view_w, view_h, c.x0, c.y0, c.x1, c.y1, 60, 70, 88);
            }
        }
    }

    char fps[24];
    std::snprintf(fps, sizeof(fps), "FPS %d", int(fps_smooth_ + 0.5f));
    draw_text_rgba(out, view_w, view_h, 6, 6, fps, 2, 80, 255, 120);
}

}
