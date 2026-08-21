#include <algorithm>
#include <cstdio>
#include <string>
#include <vector>

#include "sc2/app/casc_maps.hpp"
#include "sc2/app/front_end.hpp"
#include "sc2/app/install.hpp"
#include "sc2/app/melee_models.hpp"
#include "sc2/app/platform.hpp"
#include "sc2/app/test_map.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/formats/map_archive.hpp"
#include "sc2/render/game_window.hpp"
#include "sc2/render/terrain_tex.hpp"

using namespace sc2;
namespace platform = sc2::app::platform;

namespace {

app::InstallInfo resolve_install() {
    std::string root = platform::get_config("install_root");
    app::InstallInfo info = root.empty() ? app::InstallInfo{} : app::probe_install(root);
    while (!info.valid) {
        std::string picked = platform::pick_folder("Select your SC2 game folder");
        if (picked.empty()) return {};
        info = app::probe_install(picked);
        if (info.valid) {
            platform::set_config("install_root", picked);
        } else {
            platform::alert("CrafterSC",
                            "That folder is not a valid SC2 installation:\n" + info.message +
                                "\n\nPick the folder that contains SC2Data.");
        }
    }
    return info;
}

}

int main() {
    app::InstallInfo info = resolve_install();
    if (!info.valid) return 0;

    casc::Storage store;
    data::Catalog cat;
    app::MeleeModels models;
    try {
        platform::LoadingScreen splash("CrafterSC");
        splash.progress(0.05f, "Opening game data...");
        store.open(info.data_dir);

        splash.progress(0.35f, "Loading unit catalog...");
        cat.load_melee(store);
        cat.load_files(store, {"AbilData.xml", "RequirementData.xml", "RequirementNodeData.xml",
                               "LightData.xml", "TerrainData.xml"});

        splash.progress(0.70f, "Loading models...");
        models.load(store);

        splash.progress(0.95f, "Preparing menu...");
    } catch (const std::exception& e) {
        platform::alert("CrafterSC", std::string("Failed to load game data:\n") + e.what());
        return 1;
    }

    app::TestMap map = app::TestMap::generate(72.0f, 72.0f);
    app::FrontEnd fe(cat, models, store, map);

    std::vector<platform::MapEntry> casc_entries;
    for (const app::CascMap& cm : app::discover_casc_maps(store)) {
        try {
            std::vector<std::uint8_t> mb = store.read_by_name(cm.path);
            std::string title = app::read_casc_map_name(mb);
            formats::LoadedMap lm = formats::load_map_terrain(std::move(mb));
            platform::MapEntry e;
            e.path = cm.path;
            e.casc = true;
            const std::string sz = std::to_string(lm.width) + "x" + std::to_string(lm.height);
            e.name = !title.empty() ? title + " (" + sz + ")"
                     : (lm.tileset.empty() ? cm.name.substr(0, 8) : lm.tileset) + " " + sz;
            casc_entries.push_back(std::move(e));
        } catch (const std::exception&) {
        }
    }

    auto with_casc = [&](std::vector<platform::MapEntry> folder) {
        folder.insert(folder.end(), casc_entries.begin(), casc_entries.end());
        return folder;
    };
    std::string maps_dir = platform::get_config("maps_dir");
    fe.set_maps(with_casc(!maps_dir.empty() ? platform::list_maps(maps_dir)
                                            : platform::list_maps(info.install_root)));
    fe.set_maps_folder_callback([&] {
        std::string d = platform::pick_folder("Select a folder that contains .SC2Map files");
        if (d.empty()) return;
        platform::set_config("maps_dir", d);
        fe.set_maps(with_casc(platform::list_maps(d)));
    });
    fe.set_texture_callback([&](formats::Model& ground, const formats::LoadedMap& lm) {
        if (lm.has_splat) {
            std::vector<render::ResolvedLayer> layers =
                render::resolve_map_layers(store, lm.texture_info.layer_names);
            render::GroundImage g = render::composite_map_ground(
                store, lm.texture_info, lm.texture_mask, layers, std::max(1u, lm.width / 8u));
            if (!g.empty()) {
                render::apply_grid_uvs(ground, 1.0f);
                render::set_terrain_diffuse_rgba(ground, g.w, g.h, std::move(g.rgba));
                return;
            }
        }
        if (lm.has_retail_splat) {
            render::GroundImage g = render::composite_retail_ground(
                store, lm.tileset_textures, lm.texture_mask, std::max(1u, lm.width / 4u));
            if (!g.empty()) {
                render::apply_grid_uvs(ground, 1.0f);
                render::set_terrain_diffuse_rgba(ground, g.w, g.h, std::move(g.rgba));
                return;
            }
        }
        std::string diff = render::resolve_map_tileset_diffuse(store, lm.tileset, lm.tileset_textures);
        if (diff.empty()) diff = render::resolve_tileset_diffuse(store);
        if (!diff.empty()) {
            render::apply_grid_uvs(ground, float(lm.width) / 8.0f);
            render::set_terrain_diffuse(ground, diff);
        }
    });

    auto frame = [&](const render::InputState& in, float dt_ms,
                     std::vector<render::SceneInstance>& scene, render::Camera& cam,
                     std::vector<std::uint8_t>& overlay) -> bool {
        bool cont = fe.step(in, dt_ms, scene, cam);
        fe.build_overlay(in.view_w, in.view_h, overlay);
        return cont;
    };

    try {
        render::WindowResult r = render::run_game_window(
            frame, &store, 1280, 720, "", 0, [&] { return fe.current_environment(); });
        if (!r.created) {
            platform::alert("CrafterSC", "Could not open a display window (Vulkan/WSI unavailable).");
            return 1;
        }
    } catch (const std::exception& e) {
        platform::alert("CrafterSC", std::string("Renderer error:\n") + e.what());
        return 1;
    }
    return 0;
}
