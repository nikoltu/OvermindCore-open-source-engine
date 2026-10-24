#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <thread>
#include <map>
#include <string>
#include <vector>

#include "sc2/api/server.hpp"
#include "sc2/app/boot.hpp"
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
#include "sc2/formats/png.hpp"
#include "sc2/render/frame_prof.hpp"
#include "sc2/render/game_window.hpp"
#include "sc2/render/input_script.hpp"
#include "sc2/render/offscreen.hpp"
#include "sc2/render/overlay_pass.hpp"
#include "sc2/render/scene.hpp"
#include "sc2/render/terrain_tex.hpp"
#include "sc2/render/texture_decode.hpp"
#include "sc2/render/vk_helpers.hpp"

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
    std::vector<std::pair<int, std::string>> png_at;
    std::uint32_t canvas_w = 1280, canvas_h = 720;
    uint32_t exit_after = 0;
    std::vector<std::pair<int, std::pair<float, float>>> cam_at;
    bool headless = false;
    struct ScriptedClick { int frame = 0; float x0 = 0, y0 = 0, x1 = 0, y1 = 0; bool right = false; bool hover = false; };
    std::vector<ScriptedClick> click_at;
    struct ScriptedKey { int frame = 0; std::string spec; };
    struct ScriptedMods { int frame = 0; int frames = 2; std::string mods; };
    std::vector<ScriptedKey> key_at;
    std::vector<ScriptedMods> mods_at;
    std::vector<ScriptedKey> text_at;
    std::vector<std::pair<int, float>> damage_at;
    std::vector<std::pair<int, std::string>> create_at;
    bool no_fog = false;
    bool no_hud = false;
    float cam_hoff = 0.0f;
    bool cam_hoff_set = false;
    float cam_dist = 0.0f;
    std::string want_map;
    int api_port = sc2::api::kDefaultPort;
    std::vector<std::string> script_args;
    if (const char* sp = std::getenv("SC2_INPUT_SCRIPT"); sp && sp[0]) {
        std::ifstream sf(sp);
        std::string line;
        while (std::getline(sf, line)) {
            if (const std::size_t h = line.find('#'); h != std::string::npos) line.resize(h);
            std::size_t q = 0;
            while (q < line.size()) {
                while (q < line.size() && std::isspace(static_cast<unsigned char>(line[q]))) ++q;
                std::size_t e = q;
                while (e < line.size() && !std::isspace(static_cast<unsigned char>(line[e]))) ++e;
                if (e > q) script_args.push_back(line.substr(q, e - q));
                q = e;
            }
        }
        std::printf("crafter-sc: SC2_INPUT_SCRIPT %s: %zu tokens\n", sp, script_args.size());
    }
    std::vector<char*> all_args(argv, argv + argc);
    for (std::string& sa : script_args) all_args.push_back(sa.data());
    {
    const int argc = int(all_args.size());
    char** argv = all_args.data();
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--listen") want_listen = true;
        else if (arg == "--sandbox") want_sandbox = true;
        else if (arg == "--skirmish") want_skirmish = true;
        else if (arg == "--png-at" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const std::size_t colon = spec.find(':');
            if (colon != std::string::npos) png_at.emplace_back(std::atoi(spec.substr(0, colon).c_str()), spec.substr(colon + 1));
        }
        else if (arg == "--exit-after" && i + 1 < argc) exit_after = uint32_t(std::atoi(argv[++i]));
        else if (arg == "--canvas" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const std::size_t x = spec.find_first_of("xX");
            if (x != std::string::npos) {
                const int w = std::atoi(spec.substr(0, x).c_str());
                const int h = std::atoi(spec.substr(x + 1).c_str());
                if (w >= 320 && h >= 240 && w <= 8192 && h <= 8192) {
                    canvas_w = uint32_t(w);
                    canvas_h = uint32_t(h);
                }
            }
        }
        else if (arg == "--map" && i + 1 < argc) want_map = argv[++i];
        else if (arg == "--no-fog") no_fog = true;
        else if (arg == "--no-hud") no_hud = true;
        else if (arg == "--cam-dist" && i + 1 < argc) cam_dist = float(std::atof(argv[++i]));
        else if (arg == "--cam-hoff" && i + 1 < argc) { cam_hoff = float(std::atof(argv[++i])); cam_hoff_set = true; }
        else if (arg == "--cam-at" && i + 1 < argc) {
            const std::string spec = argv[++i];
            float cx = 0, cy = 0;
            int n = 0;
            if (std::sscanf(spec.c_str(), "%d:%f,%f", &n, &cx, &cy) == 3) cam_at.push_back({n, {cx, cy}});
        }
        else if (arg == "--create-at" && i + 1 < argc) {
            const std::string spec = argv[++i];
            const std::size_t c0 = spec.find(':');
            if (c0 != std::string::npos) create_at.push_back({std::atoi(spec.c_str()), spec.substr(c0 + 1)});
        }
        else if (arg == "--damage-at" && i + 1 < argc) {
            int n = 0;
            float fr = 0;
            if (std::sscanf(argv[++i], "%d:%f", &n, &fr) == 2) damage_at.push_back({n, fr});
        }
        else if (arg == "--headless") headless = true;
        else if (arg == "--screen" && i + 1 < argc) app::FrontEnd::request_boot_screen(argv[++i]);
        else if (arg == "--click-at" && i + 1 < argc) {
            const std::string spec = argv[++i];
            ScriptedClick ck;
            if (std::sscanf(spec.c_str(), "%d:%f,%f", &ck.frame, &ck.x0, &ck.y0) == 3) {
                ck.x1 = ck.x0;
                ck.y1 = ck.y0;
                click_at.push_back(ck);
            }
        }
        else if (arg == "--rclick-at" && i + 1 < argc) {
            const std::string spec = argv[++i];
            ScriptedClick ck;
            if (std::sscanf(spec.c_str(), "%d:%f,%f", &ck.frame, &ck.x0, &ck.y0) == 3) {
                ck.x1 = ck.x0;
                ck.y1 = ck.y0;
                ck.right = true;
                click_at.push_back(ck);
            }
        }
        else if (arg == "--drag-at" && i + 1 < argc) {
            const std::string spec = argv[++i];
            ScriptedClick ck;
            if (std::sscanf(spec.c_str(), "%d:%f,%f,%f,%f", &ck.frame, &ck.x0, &ck.y0, &ck.x1, &ck.y1) == 5) click_at.push_back(ck);
        }
        else if (arg == "--hover-at" && i + 1 < argc) {
            const std::string spec = argv[++i];
            ScriptedClick ck;
            if (std::sscanf(spec.c_str(), "%d:%f,%f", &ck.frame, &ck.x0, &ck.y0) == 3) {
                ck.x1 = ck.x0;
                ck.y1 = ck.y0;
                ck.hover = true;
                click_at.push_back(ck);
            }
        }
        else if ((arg == "--key-at" || arg == "--mods-at" || arg == "--text-at") && i + 1 < argc) {
            const std::string spec = argv[++i];
            const std::size_t colon = spec.find(':');
            render::InputState probe;
            bool ok = colon != std::string::npos && colon > 0 && colon + 1 < spec.size();
            if (ok) {
                const int n = std::atoi(spec.substr(0, colon).c_str());
                std::string rest = spec.substr(colon + 1);
                if (arg == "--key-at") {
                    ok = render::apply_key_spec(probe, rest);
                    if (ok) key_at.push_back({n, rest});
                } else if (arg == "--mods-at") {
                    ScriptedMods sm;
                    sm.frame = n;
                    const std::size_t c2 = rest.find(':');
                    if (c2 != std::string::npos) {
                        sm.frames = std::atoi(rest.substr(c2 + 1).c_str());
                        rest.resize(c2);
                    }
                    sm.mods = rest;
                    ok = sm.frames > 0 && render::apply_modifiers(probe, rest);
                    if (ok) mods_at.push_back(sm);
                } else {
                    text_at.push_back({n, rest});
                }
            }
            if (!ok) {
                std::fprintf(stderr, "crafter-sc: %s %s: not understood\n", arg.c_str(), spec.c_str());
                return 2;
            }
        }
        else if (arg == "--api-port" && i + 1 < argc) api_port = std::atoi(argv[++i]);
    }
    }
    std::stable_sort(click_at.begin(), click_at.end(), [](const ScriptedClick& a, const ScriptedClick& b) { return a.frame < b.frame; });
    app::InstallInfo info = resolve_install();
    if (!info.valid) return 0;

    double mem_prev = 0.0;
    auto mem_mark = [&](const std::string& stage) {
        if (!std::getenv("SC2_MEM_TRACE")) return;
        double ws = 0.0, pv = 0.0;
        platform::process_memory_mb(ws, pv);
        std::printf("crafter-sc: mem stage %-28s private %7.1f MB (+%6.1f)  working set %7.1f MB\n", stage.c_str(), pv, pv - mem_prev, ws);
        std::fflush(stdout);
        mem_prev = pv;
    };
    mem_mark("process start");
    if (std::getenv("SC2_MEM_TRACE")) render::mem_mark_fn() = [&](const char* s) { mem_mark(s); };
    casc::Storage store;
    data::Catalog cat;
    app::MeleeModels models;
    try {
        const std::vector<app::StartupStep> plan = app::plan_startup(
            {"AbilData.xml", "RequirementData.xml", "RequirementNodeData.xml", "LightData.xml",
             "TerrainData.xml", "TerrainTexData.xml"});
        platform::LoadingScreen splash("CrafterSC");
        for (std::size_t i = 0; i < plan.size(); ++i) {
            splash.progress(float(i) / float(plan.size()), plan[i].label);
            app::run_startup_step(plan[i], info.data_dir, store, cat, models);
            if (i + 1 == plan.size() || plan[i + 1].kind != plan[i].kind) {
                const char* k = plan[i].kind == app::StartupStep::Kind::OpenStore ? "store indexes"
                                : plan[i].kind == app::StartupStep::Kind::MeleeCatalog ? "melee catalog"
                                : plan[i].kind == app::StartupStep::Kind::CatalogFile ? "extra catalog documents"
                                                                                      : "boot models";
                mem_mark(k);
            }
        }
        if (std::getenv("SC2_MEM_TRACE")) {
            std::size_t ix = 0, en = 0, nm = 0, ci = 0, nmod = 0;
            store.memory_bytes(&ix, &en, &nm, &ci);
            const std::size_t mb = models.memory_bytes(&nmod);
            std::printf("crafter-sc: mem parts after boot: store idx %.1f MB, encoding %.1f MB, names %.1f MB, lower-case index %.1f MB; "
                        "%zu models %.1f MB\n", ix / 1048576.0, en / 1048576.0, nm / 1048576.0, ci / 1048576.0, nmod, mb / 1048576.0);
            std::size_t ce = 0, cr = 0;
            cat.memory_bytes(&ce, &cr);
            std::printf("crafter-sc: mem parts after boot: catalog entries %.1f MB, catalog resolved cache %.1f MB\n", ce / 1048576.0, cr / 1048576.0);
        }
        splash.progress(1.0f, "Preparing menu");
    } catch (const std::exception& e) {
        platform::alert("CrafterSC", std::string("Failed to load game data:\n") + e.what());
        return 1;
    }

    app::TestMap map = app::TestMap::generate(72.0f, 72.0f);
    app::FrontEnd fe(cat, models, store, map);
    mem_mark("front end");
    fe.set_config_io(platform::get_config, platform::set_config);
    fe.set_menu_data_dir(info.data_dir);

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
    fe.set_double_click_ms(platform::double_click_ms());
    fe.set_clipboard_callback(platform::clipboard_text);
    fe.set_open_folder_callback(platform::open_folder);
    fe.set_maps(with_casc(!maps_dir.empty() ? platform::list_maps(maps_dir)
                                            : platform::list_maps(info.install_root)));
    fe.set_maps_folder_callback([&] {
        std::string d = platform::pick_folder("Select a folder that contains .SC2Map files");
        if (d.empty()) return;
        platform::set_config("maps_dir", d);
        fe.set_maps(with_casc(platform::list_maps(d)));
    });
    fe.set_console_renderer([&fe](casc::Storage& st, const data::Catalog& c, const std::string& skin, std::uint32_t w, std::uint32_t h) {
        app::ConsoleView cv;
        cv.frame_w = float(w);
        cv.frame_h = float(h);
        cv.map_env = &fe.map_environment();
        return app::render_console_models(st, c, skin, w, cv);
    });
    const auto render_ss = [](casc::Storage& st, const formats::Model& pm, float fov_rad, int cam_bone, float znear, float zfar, std::uint32_t w,
                              std::uint32_t h, const data::EnvLight* env) {
        static const std::uint32_t ss = std::getenv("SC2_PORTRAIT_SS") ? std::uint32_t(std::clamp(std::atoi(std::getenv("SC2_PORTRAIT_SS")), 1, 4)) : 2u;
        app::portrait_dof().px_scale = float(ss);
        if (ss <= 1) return app::render_portrait(st, pm, fov_rad, cam_bone, znear, zfar, w, h, env);
        const std::vector<std::uint8_t> big = app::render_portrait(st, pm, fov_rad, cam_bone, znear, zfar, w * ss, h * ss, env);
        if (big.size() != std::size_t(w) * ss * h * ss * 4) return big.empty() ? big : app::render_portrait(st, pm, fov_rad, cam_bone, znear, zfar, w, h, env);
        std::vector<std::uint8_t> out(std::size_t(w) * h * 4);
        for (std::uint32_t y = 0; y < h; ++y)
            for (std::uint32_t x = 0; x < w; ++x)
                for (int c = 0; c < 4; ++c) {
                    std::uint32_t acc = 0;
                    for (std::uint32_t sy = 0; sy < ss; ++sy)
                        for (std::uint32_t sx = 0; sx < ss; ++sx) acc += big[(std::size_t(y * ss + sy) * w * ss + (x * ss + sx)) * 4 + std::size_t(c)];
                    out[(std::size_t(y) * w + x) * 4 + std::size_t(c)] = std::uint8_t((acc + ss * ss / 2) / (ss * ss));
                }
        return out;
    };
    fe.set_portrait_renderer([&fe, render_ss](casc::Storage& st, const formats::Model& pm, float fov_rad, int cam_bone, float znear, float zfar, std::uint32_t w,
                                              std::uint32_t h, const data::EnvLight* env) {
        const char* mp = std::getenv("SC2_PORTRAIT_MAP_POST");
        if (!mp && !std::getenv("SC2_PORTRAIT_OWN_POST")) mp = "ce";
        const data::EnvLight& me = fe.map_environment();
        if (!mp || !env || !me.found) return render_ss(st, pm, fov_rad, cam_bone, znear, zfar, w, h, env);
        data::EnvLight pl = *env;
        for (const auto& kv : me.params) {
            const bool col = kv.first == "Colorize" || kv.first.compare(0, 12, "Colorization") == 0;
            if ((col && std::strchr(mp, 'c')) || (kv.first == "OperatorHDR" && std::strchr(mp, 'o'))) pl.params[kv.first] = kv.second;
        }
        if (std::strchr(mp, 'e') && me.has_hdr && me.hdr_exposure > 0.0) {
            pl.hdr_exposure = me.hdr_exposure;
            pl.has_hdr = true;
            pl.params["HDRExposure"] = me.hdr_exposure;
        }
        return render_ss(st, pm, fov_rad, cam_bone, znear, zfar, w, h, &pl);
    });
    {
        std::string rd = platform::get_config("replays_dir");
        if (rd.empty()) {
            const std::string cfgp = platform::config_path();
            const std::size_t cut = cfgp.find_last_of("/\\");
            rd = (cut == std::string::npos ? std::string(".") : cfgp.substr(0, cut)) + "/replays";
        }
        fe.set_replays_dir(rd);
        std::string sd = platform::get_config("saves_dir");
        if (sd.empty()) {
            const std::string cfgp = platform::config_path();
            const std::size_t cut = cfgp.find_last_of("/\\");
            sd = (cut == std::string::npos ? std::string(".") : cfgp.substr(0, cut)) + "/saves";
        }
        fe.set_saves_dir(sd);
    }

    fe.set_texture_callback([&](formats::Model& ground, const formats::LoadedMap& lm) {
        const std::vector<std::string>& palette =
            lm.has_splat ? lm.texture_info.layer_names : lm.tileset_textures;
        const float uv_tiles = lm.uv_tiling.found ? lm.uv_tiling.u : float(lm.width) / 8.0f;
        const float uv_tiles_v = lm.uv_tiling.found ? lm.uv_tiling.v : uv_tiles;
        if (lm.has_splat || lm.has_retail_splat) {
            const bool splat_vertices = std::getenv("SC2_SPLAT_EXTENT_VERTICES") != nullptr;
            const float splat_w = splat_vertices ? float(lm.width) : float(lm.cell_width());
            const float splat_h = splat_vertices ? float(lm.height) : float(lm.cell_height());
            if (render::set_terrain_splat_gpu(ground, store, palette, lm.texture_mask,
                                              splat_w, splat_h, &lm.vertex_tint)) {
                render::apply_grid_uvs(ground, uv_tiles, uv_tiles_v);
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
            render::apply_grid_uvs(ground, uv_tiles, uv_tiles_v);
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
    if (!want_map.empty()) {
        std::ifstream mf(want_map, std::ios::binary);
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());
        app::Race human_race = app::Race::Terran;
        if (const char* rc = std::getenv("SC2_MAP_RACE")) {
            const std::string r = rc;
            if (r == "Zerg") human_race = app::Race::Zerg;
            else if (r == "Protoss") human_race = app::Race::Protoss;
        }
        app::Race other_race = app::Race::Terran;
        if (const char* rc = std::getenv("SC2_MAP_RACE2")) {
            const std::string r = rc;
            if (r == "Zerg") other_race = app::Race::Zerg;
            else if (r == "Protoss") other_race = app::Race::Protoss;
        }
        fe.set_observe_next(std::getenv("SC2_MAP_AI_VS_AI") != nullptr);
        fe.set_portrait_full_rate(!png_at.empty());
        const bool ok = !bytes.empty() && fe.begin_match(std::move(bytes), human_race, other_race);
        mem_mark("match begun (map, setup)");
        if (const char* wt = std::getenv("SC2_WARM_TYPES"); wt && fe.active_match()) {
            const auto t0 = std::chrono::steady_clock::now();
            std::vector<const formats::Model*> ms;
            std::string all = wt;
            for (std::size_t p = 0; p < all.size();) {
                std::size_t e = all.find(',', p);
                if (e == std::string::npos) e = all.size();
                if (const formats::Model* m = models.ensure(store, all.substr(p, e - p))) ms.push_back(m);
                p = e + 1;
            }
            const auto t1 = std::chrono::steady_clock::now();
            for (bool ready = false; !ready;) {
                ready = true;
                for (const formats::Model* m : ms) ready = render::request_model_textures(*m, &store, "", 0) && ready;
                if (!ready) std::this_thread::sleep_for(std::chrono::milliseconds(2));
            }
            const auto t2 = std::chrono::steady_clock::now();
            const render::TextureDecodePool* pool = render::TextureDecodePool::existing();
            std::printf("crafter-sc: SC2_WARM_TYPES %zu models: read %.0f ms, textures decoded %.0f ms, %.1f MB held decoded\n", ms.size(),
                        std::chrono::duration<double, std::milli>(t1 - t0).count(), std::chrono::duration<double, std::milli>(t2 - t1).count(),
                        pool ? double(pool->bytes()) / 1048576.0 : 0.0);
        }
        if (const char* dc = std::getenv("SC2_DEBUG_CREATE"); dc && fe.active_match()) {
            const std::string all = dc;
            std::size_t p = 0;
            int made = 0, asked = 0;
            while (p < all.size()) {
                std::size_t e = all.find(';', p);
                if (e == std::string::npos) e = all.size();
                const std::string spec = all.substr(p, e - p);
                p = e + 1;
                const std::size_t c1 = spec.find(':'), c2 = c1 == std::string::npos ? c1 : spec.find(':', c1 + 1);
                if (c2 == std::string::npos) continue;
                ++asked;
                const std::size_t c3 = spec.find(':', c2 + 1);
                if (fe.active_match()->debug_create(spec.substr(0, c1), std::strtof(spec.c_str() + c1 + 1, nullptr),
                                                    std::strtof(spec.c_str() + c2 + 1, nullptr),
                                                    c3 == std::string::npos ? -1000.0f : std::strtof(spec.c_str() + c3 + 1, nullptr)))
                    ++made;
            }
            std::printf("crafter-sc: SC2_DEBUG_CREATE stood %d of %d\n", made, asked);
        }
        if (const char* dm = std::getenv("SC2_DEBUG_MORPH"); dm && fe.active_match() && !std::getenv("SC2_DEBUG_MORPH_AT")) {
            const std::string spec = dm;
            const std::size_t c1 = spec.find(':');
            const bool went = c1 != std::string::npos && fe.active_match()->debug_morph(spec.substr(0, c1), spec.substr(c1 + 1));
            std::printf("crafter-sc: SC2_DEBUG_MORPH %s: %d\n", spec.c_str(), int(went));
        }
        std::printf("crafter-sc: --map %s: begin_match %d, match %s, error '%s'\n", want_map.c_str(), int(ok),
                    fe.active_match() ? "up" : "none", fe.last_match_error().c_str());
        std::fflush(stdout);
    }

    auto frame = [&](const render::InputState& in, float dt_ms,
                     std::vector<render::SceneInstance>& scene, render::Camera& cam,
                     std::vector<std::uint8_t>& overlay) -> bool {
        host.set_frame_view(in.view_w, in.view_h);
        if (want_listen) server.pump(host);
        static int frame_failures = 0;
        bool cont = true;
        try {
            static int cam_frame = 0;
            ++cam_frame;
            if (app::MeleeGame* g = fe.active_match(); g && !cam_at.empty()) {
                const std::pair<float, float>* want = nullptr;
                for (const auto& ca : cam_at)
                    if (ca.first <= cam_frame) want = &ca.second;
                if (want) g->camera_control().focus = {want->first, want->second, g->ground_z_at(want->first, want->second)};
                if (want && cam_dist > 0.0f) g->camera_control().distance = cam_dist;
                if (want && cam_hoff_set) g->camera_control().height_offset = cam_hoff;
            }
            if (cam_frame == 2 || cam_frame % 60 == 0) {
                double ws = 0.0, pv = 0.0;
                platform::process_memory_mb(ws, pv);
                const double gl = double(render::gpu_alloc_bytes()[0].load()) / 1048576.0, gh = double(render::gpu_alloc_bytes()[1].load()) / 1048576.0;
                const render::TextureDecodePool* dp = render::TextureDecodePool::existing();
                const double pool_mb = dp ? double(dp->bytes()) / 1048576.0 : 0.0;
                render::frame_prof_count("mem.ram_mb", ws);
                render::frame_prof_count("mem.private_mb", pv);
                render::frame_prof_count("mem.gpu_local_mb", gl);
                render::frame_prof_count("mem.gpu_host_mb", gh);
                render::frame_prof_count("mem.decode_pool_mb", pool_mb);
                if (std::getenv("SC2_MEM_TRACE") && (cam_frame == 2 || cam_frame % 3600 == 0)) {
                    app::MeleeGame* mg = fe.active_match();
                    std::printf("crafter-sc: mem frame %d tick %u units %zu: ram %.0f MB (private %.0f), gpu local %.0f MB host %.0f MB, decode pool %.0f MB\n",
                                cam_frame, mg ? unsigned(mg->world().tick()) : 0u, mg ? mg->world().units().size() : std::size_t(0), ws, pv, gl, gh, pool_mb);
                    std::size_t ix = 0, en = 0, nm = 0, ci = 0, nmod = 0;
                    store.memory_bytes(&ix, &en, &nm, &ci);
                    const std::size_t mb = models.memory_bytes(&nmod);
                    std::printf("crafter-sc: mem parts frame %d: store idx %.1f MB, encoding %.1f MB, names %.1f MB, lower-case index %.1f MB; "
                                "%zu models %.1f MB\n", cam_frame, ix / 1048576.0, en / 1048576.0, nm / 1048576.0, ci / 1048576.0, nmod, mb / 1048576.0);
                    std::size_t ce = 0, cr = 0;
                    cat.memory_bytes(&ce, &cr);
                    std::printf("crafter-sc: mem parts frame %d: catalog entries %.1f MB, catalog resolved cache %.1f MB\n", cam_frame, ce / 1048576.0, cr / 1048576.0);
                    std::fflush(stdout);
                }
            }
            if (app::MeleeGame* g = fe.active_match(); g && !create_at.empty())
                for (const auto& ca : create_at)
                    if (ca.first == cam_frame) {
                        const std::string& s = ca.second;
                        const std::size_t c1 = s.find(':'), c2 = c1 == std::string::npos ? c1 : s.find(':', c1 + 1);
                        if (c2 != std::string::npos)
                            g->debug_create(s.substr(0, c1), std::strtof(s.c_str() + c1 + 1, nullptr), std::strtof(s.c_str() + c2 + 1, nullptr));
                    }
            if (app::MeleeGame* g = fe.active_match(); g && !damage_at.empty())
                for (const auto& da : damage_at)
                    if (da.first == cam_frame)
                        for (const sim::Unit& cu : g->world().units())
                            if (cu.alive && cu.player >= 0 && cu.hp_max.raw() > 0) {
                                sim::Unit& mu = g->world().unit(cu.id);
                                mu.hp = sim::Fixed::from_double(mu.hp_max.to_double() * double(da.second));
                            }
            render::InputState scripted = in;
            for (const auto& ck : click_at) {
                if (cam_frame < ck.frame) continue;
                const bool drag = ck.x1 != ck.x0 || ck.y1 != ck.y0;
                const int k = cam_frame - ck.frame;
                scripted.mouse_x = (drag && k >= 1) ? ck.x1 : ck.x0;
                scripted.mouse_y = (drag && k >= 1) ? ck.y1 : ck.y0;
                scripted.mouse_dx = scripted.mouse_dy = 0.0f;
                if (k == 0) {
                    char mk[96];
                    std::snprintf(mk, sizeof(mk), "%s@%.0f;%.0f", ck.hover ? "hover" : ck.right ? "rclick" : drag ? "drag" : "click", double(ck.x0), double(ck.y0));
                    render::frame_prof_mark(mk);
                }
                if (ck.hover) continue;
                const int up = drag ? 3 : 1;
                scripted.left_pressed = !ck.right && k == 0;
                scripted.left_down = !ck.right && k >= 0 && k < up;
                scripted.left_released = !ck.right && k == up;
                scripted.right_pressed = ck.right && k == 0;
                scripted.right_down = ck.right && k >= 0 && k < up;
                scripted.right_released = ck.right && k == up;
            }
            for (const auto& ma : mods_at)
                if (cam_frame >= ma.frame && cam_frame < ma.frame + ma.frames) render::apply_modifiers(scripted, ma.mods);
            for (const auto& ka : key_at)
                if (cam_frame == ka.frame) {
                    render::apply_key_spec(scripted, ka.spec);
                    render::frame_prof_mark("key:" + ka.spec);
                    std::printf("crafter-sc: frame %d: key %s\n", cam_frame, ka.spec.c_str());
                    std::fflush(stdout);
                }
            for (const auto& ta : text_at)
                if (cam_frame == ta.frame) scripted.text += ta.spec;
            if (const char* us = std::getenv("SC2_UNIT_STATUS"); us && fe.active_match()) {
                using M = render::UnitOverlay::StatusBarOptions::Mode;
                const std::string s = us;
                fe.active_match()->set_status_bar_mode(s == "Always" ? M::Always : s == "Damaged" ? M::Damaged : s == "Selected" ? M::Selected : s == "Targeting" ? M::Targeting : M::Normal);
                fe.active_match()->set_keep_status_layer(true);
            }
            { SC2_PROF_SCOPE("fe.step"); cont = fe.step(scripted, dt_ms, scene, cam); }
            fe.build_overlay(in.view_w, in.view_h, overlay);
            render::set_os_cursor_hidden(fe.cursor_drawn());
            frame_failures = 0;
            static int frame_index = 0;
            ++frame_index;
            if (const char* at = std::getenv("SC2_DEBUG_MORPH_AT"); at && fe.active_match()) {
                static bool morphed = false;
                if (!morphed && fe.active_match()->world().tick() >= std::atoi(at))
                    if (const char* dm = std::getenv("SC2_DEBUG_MORPH")) {
                        morphed = true;
                        const std::string spec = dm;
                        const std::size_t c1 = spec.find(':');
                        const bool went = c1 != std::string::npos && fe.active_match()->debug_morph(spec.substr(0, c1), spec.substr(c1 + 1));
                        std::printf("crafter-sc: SC2_DEBUG_MORPH %s at tick %d: %d\n", spec.c_str(), fe.active_match()->world().tick(), int(went));
                    }
            }
            for (const auto& pa : png_at)
                if (pa.first == frame_index) {
                    std::size_t drawable = 0;
                    for (const render::SceneInstance& si : scene) drawable += si.model != nullptr;
                    std::printf("crafter-sc: frame %d: %zu instances, %zu with a model, map %s, sim tick %d\n", pa.first, scene.size(), drawable,
                                fe.active_map() ? "loaded" : "none", fe.active_match() ? fe.active_match()->world().tick() : -1);
                    if (fe.active_match() && std::getenv("SC2_DEBUG_CREATE"))
                        for (const auto& bu : fe.active_match()->world().units())
                            if (bu.alive && bu.constructing)
                                std::printf("crafter-sc: constructing %s progress %.3f of %.1f s\n", bu.catalog_id.c_str(), bu.build_progress.to_double(), bu.build_time.to_double());
                    if (fe.active_match() && std::getenv("SC2_UNIT_STATUS"))
                        for (const auto& su : fe.active_match()->world().units())
                            if (su.alive && su.player >= 0 && (su.hp < su.hp_max || (su.energy_max.raw() > 0 && su.energy < su.energy_max) || (su.shields_max.raw() > 0 && su.shields < su.shields_max)))
                                std::printf("crafter-sc: short vital %s at (%.1f, %.1f) life %.1f / %.1f energy %.1f / %.1f shields %.1f / %.1f\n", su.catalog_id.c_str(), su.x.to_double(), su.y.to_double(),
                                            su.hp.to_double(), su.hp_max.to_double(), su.energy.to_double(), su.energy_max.to_double(), su.shields.to_double(), su.shields_max.to_double());
                    if (fe.active_match() && std::getenv("SC2_GATHER_TRACE")) {
                        int mining = 0, carrying = 0, other = 0;
                        std::map<std::uint32_t, int> per_node;
                        for (const auto& gu : fe.active_match()->world().units()) {
                            if (!gu.alive || gu.orders.empty() || gu.orders.front().type != sim::OrderType::Gather) continue;
                            if (gu.mine_cd.raw() > 0) ++per_node[gu.gather_target];
                            if (gu.mine_cd.raw() > 0 && gu.gather_target < fe.active_match()->world().units().size()) {
                                const auto& nn = fe.active_match()->world().units()[gu.gather_target];
                                std::printf("crafter-sc:   miner %s at (%.2f, %.2f) node (%.2f, %.2f) d %.2f\n", gu.catalog_id.c_str(), gu.x.to_double(), gu.y.to_double(),
                                            nn.x.to_double(), nn.y.to_double(), std::hypot((nn.x - gu.x).to_double(), (nn.y - gu.y).to_double()));
                            }
                            if (gu.mine_cd.raw() <= 0 && gu.carrying > 0)
                                std::printf("crafter-sc:   carrier %s at (%.2f, %.2f)\n", gu.catalog_id.c_str(), gu.x.to_double(), gu.y.to_double());
                            if (gu.mine_cd.raw() > 0) ++mining;
                            else if (gu.carrying > 0) ++carrying;
                            else ++other;
                        }
                        std::printf("crafter-sc: gather legs mining %d carrying %d to-node %d\n", mining, carrying, other);
                        for (const auto& kv : per_node) {
                            const auto& nu = fe.active_match()->world().units();
                            std::printf("crafter-sc:   node %u %s miners_max %d mining %d\n", unsigned(kv.first),
                                        kv.first < nu.size() ? nu[kv.first].catalog_id.c_str() : "?", kv.first < nu.size() ? int(nu[kv.first].miners_max) : -1, kv.second);
                        }
                    }
                    {
                        std::map<std::string, int> clips;
                        for (const render::SceneInstance& si : scene) {
                            if (!si.model || si.model->sequences.empty()) continue;
                            const std::string nm = si.seq < 0 ? std::string("(none)")
                                                   : std::size_t(si.seq) < si.model->sequences.size() ? si.model->sequences[std::size_t(si.seq)].name
                                                                                                        : std::string("?");
                            ++clips[nm];
                        }
                        for (const auto& kv : clips) std::printf("crafter-sc:   clip %-24s x%d\n", kv.first.c_str(), kv.second);
                        if (const char* pt = std::getenv("SC2_POSE_TRACE"))
                            for (const render::SceneInstance& si : scene) {
                                if (!si.model || si.seq < 0 || std::size_t(si.seq) >= si.model->sequences.size()) continue;
                                const formats::M3Sequence& sq = si.model->sequences[std::size_t(si.seq)];
                                if (sq.name.find(pt) == std::string::npos) continue;
                                float mn[3] = {0, 0, 0}, mx[3] = {0, 0, 0};
                                render::posed_bounds(*si.model, si.seq, si.time_ms, mn, mx);
                                const float* w = si.world.m.data();
                                std::printf("crafter-sc:   pose '%s' seq %d t %.0f of %u ms, scale x %.3f z %.3f, at (%.1f, %.1f, %.2f), posed [%.2f,%.2f,%.2f]..[%.2f,%.2f,%.2f], %zu bones, blend %d w %.2f, turret %d yaw %.3f, beam %d, fx %u, team %.2f %.2f %.2f a %.2f, emissive role a %.2f\n",
                                            sq.name.c_str(), si.seq, double(si.time_ms), sq.length_ms(), double(std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2])),
                                            double(std::sqrt(w[8] * w[8] + w[9] * w[9] + w[10] * w[10])), double(w[12]), double(w[13]),
                                            double(w[14]), double(mn[0]), double(mn[1]), double(mn[2]), double(mx[0]), double(mx[1]), double(mx[2]), si.model->bones.size(), si.blend_seq,
                                            double(si.blend_weight), si.turret_bone, double(si.turret_yaw), si.beam_bone, unsigned(si.fx.kind), double(si.team[0]), double(si.team[1]),
                                            double(si.team[2]), double(si.team[3]), double(si.team_emissive[3]));
                                const std::vector<render::Mat4> tw = render::pose_world(*si.model, std::uint32_t(si.seq), si.time_ms);
                                for (const formats::M3Ribbon& rb : si.model->ribbons)
                                    if (rb.end_bone >= 0 && rb.bone < tw.size() && std::size_t(rb.end_bone) < tw.size()) {
                                        const float* e = tw[std::size_t(rb.end_bone)].m.data();
                                        const float* r0 = tw[rb.bone].m.data();
                                        std::printf("crafter-sc:     ribbon world (%.2f, %.2f, %.2f) -> (%.2f, %.2f, %.2f)\n",
                                                    double(w[0] * r0[12] + w[4] * r0[13] + w[8] * r0[14] + w[12]), double(w[1] * r0[12] + w[5] * r0[13] + w[9] * r0[14] + w[13]),
                                                    double(w[2] * r0[12] + w[6] * r0[13] + w[10] * r0[14] + w[14]), double(w[0] * e[12] + w[4] * e[13] + w[8] * e[14] + w[12]),
                                                    double(w[1] * e[12] + w[5] * e[13] + w[9] * e[14] + w[13]), double(w[2] * e[12] + w[6] * e[13] + w[10] * e[14] + w[14]));
                                        std::printf("crafter-sc:     ribbon bone %u (%.2f, %.2f, %.2f) -> %d (%.2f, %.2f, %.2f), yaw of the instance %.3f rad\n", rb.bone, double(tw[rb.bone].m[12]),
                                                    double(tw[rb.bone].m[13]), double(tw[rb.bone].m[14]), rb.end_bone, double(tw[std::size_t(rb.end_bone)].m[12]),
                                                    double(tw[std::size_t(rb.end_bone)].m[13]), double(tw[std::size_t(rb.end_bone)].m[14]), double(std::atan2(w[1], w[0])));
                                    }
                            }
                        std::fflush(stdout);
                    }
                    if (drawable == 0) {
                        if (overlay.size() != std::size_t(in.view_w) * in.view_h * 4) continue;
                        std::vector<std::uint8_t> black(overlay.size(), 0);
                        for (std::size_t q = 3; q < black.size(); q += 4) black[q] = 255;
                        const std::vector<std::uint8_t> shell = render::composite_overlay(black, overlay, in.view_w, in.view_h);
                        const std::vector<std::uint8_t> spng = formats::write_png(shell.data(), in.view_w, in.view_h);
                        if (FILE* f = std::fopen(pa.second.c_str(), "wb")) {
                            std::fwrite(spng.data(), 1, spng.size(), f);
                            std::fclose(f);
                            std::printf("crafter-sc: frame %d -> %s (%u x %u, overlay only)\n", pa.first, pa.second.c_str(), in.view_w, in.view_h);
                            std::fflush(stdout);
                        }
                        continue;
                    }
                    const data::EnvLight dump_env = fe.current_environment();
                    const float dump_clip = app::melee_shadow_clip(cat);
                    render::RenderResult rr =
                        render::render_scene(scene, in.view_w, in.view_h, &cam, &store, "",  true,
                                             &dump_env, fe.ground_fog(), 0, std::getenv("SC2_DUMP_SCENE_FIT") ? nullptr : &dump_clip);
                    if (!no_hud && overlay.size() == rr.rgba.size()) rr.rgba = render::composite_overlay(rr.rgba, overlay, rr.width, rr.height);
                    if (no_hud && std::getenv("SC2_UNIT_STATUS") && fe.active_match() && fe.active_match()->status_layer().size() == rr.rgba.size())
                        rr.rgba = render::composite_overlay(rr.rgba, fe.active_match()->status_layer(), rr.width, rr.height);
                    const std::vector<std::uint8_t> png = formats::write_png(rr.rgba.data(), rr.width, rr.height);
                    if (FILE* f = std::fopen(pa.second.c_str(), "wb")) {
                        std::fwrite(png.data(), 1, png.size(), f);
                        std::fclose(f);
                        std::printf("crafter-sc: frame %d -> %s (%u x %u, %zu instances)\n", pa.first, pa.second.c_str(), rr.width, rr.height, scene.size());
                        std::fflush(stdout);
                    }
                }
        } catch (const std::exception& e) {
            std::fprintf(stderr, "crafter-sc: frame error: %s\n", e.what());
            cont = ++frame_failures < 120;
        }
        return cont;
    };

    fe.set_reveal_map(no_fog);
    fe.set_ground_fog(!no_fog);
    fe.set_software_cursor(png_at.empty());
    fe.set_portrait_full_rate(!png_at.empty());
    if (headless && exit_after > 0) {
        render::InputState hin{};
        hin.view_w = canvas_w;
        hin.view_h = canvas_h;
        hin.mouse_x = float(canvas_w) * 0.5f;
        hin.mouse_y = float(canvas_h) * 0.25f;
        std::vector<render::SceneInstance> hscene;
        render::Camera hcam;
        std::vector<std::uint8_t> hoverlay;
        static const bool kUnpaced = std::getenv("SC2_HEADLESS_UNPACED") != nullptr;
        for (std::uint32_t f = 0; f < exit_after; ++f) {
            hscene.clear();
            bool go = false;
            { SC2_PROF_SCOPE("frame_cb"); go = frame(hin, 1000.0f / 60.0f, hscene, hcam, hoverlay); }
            render::frame_prof_count("units", fe.active_match() ? double(fe.active_match()->world().units().size()) : 0.0);
            render::frame_prof_count("instances", double(hscene.size()));
            render::frame_prof_tick();
            if (!go) break;
            if (want_listen && !kUnpaced) std::this_thread::sleep_for(std::chrono::milliseconds(4));
        }
        render::frame_prof_flush();
        return 0;
    }
    try {
        render::WindowResult r = render::run_game_window(
            frame, &store, canvas_w, canvas_h, "", exit_after, [&] { return fe.current_environment(); },
            app::melee_shadow_clip(cat),
            [&]() -> const render::FogField* { return fe.ground_fog(); });
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
