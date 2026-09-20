#include "sc2/app/shell_emblem.hpp"

#include <vector>

#include "sc2/formats/dds.hpp"

#include "emblem.dds.h"
#include "emblem_sm.dds.h"

namespace sc2::app {

namespace {
IconAtlas::Icon decode(const unsigned char* bytes, std::size_t len) {
    IconAtlas::Icon icon;
    try {
        const std::vector<std::uint8_t> data(bytes, bytes + len);
        formats::DdsInfo info;
        std::vector<std::uint8_t> rgba = formats::dds_decode_rgba(data, info);
        if (info.width && info.height && rgba.size() == std::size_t(info.width) * info.height * 4) {
            icon.w = info.width;
            icon.h = info.height;
            icon.rgba = std::move(rgba);
        }
    } catch (const std::exception&) {
    }
    return icon;
}
}

const IconAtlas::Icon& shell_emblem(bool small) {
    static const IconAtlas::Icon large = decode(kEmblemDds, kEmblemDds_len);
    static const IconAtlas::Icon sm = decode(kEmblemSmDds, kEmblemSmDds_len);
    return small ? sm : large;
}

}
