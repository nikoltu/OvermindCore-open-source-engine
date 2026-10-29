#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

#include "omc/casc/storage.hpp"
#include "omc/formats/dds.hpp"

namespace omc::app {

class IconAtlas {
public:
    struct Icon {
        std::uint32_t w = 0, h = 0;
        std::vector<std::uint8_t> rgba;
        bool ok() const { return w && h && rgba.size() == std::size_t(w) * h * 4; }
    };

    const Icon& get(casc::Storage& store, const std::string& name) {
        auto it = cache_.find(name);
        if (it != cache_.end()) return it->second;
        Icon icon;
        static const char* kRoots[] = {
            "mods/liberty.sc2mod/base.sc2assets/", "mods/core.sc2mod/base.sc2assets/",
            "mods/swarm.sc2mod/base.sc2assets/", "mods/void.sc2mod/base.sc2assets/",
            "mods/voidmulti.sc2mod/base.sc2assets/", ""};
        std::string rel = name;
        for (char& c : rel)
            if (c == '\\') c = '/';
        const bool has_dir = rel.find('/') != std::string::npos;
        const bool has_ext = rel.size() > 4 && rel.compare(rel.size() - 4, 4, ".dds") == 0;
        if (!has_dir && !has_ext) rel = "Assets/Textures/" + rel + ".dds";
        else if (!has_ext) rel += ".dds";
        for (const char* r : kRoots) {
            const std::string path = std::string(r) + rel;
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
        return cache_.emplace(name, std::move(icon)).first->second;
    }

private:
    std::unordered_map<std::string, Icon> cache_;
};

inline void icon_box4_texel(const IconAtlas::Icon& ic, int dw, int dh, int x, int y, bool nearest, float rgb[3], float& a) {
    a = 0.0f;
    rgb[0] = rgb[1] = rgb[2] = 0.0f;
    if (nearest || (int(ic.w) <= dw && int(ic.h) <= dh)) {
        const int sx = x * int(ic.w) / dw, sy = y * int(ic.h) / dh;
        const std::size_t si = (std::size_t(sy) * ic.w + sx) * 4;
        a = ic.rgba[si + 3] / 255.0f;
        for (int c = 0; c < 3; ++c) rgb[c] = float(ic.rgba[si + c]);
    } else {
        float wa = 0.0f;
        for (int j = 0; j < 4; ++j)
            for (int k = 0; k < 4; ++k) {
                const int sx = std::min(int(ic.w) - 1, int((float(x) + (float(k) + 0.5f) * 0.25f) * float(ic.w) / float(dw)));
                const int sy = std::min(int(ic.h) - 1, int((float(y) + (float(j) + 0.5f) * 0.25f) * float(ic.h) / float(dh)));
                const std::size_t si = (std::size_t(sy) * ic.w + std::size_t(sx)) * 4;
                const float sa = ic.rgba[si + 3] / 255.0f;
                wa += sa;
                for (int c = 0; c < 3; ++c) rgb[c] += float(ic.rgba[si + c]) * sa;
            }
        a = wa / 16.0f;
        if (wa > 0.0f) for (int c = 0; c < 3; ++c) rgb[c] /= wa;
    }
}

inline void icon_area_texel(const IconAtlas::Icon& ic, int dw, int dh, int x, int y, double col[3], float& a) {
    const double fx0 = double(x) * ic.w / dw, fx1 = double(x + 1) * ic.w / dw;
    const double fy0 = double(y) * ic.h / dh, fy1 = double(y + 1) * ic.h / dh;
    double acc[3] = {0, 0, 0}, wa = 0.0, wsum = 0.0;
    for (int sy = int(fy0); sy < int(ic.h) && double(sy) < fy1; ++sy) {
        const double wy = std::min(fy1, double(sy + 1)) - std::max(fy0, double(sy));
        for (int sx = int(fx0); sx < int(ic.w) && double(sx) < fx1; ++sx) {
            const double w = wy * (std::min(fx1, double(sx + 1)) - std::max(fx0, double(sx)));
            if (w <= 0.0) continue;
            const std::size_t si = (std::size_t(sy) * ic.w + std::size_t(sx)) * 4;
            const double ta = ic.rgba[si + 3] / 255.0;
            for (int c = 0; c < 3; ++c) acc[c] += ic.rgba[si + c] * ta * w;
            wa += ta * w;
            wsum += w;
        }
    }
    if (wsum <= 0.0 || wa <= 0.0) { a = -1.0f; col[0] = col[1] = col[2] = 0.0; return; }
    a = float(wa / wsum);
    for (int c = 0; c < 3; ++c) col[c] = acc[c] / wa;
}

class IconResampleCache {
public:
    const std::vector<float>& box4(const IconAtlas::Icon& ic, int dw, int dh, bool nearest) {
        Layer& l = slot(ic, dw, dh, nearest ? 1 : 0);
        if (l.f.empty() && dw > 0 && dh > 0) {
            ++misses_;
            l.f.resize(std::size_t(dw) * std::size_t(dh) * 4);
            for (int y = 0; y < dh; ++y)
                for (int x = 0; x < dw; ++x) {
                    float* p = &l.f[(std::size_t(y) * std::size_t(dw) + std::size_t(x)) * 4];
                    icon_box4_texel(ic, dw, dh, x, y, nearest, p, p[3]);
                }
        } else {
            ++hits_;
        }
        return l.f;
    }
    const std::vector<double>& area(const IconAtlas::Icon& ic, int dw, int dh) {
        Layer& l = slot(ic, dw, dh, 2);
        if (l.d.empty() && dw > 0 && dh > 0) {
            ++misses_;
            l.d.resize(std::size_t(dw) * std::size_t(dh) * 4);
            for (int y = 0; y < dh; ++y)
                for (int x = 0; x < dw; ++x) {
                    double* p = &l.d[(std::size_t(y) * std::size_t(dw) + std::size_t(x)) * 4];
                    float a = 0.0f;
                    icon_area_texel(ic, dw, dh, x, y, p, a);
                    p[3] = double(a);
                }
        } else {
            ++hits_;
        }
        return l.d;
    }
    void clear() { layers_.clear(); }
    std::size_t size() const { return layers_.size(); }
    std::size_t hits() const { return hits_; }
    std::size_t misses() const { return misses_; }

private:
    struct Layer {
        std::vector<float> f;
        std::vector<double> d;
    };
    struct Key {
        const void* icon;
        const void* pixels;
        std::uint32_t w, h;
        int dw, dh, mode;
        bool operator==(const Key& o) const {
            return icon == o.icon && pixels == o.pixels && w == o.w && h == o.h && dw == o.dw && dh == o.dh && mode == o.mode;
        }
    };
    struct KeyHash {
        std::size_t operator()(const Key& k) const {
            std::size_t h = std::hash<const void*>()(k.icon) ^ (std::hash<const void*>()(k.pixels) << 1);
            h = h * 1000003u ^ (std::size_t(k.w) << 20 ^ std::size_t(k.h) << 8);
            h = h * 1000003u ^ (std::size_t(std::uint32_t(k.dw)) << 16 ^ std::size_t(std::uint32_t(k.dh)) << 2 ^ std::size_t(k.mode));
            return h;
        }
    };
    Layer& slot(const IconAtlas::Icon& ic, int dw, int dh, int mode) {
        if (layers_.size() >= 1024) layers_.clear();
        return layers_[Key{&ic, ic.rgba.data(), ic.w, ic.h, dw, dh, mode}];
    }
    std::unordered_map<Key, Layer, KeyHash> layers_;
    std::size_t hits_ = 0, misses_ = 0;
};

}
