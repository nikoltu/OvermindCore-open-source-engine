#pragma once

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace sc2::data {

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
    std::vector<std::pair<std::string, std::string>> flags;

    const std::string* flag(const std::string& index) const {
        for (const auto& [k, v] : flags)
            if (k == index) return &v;
        return nullptr;
    }
};

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
