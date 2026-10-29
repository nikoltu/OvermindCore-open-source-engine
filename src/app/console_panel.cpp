#include "omc/app/console_panel.hpp"

#include "omc/data/catalog.hpp"
#include "omc/formats/dds.hpp"
#include "omc/formats/m3.hpp"

#include <algorithm>
#include <vector>

namespace omc::app {

static const char* const kRoots[] = {
    "mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
    "mods/swarm.sc2mod/base.sc2assets/", "mods/void.sc2mod/base.sc2assets/",
    "mods/voidmulti.sc2mod/base.sc2assets/", "", nullptr};

const char* const* console_asset_roots() { return kRoots; }

ConsoleTexture load_console(casc::Storage& store, int race, const data::Catalog* cat) {
    ConsoleTexture tex;

    static const char* kTerran[] = {
        "sc2_ui_collection_huds_large_consoleskinterrandefault",
        "sc2_ui_collection_huds_large_consoleterran_classic", nullptr};
    static const char* kProtoss[] = {
        "sc2_ui_collection_huds_large_consoleskinprotossdefault", nullptr};
    static const char* kZerg[] = {
        "sc2_ui_collection_huds_large_consoleskinzergdefault", nullptr};
    const char* const* names = race == 1 ? kProtoss : race == 2 ? kZerg : kTerran;
    if (cat) {
        const char* skin = console_skin_id_for_race(race);
        if (auto authored = cat->console_skin_featured_image(skin)) {
            for (const char* const* rp = kRoots; *rp; ++rp) {
            const char* r = *rp;
                try {
                    std::vector<std::uint8_t> bytes = store.read_by_name(std::string(r) + *authored);
                    formats::DdsInfo info;
                    std::vector<std::uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
                    if (info.width && info.height &&
                        rgba.size() == std::size_t(info.width) * info.height * 4) {
                        tex.w = info.width;
                        tex.h = info.height;
                        tex.rgba = std::move(rgba);
                        return tex;
                    }
                } catch (...) {   }
            }
        }
    }
    for (const char* const* n = names; *n; ++n) {
        for (const char* const* rp = kRoots; *rp; ++rp) {
            const char* r = *rp;
            const std::string path = std::string(r) + "Assets/Textures/" + *n + ".dds";
            try {
                std::vector<std::uint8_t> bytes = store.read_by_name(path);
                formats::DdsInfo info;
                std::vector<std::uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
                if (info.width && info.height &&
                    rgba.size() == std::size_t(info.width) * info.height * 4) {
                    tex.w = info.width;
                    tex.h = info.height;
                    tex.rgba = std::move(rgba);
                    return tex;
                }
            } catch (...) {   }
        }
    }
    return tex;
}

ConsoleTexture load_terran_console(casc::Storage& store) { return load_console(store, 0); }

void composite_console(std::vector<std::uint8_t>& img, std::uint32_t view_w,
                       std::uint32_t view_h, const std::uint8_t* rgba, int tw, int th) {
    if (!rgba || tw <= 0 || th <= 0 || view_w == 0 || view_h == 0) return;
    if (img.size() < std::size_t(view_w) * view_h * 4) return;
    const int dst_w = int(view_w);
    int dst_h = int(std::int64_t(dst_w) * th / tw);
    if (dst_h < 1) dst_h = 1;
    const int dy0 = int(view_h) - dst_h;
    for (int y = 0; y < dst_h; ++y) {
        const int py = dy0 + y;
        if (py < 0 || py >= int(view_h)) continue;
        const int sy = int(std::int64_t(y) * th / dst_h);
        for (int x = 0; x < dst_w; ++x) {
            const int px = x;
            if (px >= int(view_w)) break;
            const int sx = int(std::int64_t(x) * tw / dst_w);
            const std::size_t si = (std::size_t(sy) * tw + sx) * 4;
            const float a = rgba[si + 3] / 255.0f;
            if (a <= 0.0f) continue;
            const std::size_t di = (std::size_t(py) * view_w + px) * 4;
            const float da = img[di + 3] / 255.0f;
            const float oa = a + da * (1.0f - a);
            auto comp = [&](std::uint8_t sc, std::uint8_t dc) {
                if (oa <= 0.0f) return std::uint8_t(0);
                float v = (float(sc) * a + float(dc) * da * (1.0f - a)) / oa;
                if (v < 0.0f) v = 0.0f;
                else if (v > 255.0f) v = 255.0f;
                return std::uint8_t(v + 0.5f);
            };
            img[di] = comp(rgba[si], img[di]);
            img[di + 1] = comp(rgba[si + 1], img[di + 1]);
            img[di + 2] = comp(rgba[si + 2], img[di + 2]);
            img[di + 3] = std::uint8_t(oa * 255.0f + 0.5f);
        }
    }
}

const char* console_skin_id_for_race(int race) {
    return race == 1 ? "ConsoleProtoss_Default" : race == 2 ? "ConsoleZerg_Default"
                                                            : "ConsoleTerran_Default";
}

bool& console_race_assets() {
    static bool on = true;
    return on;
}

const char* console_skin_race(const std::string& skin_id) {
    for (int r = 0; r < 3; ++r)
        if (skin_id == console_skin_id_for_race(r)) return r == 1 ? "Prot" : r == 2 ? "Zerg" : "Terr";
    return "";
}

std::string console_asset_race(const std::string& skin_id) {
    return console_race_assets() ? std::string(console_skin_race(skin_id)) : std::string();
}

}
