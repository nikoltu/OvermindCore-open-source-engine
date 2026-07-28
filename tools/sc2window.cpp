#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/anim.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/render/window.hpp"
#include "sc2/sim/world.hpp"

using namespace sc2;
using sim::Fixed;

static bool load_first(casc::Storage& store, const std::vector<std::string>& paths,
                       formats::Model& out) {
    for (const std::string& p : paths) {
        try {
            auto bytes = store.read_by_name(p);
            out = formats::M3::parse(bytes).decode_model();
            if (out.vertex_count && !out.indices.empty()) return true;
        } catch (const std::exception&) {   }
    }
    return false;
}

static formats::Model ground_quad(float x0, float y0, float x1, float y1) {
    formats::Model m;
    m.vertex_count = 4;
    m.positions = {x0, y0, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0};
    m.uvs = {0, 0, 1, 0, 1, 1, 0, 1};
    m.indices = {0, 1, 2, 0, 2, 3};
    formats::M3Region r;
    r.first_vertex = 0; r.num_vertices = 4; r.first_index = 0; r.num_indices = 6;
    m.regions.push_back(r);
    m.region_material.push_back(-1);
    return m;
}

static void set_stats(sim::Unit& u, float hp, float dmg, float period, float range,
                      float speed, float radius) {
    u.hp_max = u.hp = Fixed::from_ratio(int(hp * 64), 64);
    u.wpn_damage = Fixed::from_ratio(int(dmg * 64), 64);
    u.wpn_period = Fixed::from_ratio(int(period * 64), 64);
    u.wpn_range = Fixed::from_ratio(int(range * 64), 64);
    u.speed = Fixed::from_ratio(int(speed * 64), 64);
    u.radius = Fixed::from_ratio(int(radius * 64), 64);
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sc2window <data_dir> [WxH] [--ticks N] [--frames N]\n");
        return 2;
    }
    const char* data = argv[1];
    uint32_t W = 1280, H = 720;
    int ticks = 40;
    uint32_t frames = 120;
    for (int i = 2; i < argc; ++i) {
        int pw = 0, ph = 0;
        if (std::sscanf(argv[i], "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0) { W = pw; H = ph; }
        else if (std::strcmp(argv[i], "--ticks") == 0 && i + 1 < argc) ticks = std::atoi(argv[++i]);
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = uint32_t(std::atoi(argv[++i]));
    }

    try {
        casc::Storage store;
        store.open(data);

        formats::Model marine, zergling;
        bool have_m = load_first(store, {
            "mods/liberty.sc2mod/base.sc2assets/Assets/Units/Terran/Marine/Marine.m3"}, marine);
        bool have_z = load_first(store, {
            "mods/liberty.sc2mod/base.sc2assets/Assets/Units/Zerg/Zergling/Zergling.m3",
            "mods/swarm.sc2mod/base.sc2assets/Assets/Units/Zerg/Zergling/Zergling.m3"}, zergling);
        if (!have_m) { std::fprintf(stderr, "error: Marine.m3 not in this install\n"); return 1; }
        if (!have_z) zergling = marine;

        float mn = 1e30f, mx = -1e30f;
        for (uint32_t i = 0; i < marine.vertex_count; ++i)
            for (int a = 0; a < 2; ++a) {
                float c = marine.positions[i * 3 + a];
                mn = c < mn ? c : mn; mx = c > mx ? c : mx;
            }
        float diam = (mx - mn);
        float ws = diam * 0.6f;
        if (ws < 0.1f) ws = 1.0f;

        sim::World world(1);
        std::vector<uint32_t> marines, zerglings;
        for (int i = 0; i < 5; ++i) {
            sim::Unit u; u.player = 0;
            u.x = Fixed::from_int(3 + i * 2); u.y = Fixed::from_int(3);
            set_stats(u, 45, 6, 0.86f, 5.0f, 2.25f, 0.5f);
            marines.push_back(world.add_unit(u));
        }
        for (int i = 0; i < 6; ++i) {
            sim::Unit u; u.player = 1;
            u.x = Fixed::from_int(2 + i * 2); u.y = Fixed::from_int(15);
            set_stats(u, 35, 5, 0.70f, 0.3f, 3.5f, 0.4f);
            zerglings.push_back(world.add_unit(u));
        }
        for (uint32_t id : marines)
            world.issue(id, {sim::OrderType::AttackMove, Fixed::from_int(8), Fixed::from_int(15)});
        for (uint32_t id : zerglings)
            world.issue(id, {sim::OrderType::AttackMove, Fixed::from_int(8), Fixed::from_int(3)});
        world.run(ticks);

        int seq_m = render::select_sequence(marine, {"Walk"}, 1);
        int seq_z = render::select_sequence(zergling, {"Walk"}, 2);
        float time_ms = float(ticks) * (1000.0f / sim::World::kTicksPerSecond);

        std::vector<render::SceneInstance> scene;
        float bx0 = 1e30f, by0 = 1e30f, bx1 = -1e30f, by1 = -1e30f;
        int alive = 0;
        for (const sim::Unit& u : world.units()) {
            if (!u.alive) continue;
            ++alive;
            float wx = float(u.x.to_double()) * ws, wy = float(u.y.to_double()) * ws;
            bx0 = wx < bx0 ? wx : bx0; bx1 = wx > bx1 ? wx : bx1;
            by0 = wy < by0 ? wy : by0; by1 = wy > by1 ? wy : by1;
            render::SceneInstance inst;
            inst.model = u.player == 0 ? &marine : &zergling;
            inst.world = render::translation(wx, wy, 0);
            inst.seq = u.player == 0 ? seq_m : seq_z;
            inst.time_ms = time_ms;
            inst.team = u.player == 0 ? std::array<float, 4>{0.85f, 0.15f, 0.12f, 1.0f}
                                      : std::array<float, 4>{0.15f, 0.35f, 0.9f, 1.0f};
            scene.push_back(inst);
        }
        if (alive == 0) { std::fprintf(stderr, "error: no survivors to render\n"); return 1; }

        float pad = diam * 2.0f;
        formats::Model gm = ground_quad(bx0 - pad, by0 - pad, bx1 + pad, by1 + pad);
        render::SceneInstance ground;
        ground.model = &gm;
        ground.team = {0, 0, 0, -1.0f};
        scene.insert(scene.begin(), ground);

        try {
            render::WindowResult r = render::render_window(scene, nullptr, &store, W, H, frames, "");
            std::printf("window: %d survivors, presented %u/%u frames at %ux%u; "
                        "validation %s (%u errors); sim state_hash %016llx\n",
                        alive, r.frames_presented, frames, r.width, r.height,
                        r.validation_active ? "on" : "off", r.validation_errors,
                        (unsigned long long)world.state_hash());
            return r.validation_errors == 0 ? 0 : 1;
        } catch (const render::WindowUnavailable& e) {
            std::printf("window unavailable (%s) — no display / WSI here\n", e.what());
            return 0;
        }
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
