#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "sc2/casc/storage.hpp"
#include "sc2/formats/dds.hpp"

namespace sc2::app {

class IconAtlas {
public:
    struct Icon {
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint8_t> rgba;
        bool ok() const { return w && h && rgba.size() == std::size_t(w) * h * 4; }
    };

    const Icon& get(casc::Storage& store, const std::string& short_name) {
        auto it = cache_.find(short_name);
        if (it != cache_.end()) return it->second;
        Icon icon;
        static const char* kRoots[] = {
            "mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
            "mods/swarm.sc2mod/base.sc2assets/", "mods/void.sc2mod/base.sc2assets/",
            "mods/voidmulti.sc2mod/base.sc2assets/", ""};
        for (const char* r : kRoots) {
            const std::string path =
                std::string(r) + "Assets/Textures/" + short_name + ".dds";
            try {
                std::vector<std::uint8_t> bytes = store.read_by_name(path);
                formats::DdsInfo info;
                std::vector<std::uint8_t> rgba = formats::dds_decode_rgba(bytes, info);
                if (info.width && info.height &&
                    rgba.size() == std::size_t(info.width) * info.height * 4) {
                    icon.w = info.width; icon.h = info.height; icon.rgba = std::move(rgba);
                }
                break;
            } catch (...) {   }
        }
        return cache_.emplace(short_name, std::move(icon)).first->second;
    }

private:
    std::unordered_map<std::string, Icon> cache_;
};

}
