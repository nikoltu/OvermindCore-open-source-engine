#include "sc2/app/melee_game.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <string>
#include <unordered_map>

#include "sc2/app/command_bind.hpp"
#include "sc2/app/control.hpp"
#include "sc2/app/hud.hpp"
#include "sc2/app/hud_render.hpp"
#include "sc2/app/melee_commands.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/mat4.hpp"
#include "sc2/render/prim.hpp"
#include "sc2/ui/font.hpp"

namespace sc2::app {

using sim::Unit;

namespace {

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
    ctx.can_afford_producer = bank >= human_producer_cost_;
    ctx.can_afford_army = bank >= human_army_cost_;
    ctx.can_afford_worker = bank >= human_worker_cost_;
    ctx.can_afford_supply = bank >= human_supply_cost_;
    return ctx;
}

void MeleeGame::setup(const MeleeSetup& setup, const MeleeModels* models,
                      const formats::Model* ground, float map_w, float map_h,
                      const sim::NavGrid* nav) {
    layout_ = build_melee(world_, setup);
    ground_ = ground;
    map_w_ = map_w;
    map_h_ = map_h;
    if (nav) world_.nav() = *nav;
    world_.set_separation(true);

    driver_.default_model = models ? models->model_for("Marine") : nullptr;
    for (uint32_t id = 0; id < layout_.unit_type.size(); ++id) {
        const formats::Model* m = models ? models->model_for(layout_.unit_type[id]) : nullptr;
        driver_.set_model(id, m);
    }
    producer_model_ = models ? models->model_for("Barracks") : nullptr;
    carry_model_ = models ? models->model_for("MineralField") : nullptr;
    bound_count_ = layout_.unit_type.size();

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
        human_producer_cost_ = hp.producer_mineral_cost;
        human_army_cost_ = hp.army_mineral_cost;
        human_worker_proto_ = hp.worker;
        human_worker_cost_ = hp.worker_mineral_cost;
        human_worker_build_time_ = hp.worker_build_time;
        human_supply_proto_ = hp.supply_unit;
        human_supply_cost_ = hp.supply_mineral_cost;
        human_supply_build_time_ = hp.supply_build_time;
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
        if (in.key_hold && !prev_hold_)
            for (std::uint32_t id : selection_)
                world_.issue(id, sim::Order{sim::OrderType::HoldPosition});
        if (in.key_stop && !prev_stop_)
            for (std::uint32_t id : selection_) world_.stop(id);
        if (in.key_patrol && !prev_patrol_ && !over_bar) {
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
    prev_hold_ = in.key_hold;
    prev_stop_ = in.key_stop;
    prev_patrol_ = in.key_patrol;

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
            const CommandCard card = build_melee_command_card(command_context());
            for (int i = 0; i < kCommandCols * kCommandRows && i < CommandCard::kSlots; ++i) {
                const HudRect c = hud_command_cell(in.view_w, in.view_h, i);
                if (in.mouse_x >= float(c.x0) && in.mouse_x < float(c.x1) &&
                    in.mouse_y >= float(c.y0) && in.mouse_y < float(c.y1)) {
                    switch (card.slots[i].ability) {
                        case kMeleeAbilTrainWorker: ui_trainworker_ = true; break;
                        case kMeleeAbilBuildProducer: ui_build_ = true; break;
                        case kMeleeAbilBuildSupply: ui_buildsupply_ = true; break;
                        case kMeleeAbilTrainArmy: ui_train_ = true; break;
                        case kMeleeAbilStop:
                            for (std::uint32_t id : selection_) world_.stop(id);
                            pending_attack_ = pending_patrol_ = false;
                            break;
                        case kMeleeAbilHold:
                            for (std::uint32_t id : selection_)
                                world_.issue(id, sim::Order{sim::OrderType::HoldPosition});
                            pending_attack_ = pending_patrol_ = false;
                            break;
                        case kMeleeAbilAttack:
                            pending_attack_ = true;
                            pending_patrol_ = false;
                            break;
                        case kMeleeAbilPatrol:
                            pending_patrol_ = true;
                            pending_attack_ = false;
                            break;
                        default: break;
                    }
                    break;
                }
            }
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

    if (in.right_pressed && !selection_.empty() && !over_bar) {
        std::uint32_t node = sim::kNoTarget;
        sim::Fixed gx, gy;
        if (ctrl.ground_point(world_, ScreenPoint{in.mouse_x, in.mouse_y}, gx, gy)) {
            sim::Fixed best_d2;
            for (const Unit& u : world_.units()) {
                if (!u.alive || u.player >= 0) continue;
                sim::Fixed r = u.radius + sim::Fixed::from_int(2);
                sim::Fixed dx = u.x - gx, dy = u.y - gy, d2 = dx * dx + dy * dy;
                if (d2 > r * r) continue;
                if (node == sim::kNoTarget || d2 < best_d2) { node = u.id; best_d2 = d2; }
            }
        }
        if (node != sim::kNoTarget && cfg_.human_player >= 0 &&
            size_t(cfg_.human_player) < layout_.players.size()) {
            const std::uint32_t depot = layout_.players[size_t(cfg_.human_player)].base.depot;
            for (std::uint32_t id : selection_) {
                if (id >= world_.units().size()) continue;
                if (id < layout_.unit_type.size() && layout_.unit_type[id] == "SCV") {
                    Unit& w = world_.unit(id);
                    w.home_depot = depot;
                    w.gather_target = node;
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
    const float tick_ms = 1000.0f / float(sim::World::kTicksPerSecond);
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

        if (human_producer_ == sim::kNoTarget && human_producer_pending_) {
            for (const Unit& u : world_.units())
                if (u.alive && u.player == cfg_.human_player && u.speed.raw() == 0 &&
                    u.id != pl.base.depot && u.supply_provides.raw() == 0) {
                    human_producer_ = u.id;
                    human_producer_pending_ = false;
                    break;
                }
        }

        const bool do_build = (in.key_build && !prev_build_) || ui_build_;
        if (do_build && !human_producer_pending_ &&
            human_producer_ == sim::kNoTarget && pl.base.depot < world_.units().size()) {
            const ai::BuildItem& pu = pl.config.producer_unit;
            if (world_.player(cfg_.human_player).minerals >= pu.minerals &&
                world_.queue_train(pl.base.depot, pu.proto, pu.minerals, pu.gas, pu.build_time))
                human_producer_pending_ = true;
        }

        const bool do_train = (in.key_train && !prev_train_) || ui_train_;
        if (do_train && human_producer_ != sim::kNoTarget) {
            const ai::BuildItem& au = pl.config.army_unit;
            MeleeCommands::train(world_, human_producer_, au.proto, au.minerals, au.gas,
                                 au.build_time);
        }

        if (ui_trainworker_ && pl.base.depot < world_.units().size() &&
            world_.unit(pl.base.depot).alive &&
            world_.player(cfg_.human_player).minerals >= human_worker_cost_)
            world_.queue_train(pl.base.depot, human_worker_proto_, human_worker_cost_, 0,
                               human_worker_build_time_);

        if (ui_buildsupply_ && pl.base.depot < world_.units().size() &&
            world_.unit(pl.base.depot).alive &&
            world_.player(cfg_.human_player).minerals >= human_supply_cost_)
            world_.queue_train(pl.base.depot, human_supply_proto_, human_supply_cost_, 0,
                               human_supply_build_time_);
    }
    prev_train_ = in.key_train;
    prev_build_ = in.key_build;
    ui_build_ = ui_train_ = ui_trainworker_ = ui_buildsupply_ = false;

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

    const float tick_ms = 1000.0f / float(sim::World::kTicksPerSecond);
    accum_ms_ += dt_ms;
    if (accum_ms_ > tick_ms * 8.0f) accum_ms_ = tick_ms * 8.0f;
    if (curr_pos_.empty()) snapshot_positions(curr_pos_);
    bool ticked = false;
    while (accum_ms_ >= tick_ms) {
        prev_pos_ = curr_pos_;
        world_.step();
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
                if (cfg_.human_player < 0 || int(p) != cfg_.human_player) ais_[p]->update(world_);
        }
    }

    for (std::size_t id = bound_count_; id < world_.units().size(); ++id) {
        const Unit& u = world_.unit(std::uint32_t(id));
        if (u.player >= 0 && u.speed.raw() == 0)
            driver_.set_model(std::uint32_t(id), producer_model_);
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
        const CommandCard card = build_melee_command_card(ctx);

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
