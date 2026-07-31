#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "sc2/app/game.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/formats/m3.hpp"
#include "sc2/render/game_window.hpp"

using namespace sc2;

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
    r.first_vertex = 0;
    r.num_vertices = 4;
    r.first_index = 0;
    r.num_indices = 6;
    m.regions.push_back(r);
    m.region_material.push_back(-1);
    return m;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::printf("usage: sc2game <data_dir> [WxH] [--frames N] [--no-ai]\n");
        return 2;
    }
    const char* data = argv[1];
    uint32_t W = 1280, H = 720;
    uint32_t frames = 0;
    bool ai = true;
    for (int i = 2; i < argc; ++i) {
        int pw = 0, ph = 0;
        if (std::sscanf(argv[i], "%dx%d", &pw, &ph) == 2 && pw > 0 && ph > 0) { W = pw; H = ph; }
        else if (std::strcmp(argv[i], "--frames") == 0 && i + 1 < argc) frames = uint32_t(std::atoi(argv[++i]));
        else if (std::strcmp(argv[i], "--no-ai") == 0) ai = false;
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

        const float pad = 8.0f;
        formats::Model ground = ground_quad(2 - pad, 3 - pad, 12 + pad, 15 + pad);

        app::SkirmishConfig cfg;
        cfg.marine = &marine;
        cfg.zergling = &zergling;
        cfg.ground = &ground;
        cfg.enable_ai = ai;
        app::Skirmish sk(cfg);
        sk.setup_demo();

        auto frame = [&](const render::InputState& in, float dt_ms,
                         std::vector<render::SceneInstance>& scene, render::Camera& cam,
                         std::vector<uint8_t>&  ) -> bool {
            return sk.step(in, dt_ms, scene, cam);
        };

        render::WindowResult r = render::run_game_window(frame, &store, W, H, "", frames);
        if (!r.created) {
            std::fprintf(stderr, "error: window/WSI unavailable\n");
            return 1;
        }
        std::printf("sc2game: presented %u frames at %ux%u; final sim state_hash %016llx; "
                    "%u validation errors\n",
                    r.frames_presented, r.width, r.height,
                    (unsigned long long)sk.world().state_hash(), r.validation_errors);
        return 0;
    } catch (const render::WindowUnavailable& e) {
        std::fprintf(stderr, "window unavailable: %s\n", e.what());
        return 1;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}
