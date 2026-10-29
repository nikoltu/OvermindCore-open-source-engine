#pragma once

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace omc::data {

class Catalog;

struct SoundAsset {
    std::string file;
    std::optional<std::string> volume;
    std::optional<std::string> weight;
};

struct Sound {
    std::string id;
    std::string parent;
    bool is_default = false;
    std::vector<SoundAsset> assets;
    std::optional<std::string> volume;
    std::optional<std::string> category;
    std::optional<std::string> select;
    std::optional<std::string> mode;
    std::optional<std::string> chance;
    std::optional<std::string> pitch;
    int dupe_destroy = 16;
    int dupe_mute = 10;
    bool dupe_prevent = false;
    int resource_priority = 32;
    struct RolloffPoint {
        float distance = 0.0f;
        float volume_db = 0.0f;
    };
    std::vector<RolloffPoint> rolloff;
    std::optional<std::string> rolloff_blend;
    std::vector<std::pair<std::string, std::string>> flags;

    const std::string* flag(const std::string& index) const {
        for (const auto& [k, v] : flags)
            if (k == index) return &v;
        return nullptr;
    }
};

inline float rolloff_gain(const Sound& s, float distance) {
    if (s.rolloff.empty()) return 1.0f;
    std::vector<Sound::RolloffPoint> pts = s.rolloff;
    std::sort(pts.begin(), pts.end(), [](const Sound::RolloffPoint& a, const Sound::RolloffPoint& b) { return a.distance < b.distance; });
    float db;
    if (distance < pts.front().distance) {
        db = 0.0f;
    } else if (distance >= pts.back().distance) {
        db = pts.back().volume_db;
    } else {
        db = pts.back().volume_db;
        for (std::size_t i = 0; i + 1 < pts.size(); ++i)
            if (distance >= pts[i].distance && distance <= pts[i + 1].distance) {
                const float t = (distance - pts[i].distance) / std::max(pts[i + 1].distance - pts[i].distance, 1e-6f);
                db = pts[i].volume_db + (pts[i + 1].volume_db - pts[i].volume_db) * t;
                break;
            }
    }
    if (db <= -96.0f) return 0.0f;
    return std::pow(10.0f, db / 20.0f);
}

struct Soundtrack {
    std::string id;
    std::string master_sound;
    float delay_min = 0.0f, delay_max = 0.0f;
    std::optional<std::string> select;
    std::string next;
    bool continuous = true;
    bool found = false;
};
Soundtrack resolve_soundtrack(const Catalog& cat, const std::string& id);

class AudioCatalog {
public:
    void build(const Catalog& catalog);

    const Sound* sound(const std::string& id) const;
    std::vector<std::string> ids() const;
    size_t size() const { return sounds_.size(); }

    std::vector<std::string> resolve(const std::string& id) const;

private:
    std::unordered_map<std::string, Sound> sounds_;
};

}
