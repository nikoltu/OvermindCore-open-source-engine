#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

#include "sc2/api/server.hpp"
#include "sc2/app/casc_maps.hpp"
#include "sc2/app/front_end.hpp"
#include "sc2/app/install.hpp"
#include "sc2/app/replay_orders.hpp"
#include "sc2/app/melee_models.hpp"
#include "sc2/app/platform.hpp"
#include "sc2/app/test_map.hpp"
#include "sc2/core/version.hpp"
#include "sc2/casc/storage.hpp"
#include "sc2/data/catalog.hpp"
#include "sc2/data/melee_mods.hpp"
#include "sc2/formats/map_archive.hpp"
#include "sc2/formats/mpq.hpp"
#include "sc2/formats/replay_events.hpp"
#include "sc2/render/game_window.hpp"
#include "sc2/render/terrain_tex.hpp"

using namespace sc2;
namespace platform = sc2::app::platform;

namespace {

const char kLicenceNotice[] =
    "crafter-sc third-party notices\n"
    "\n"
    "  API message schema (s2client-proto)\n"
    "      MIT. Vendored verbatim under third_party/s2client-proto/; the upstream LICENSE text\n"
    "      is kept there byte-for-byte, and third_party/s2client-proto/VERSION names the exact\n"
    "      upstream commit these bindings were generated from. This executable contains code\n"
    "      generated from that schema.\n"
    "\n"
    "  Protocol Buffers runtime (libprotobuf)\n"
    "      BSD-3-Clause. Linked statically.\n"
    "\n"
    "  Abseil (libabsl_*)\n"
    "      Apache-2.0. Linked statically, as a dependency of the Protocol Buffers runtime.\n"
    "\n"
    "  utf8_range\n"
    "      MIT. Linked statically, as a dependency of the Protocol Buffers runtime.\n"
    "\n"
    "Everything else in this binary was written for this project.\n";

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

class ClientHost final : public sc2::api::Host {
public:
    ClientHost(app::FrontEnd& fe, casc::Storage& store, data::Catalog& cat,
               const app::InstallInfo& info)
        : fe_(fe), store_(store), cat_(cat), info_(info) {}

    void set_frame_view(std::uint32_t w, std::uint32_t h) {
        view_w_ = w;
        view_h_ = h;
    }

    bool create_match(std::vector<std::uint8_t> map_bytes, app::Race p0, app::Race p1,
                      std::string& error) override {
        fe_.end_match();
        try {
            map_name_ = app::read_casc_map_name(map_bytes);
            fe_.begin_match(std::move(map_bytes), p0, p1);
        } catch (const std::exception& e) {
            error = std::string("the map did not load: ") + e.what();
            return false;
        }
        if (!fe_.active_match()) {
            error = "the map loaded but no match was created";
            return false;
        }
        apply_realtime();
        return true;
    }

    void leave_match() override { fe_.end_match(); }
    app::MeleeGame* match() override { return fe_.active_match(); }
    const formats::LoadedMap* map() override { return fe_.active_map(); }
    std::string map_name() const override { return map_name_; }
    const data::Catalog* catalog() const override { return &cat_; }

    bool read_map_file(const std::string& path, std::vector<std::uint8_t>& bytes,
                       std::string& error) override {
        std::ifstream in(path, std::ios::binary);
        if (!in) {
            error = "cannot open " + path;
            return false;
        }
        bytes.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        if (bytes.empty()) {
            error = path + " is empty";
            return false;
        }
        return true;
    }

    int step(int ticks) override {
        app::MeleeGame* game = fe_.active_match();
        if (!game) return 0;
        const bool was_paused = game->paused();
        game->set_paused(false);
        const float tick_ms = (1000.0f / float(sim::World::kTicksPerSecond)) /
                              float(app::GameClock::multiplier(game->speed()));
        render::InputState in;
        in.view_w = view_w_;
        in.view_h = view_h_;
        std::vector<render::SceneInstance> scene;
        render::Camera cam;
        std::vector<std::uint8_t> overlay;
        for (int i = 0; i < ticks; ++i) {
            const int before = game->world().tick();
            for (int guard = 0; guard < kStepGuard && game->world().tick() == before; ++guard) {
                if (!fe_.step(in, tick_ms, scene, cam)) break;
                fe_.build_overlay(view_w_, view_h_, overlay);
                if (!fe_.active_match()) break;
            }
            game = fe_.active_match();
            if (!game) return before;
        }
        game->set_paused(was_paused);
        return game->world().tick();
    }

    void set_realtime(bool on) override {
        realtime_ = on;
        apply_realtime();
    }

    std::string game_version() const override { return sc2::kVersion; }
    std::uint32_t data_build() const override {
        return std::uint32_t(std::atoi(info_.build.c_str()));
    }
    std::vector<std::string> mod_names() const override {
        std::vector<std::string> out;
        for (const char* mod : data::kMeleeMods) out.push_back(std::string("mods/") + mod + ".sc2mod");
        return out;
    }
    std::string truth_data_dir() const override {
        const char* env = std::getenv("TRUTH_DATA_DIR");
        return env ? std::string(env) : std::string();
    }

private:
    static constexpr int kStepGuard = 4;

    void apply_realtime() {
        if (app::MeleeGame* game = fe_.active_match()) game->set_paused(!realtime_);
    }

    app::FrontEnd& fe_;
    casc::Storage& store_;
    data::Catalog& cat_;
    const app::InstallInfo& info_;
    std::string map_name_;
    bool realtime_ = false;
    std::uint32_t view_w_ = 0, view_h_ = 0;
};

void print_replay_event_table() {
    std::printf("replay event table, build %u\n", formats::kReplayProtocolBuild);
    for (const app::EventRow& row : app::replay_event_table()) {
        std::printf("%-12s %-50s %s\n", app::replay_event_bucket_name(row.bucket), row.name, row.why);
    }
    std::printf("\nAn event name with no row here is UNLISTED, and an unlisted event refuses the "
                "replay that carries it.\n");
}

int print_replay_events(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::printf("crafter-sc: cannot open %s\n", path.c_str());
        return 1;
    }
    const std::vector<uint8_t> image((std::istreambuf_iterator<char>(f)),
                                     std::istreambuf_iterator<char>());
    try {
        const formats::ReplayVersion version = formats::decode_replay_version(image);
        std::printf("%s\nrecorded on build %u, %u game loops\n", path.c_str(), version.base_build,
                    version.elapsed_game_loops);
        formats::require_protocol_build(version);
        formats::Mpq archive = formats::Mpq::open(std::vector<uint8_t>(image));
        const std::vector<formats::ReplayEvent> events =
            formats::decode_game_events(archive.read_file("replay.game.events"));

        std::map<std::string, size_t> census;
        for (const formats::ReplayEvent& e : events) census[e.name] += 1;
        std::printf("%zu events decoded, %zu distinct names\n", events.size(), census.size());
        for (const auto& kv : census) {
            std::printf("%-12s %-50s %zu\n",
                        app::replay_event_bucket_name(app::replay_event_bucket(kv.first)),
                        kv.first.c_str(), kv.second);
        }

        size_t inert = 0;
        const std::vector<app::ReplayCommand> commands = app::translate_commands(events, &inert);
        std::printf("translated %zu commands, %zu inert events\n", commands.size(), inert);
        app::OrderLog log;
        const app::OrdersOutcome outcome = app::events_to_orders(commands, log);
        if (!outcome.ok) {
            std::printf("no order log: this translation is blocked on\n");
            for (const std::string& b : outcome.blockers) std::printf("  %s\n", b.c_str());
        }
        return 0;
    } catch (const std::exception& e) {
        std::printf("refused: %s\n", e.what());
        return 2;
    }
}

}

int main(int argc, char** argv) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--version" || arg == "-v") {
            platform::attach_parent_console();
            std::printf("crafter-sc %s\n", sc2::kVersion);
            return 0;
        }
        if (arg == "--help" || arg == "-h") {
            platform::attach_parent_console();
            std::printf("crafter-sc %s\n\n"
                        "  (no arguments)  launch the client\n"
                        "  --listen        also serve the client API on 127.0.0.1 (loopback only)\n"
                        "  --api-port N    listen on port N instead of the default %d\n"
                        "  --sandbox       boot straight into the sandbox test map\n"
                        "  --skirmish      boot straight into a skirmish\n"
                        "  --licences      print the third-party licence notices and exit\n"
                        "  --replay-event-table  print the closed replay event table and exit\n"
                        "  --replay-events F     decode replay F, print its event census with each\n"
                        "                        event's bucket, and exit\n"
                        "  --version, -v   print the version and exit\n"
                        "  --help, -h      print this and exit\n",
                        sc2::kVersion, sc2::api::kDefaultPort);
            return 0;
        }
        if (arg == "--licences" || arg == "--licenses") {
            platform::attach_parent_console();
            std::printf("%s", kLicenceNotice);
            return 0;
        }
        if (arg == "--replay-event-table") {
            platform::attach_parent_console();
            print_replay_event_table();
            return 0;
        }
        if (arg == "--replay-events" && i + 1 < argc) {
            platform::attach_parent_console();
            return print_replay_events(argv[i + 1]);
        }
    }

    bool want_listen = false, want_sandbox = false, want_skirmish = false;
    int api_port = sc2::api::kDefaultPort;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--listen") want_listen = true;
        else if (arg == "--sandbox") want_sandbox = true;
        else if (arg == "--skirmish") want_skirmish = true;
        else if (arg == "--api-port" && i + 1 < argc) api_port = std::atoi(argv[++i]);
    }
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
        const std::vector<std::string>& palette =
            lm.has_splat ? lm.texture_info.layer_names : lm.tileset_textures;
        const float uv_tiles = lm.uv_tiling.found ? lm.uv_tiling.u : float(lm.width) / 8.0f;
        if (lm.has_splat || lm.has_retail_splat) {
            if (render::set_terrain_splat_gpu(ground, store, palette, lm.texture_mask,
                                              float(lm.width), float(lm.height))) {
                render::apply_grid_uvs(ground, uv_tiles);
                return;
            }
            std::vector<render::ResolvedLayer> layers = render::resolve_map_layers(store, palette);
            render::GroundImage g = render::composite_map_ground(
                store, lm.has_splat ? lm.texture_info : formats::TextureInfo{}, lm.texture_mask,
                layers, std::max(1u, uint32_t(uv_tiles)));
            if (!g.empty()) {
                render::apply_grid_uvs(ground, 1.0f);
                render::set_terrain_diffuse_rgba(ground, g.w, g.h, std::move(g.rgba));
                return;
            }
        }
        std::string diff = render::resolve_map_tileset_diffuse(store, lm.tileset, lm.tileset_textures);
        if (diff.empty()) diff = render::resolve_tileset_diffuse(store);
        if (!diff.empty()) {
            render::apply_grid_uvs(ground, uv_tiles);
            render::set_terrain_diffuse(ground, diff);
        }
    });

    ClientHost host(fe, store, cat, info);
    sc2::api::Server server;
    if (want_listen) {
        std::string err;
        if (!server.start(api_port, err)) {
            platform::attach_parent_console();
            std::fprintf(stderr, "crafter-sc: --listen failed on port %d: %s\n", api_port,
                         err.c_str());
            platform::alert("CrafterSC",
                            "Could not open the API port " + std::to_string(api_port) + ":\n" +
                                err);
            return 1;
        }
        platform::attach_parent_console();
        std::printf("crafter-sc: API listening on ws://127.0.0.1:%d/sc2api\n", api_port);
        std::fflush(stdout);
        fe.enter_lobby();
    }
    if (want_sandbox) fe.enter_sandbox();
    if (want_skirmish) fe.enter_skirmish();

    auto frame = [&](const render::InputState& in, float dt_ms,
                     std::vector<render::SceneInstance>& scene, render::Camera& cam,
                     std::vector<std::uint8_t>& overlay) -> bool {
        host.set_frame_view(in.view_w, in.view_h);
        if (want_listen) server.pump(host);
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
